// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The desktop program's update notice and settings (lab_self_update_ui.hpp).
#include "lab_self_update_ui.hpp"
#include "lab_game_manager.hpp"
#include "lab_product_ui.hpp"
#include <imgui.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace lab {
namespace {
bool ready(const auto& future){using namespace std::chrono_literals;return future.valid()&&future.wait_for(0ms)==std::future_status::ready;}
void clear_pending(update::Settings& s){s.pending_from.clear();s.pending_to.clear();}
std::string local_time(std::int64_t utc){
    std::time_t t=static_cast<std::time_t>(utc);std::tm local{};if(localtime_s(&local,&t))return {};
    char text[32]{};std::strftime(text,sizeof text,"%Y-%m-%d %H:%M",&local);return text;}
void log_line(const std::string& text){OutputDebugStringW(wide("Overglaze update: "+text+"\n").c_str());}
}

UpdatePanel::UpdatePanel(bool enabled):enabled_(enabled){
    try{running_text_=update::running_version();running_=update::parse_version(running_text_);}
    catch(const std::exception& e){version_error_=e.what();}
    if(!enabled_){store_=std::make_shared<update::SettingsStore>();return;}
    install_=update::detect_self();
    store_=std::make_shared<update::SettingsStore>(install_.data);
    if(!running_)return;
    const auto settings=store_->get();
    // Shown once: the record is cleared as soon as it has been read.
    after_=update::after_update(settings,*running_);
    if(!settings.pending_to.empty()){after_from_=settings.pending_from;after_to_=settings.pending_to;store_->update(clear_pending);}
    notice_=after_!=update::AfterUpdate::none;
    // Setups of versions already installed are no longer needed. Right after a
    // relaunch the Setup may still be running; its folder goes on a later start.
    {const auto data=install_.data;const auto running=*running_;
     cleanup_=std::async(std::launch::async,[data,running]{try{update::cleanup_installed(data,running);}catch(...){}});}
    if(after_==update::AfterUpdate::updated){
        const auto root=install_.root;
        plugin_updates_job_=std::async(std::launch::async,[root]{
            games::Manager manager(root);int count=0;
            for(const auto& entry:manager.list()){const auto status=manager.inspect(entry);if(status.installed&&status.update_available)++count;}
            return count;});
    }
    if(update::automatic_check_due(settings,std::time(nullptr)))start_check(Mode::automatic);
}
UpdatePanel::~UpdatePanel(){check_cancel_->store(true);if(job_)job_->cancel=true;}

void UpdatePanel::start_check(Mode mode){
    if(!enabled_||!running_||checking_.valid()||installing_.valid())return;
    check_mode_=mode;check_message_.clear();
    store_->update([](update::Settings& s){s.last_check=std::time(nullptr);});
    const auto endpoint=update::official(running_text_);const auto running=*running_;const auto cancel=check_cancel_;
    checking_=std::async(std::launch::async,[endpoint,running,cancel]{return update::check(endpoint,running,cancel.get());});
}
void UpdatePanel::start_install(){
    if(!offer_||offer_->state!=update::Offer::State::available||!install_.by_setup||installing_.valid()||checking_.valid())return;
    install_message_.clear();job_=std::make_shared<Job>();
    const auto job=job_;const auto store=store_;const auto offer=*offer_;const auto at=install_;
    const auto endpoint=update::official(running_text_);const auto from=running_text_;
    installing_=std::async(std::launch::async,[job,store,offer,at,endpoint,from]()->std::string{
        job->stage=1;
        if(auto why=update::install_refusal(at,offer.manifest.setup.size);!why.empty())return why;
        job->stage=2;
        const auto setup=update::download_setup(endpoint,offer,at.data,&job->cancel,&job->progress);
        // Again just before the hand-over: a game may have started meanwhile.
        // The space for the download itself is spent by now.
        job->stage=3;
        if(auto why=update::install_refusal(at,0);!why.empty())return why;
        if(job->cancel)throw update::Cancelled();
        job->stage=4;
        store->update([&](update::Settings& s){s.pending_from=from;s.pending_to=offer.version;});
        try{update::launch_setup(setup,offer.manifest.setup,at.root,GetCurrentProcessId());}
        catch(...){store->update(clear_pending);throw;}
        return {};
    });
}
void UpdatePanel::poll(){
    if(ready(checking_)){
        std::string result;
        try{auto offer=checking_.get();
            if(offer.state==update::Offer::State::up_to_date){result="已是最新版本";offer_.reset();}
            else{result=std::string(offer.state==update::Offer::State::available?"发现新版本 ":"发现新版本（需手动下载） ")+offer.version;
                offer_=std::move(offer);dismissed_=false;}
            if(check_mode_==Mode::manual)check_message_=result;}
        catch(const update::Cancelled&){}
        catch(const std::exception& e){
            // Automatic checks fail silently: the reason is logged and kept for 设置 / 更新.
            result=std::string("检查失败：")+e.what();log_line(result);if(check_mode_==Mode::manual)check_message_=result;}
        if(!result.empty())store_->update([&](update::Settings& s){s.last_result=result;});
    }
    if(ready(installing_)){
        try{const auto refusal=installing_.get();if(refusal.empty())exit_=true;else install_message_=refusal;}
        catch(const update::Cancelled&){install_message_="已取消下载。";}
        catch(const std::exception& e){install_message_=e.what();}
        if(!install_message_.empty())log_line(install_message_);
        job_.reset();
    }
    if(ready(plugin_updates_job_)){try{plugin_updates_=plugin_updates_job_.get();}catch(...){plugin_updates_=-1;}}
}
void UpdatePanel::open_page(HWND window,const std::string& url){
    // Only the project's own release pages.
    const std::string page=url.rfind(update::kPagePrefix,0)==0?url:update::kReleasesPage;
    ShellExecuteW(window,L"open",wide(page).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
}
void UpdatePanel::banner(HWND window,float dpi){
    if(!enabled_)return;
    if(notice_){
        ImGui::PushStyleColor(ImGuiCol_ChildBg,product::raised);
        ImGui::BeginChild("##update-notice",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
        if(after_==update::AfterUpdate::updated){
            product::pill("已更新");ImGui::SameLine();ImGui::Text("已更新到 %s",running_text_.c_str());
            if(plugin_updates_job_.valid())ImGui::TextDisabled("正在检查游戏插件…");
            else if(plugin_updates_>0){
                ImGui::TextWrapped("%d 款游戏需要更新插件。请在游戏库里逐个更新，不会自动更新。",plugin_updates_);
                if(product::action("查看游戏库",{150*dpi,38*dpi},true)){open_games_=true;notice_=false;}ImGui::SameLine();}
        }else{
            product::pill("更新未完成",product::amber);ImGui::SameLine();
            ImGui::TextWrapped("上次更新到 %s 没有完成，当前仍是 %s。可以在设置里重新检查，或从下载页手动下载安装程序。",after_to_.c_str(),running_text_.c_str());
            if(ImGui::Button("打开下载页##notice",{150*dpi,38*dpi}))open_page(window,update::kReleasesPage);ImGui::SameLine();
        }
        if(ImGui::Button("知道了",{110*dpi,38*dpi}))notice_=false;
        ImGui::EndChild();ImGui::PopStyleColor();ImGui::Dummy({0,6*dpi});
    }
    if(!offer_||offer_->state==update::Offer::State::up_to_date||dismissed_)return;
    const auto& offer=*offer_;const bool installable=offer.state==update::Offer::State::available&&install_.by_setup;
    ImGui::PushStyleColor(ImGuiCol_ChildBg,product::raised);
    ImGui::BeginChild("##update-offer",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
    ImGui::PushFont(nullptr,20);ImGui::Text("发现新版本 %s",offer.version.c_str());ImGui::PopFont();
    ImGui::SameLine();ImGui::TextDisabled("当前 %s",running_text_.c_str());
    if(offer.state==update::Offer::State::available&&!offer.manifest.notes_zh.empty()){
        const float width=ImGui::GetContentRegionAvail().x;
        const float height=std::min(ImGui::CalcTextSize(offer.manifest.notes_zh.c_str(),nullptr,false,width-32*dpi).y+24*dpi,150*dpi);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,product::surface);
        ImGui::BeginChild("##update-notes",{0,height},ImGuiChildFlags_Borders);ImGui::TextWrapped("%s",offer.manifest.notes_zh.c_str());ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if(installing_.valid()&&job_){
        const int stage=job_->stage.load();
        if(stage==2){
            const auto total=job_->progress.total.load(),received=job_->progress.received.load();
            char label[64];std::snprintf(label,sizeof label,"%.1f / %.1f MiB",received/1048576.0,total/1048576.0);
            ImGui::TextDisabled("正在下载安装程序…");
            ImGui::ProgressBar(total?float(double(received)/double(total)):0.f,{std::min(ImGui::GetContentRegionAvail().x,520*dpi),0},label);
        }else ImGui::TextDisabled("%s",stage<=1?"正在确认可以安装…":stage==3?"安装程序已校验，正在做最后检查…":"正在启动安装程序…");
        ImGui::BeginDisabled(stage>=4||job_->cancel.load());
        if(ImGui::Button("取消",{110*dpi,38*dpi}))job_->cancel=true;
        ImGui::EndDisabled();
    }else{
        if(installable){if(product::action("更新",{120*dpi,40*dpi},true))start_install();ImGui::SameLine();}
        else{if(product::action("打开下载页",{150*dpi,40*dpi},true))open_page(window,offer.page);ImGui::SameLine();}
        if(ImGui::Button("稍后",{100*dpi,40*dpi}))dismissed_=true;
        if(installable){ImGui::SameLine();if(ImGui::Button("打开下载页",{150*dpi,40*dpi}))open_page(window,offer.page);}
        ImGui::PushStyleColor(ImGuiCol_Text,product::muted);
        if(installable)ImGui::TextWrapped("发布签名已校验。更新会关闭釉光，安装完成后自动重新打开；游戏插件不会自动更新。");
        else if(offer.state==update::Offer::State::manual_only)ImGui::TextWrapped("这个版本没有附带签名更新清单，只能从下载页手动更新。");
        else ImGui::TextWrapped("便携版不会自动安装，请从下载页获取新版本。");
        ImGui::PopStyleColor();
        if(!install_message_.empty()){ImGui::PushStyleColor(ImGuiCol_Text,product::amber);ImGui::TextWrapped("%s",install_message_.c_str());ImGui::PopStyleColor();}
    }
    ImGui::EndChild();ImGui::PopStyleColor();ImGui::Dummy({0,6*dpi});
}
void UpdatePanel::settings(HWND window,float dpi){
    ImGui::SeparatorText("更新");
    ImGui::TextDisabled("当前版本 %s · %s",running_text_.empty()?"未知":running_text_.c_str(),!enabled_?"测试模式，不检查更新":install_.by_setup?"安装版":"便携版");
    if(!enabled_)return;
    if(!version_error_.empty())ImGui::TextWrapped("%s",version_error_.c_str());
    const auto settings=store_->get();bool on=settings.check_on_start;
    if(ImGui::Checkbox("启动时检查更新",&on))store_->update([on](update::Settings& s){s.check_on_start=on;});
    ImGui::BeginDisabled(!running_||checking_.valid()||installing_.valid());
    if(ImGui::Button(checking_.valid()?"检查中…":"检查更新",{140*dpi,0}))start_check(Mode::manual);
    ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button("打开下载页##settings"))open_page(window,update::kReleasesPage);
    if(!check_message_.empty())ImGui::TextWrapped("%s",check_message_.c_str());
    if(settings.last_check>0)ImGui::TextDisabled("上次检查 %s%s%s",local_time(settings.last_check).c_str(),settings.last_result.empty()?"":" · ",settings.last_result.c_str());
    if(!install_.by_setup)ImGui::TextDisabled("便携版只提示新版本，不会自动安装（%s）。",install_.why.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text,product::muted);
    ImGui::TextWrapped("检查更新只读取 GitHub 上的公开发布信息，不发送任何关于你、游戏或文件的内容；启动时每 24 小时最多自动检查一次。更新必须带有项目发布密钥的签名，校验不通过不会安装。游戏内组件从不联网。");
    ImGui::PopStyleColor();
    if(const auto error=store_->error();!error.empty()){ImGui::PushStyleColor(ImGuiCol_Text,product::amber);ImGui::TextWrapped("更新设置未保存：%s",error.c_str());ImGui::PopStyleColor();}
}
}
