// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_game_manager_ui.hpp"
#include "lab_game_presentation.hpp"
#include "lab_product_ui.hpp"
#include <imgui.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cctype>
namespace lab {
namespace {
std::string text(const std::filesystem::path& p){return utf8(p.wstring());}
// Muted semantic colours: installation status is never an image-quality claim.
ImVec4 ink(games::Tone t){switch(t){
    case games::Tone::success:return product::accent;case games::Tone::accent:return product::accent;
    case games::Tone::warning:return {.85f,.71f,.48f,1};case games::Tone::error:return {.89f,.57f,.50f,1};
    case games::Tone::neutral:return {.56f,.61f,.64f,1};case games::Tone::info:return {.60f,.71f,.77f,1};}
    return {.56f,.61f,.64f,1};}
constexpr ImVec4 panel=product::surface;
constexpr ImVec4 muted=product::muted;
constexpr ImVec4 foreground=product::text;
void gap(float height){
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{ImGui::GetStyle().ItemSpacing.x,0});
    ImGui::Dummy({0,height});ImGui::PopStyleVar();
}
void muted_text(const char* value){ImGui::PushStyleColor(ImGuiCol_Text,muted);ImGui::TextWrapped("%s",value);ImGui::PopStyleColor();}
void heading(const char* value,float size){ImGui::PushFont(nullptr,size);ImGui::TextWrapped("%s",value);ImGui::PopFont();}
bool primary_button(const char* value,ImVec2 size){
    return product::action(value,size,true);
}
// A drawn controller symbol needs no icon font, downloaded artwork or game assets.
void game_symbol(ImDrawList* draw,ImVec2 at,float size,ImU32 colour){
    const float x=at.x,y=at.y,u=size/24.f;
    draw->AddRect({x+3*u,y+6*u},{x+21*u,y+18*u},colour,4*u,0,1.4f*u);
    draw->AddLine({x+6*u,y+12*u},{x+11*u,y+12*u},colour,1.4f*u);
    draw->AddLine({x+8.5f*u,y+9.5f*u},{x+8.5f*u,y+14.5f*u},colour,1.4f*u);
    draw->AddCircleFilled({x+16*u,y+10*u},1.1f*u,colour);
    draw->AddCircleFilled({x+18*u,y+13*u},1.1f*u,colour);
}
std::string folded(std::string value){for(char& c:value)if(static_cast<unsigned char>(c)<128)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return value;}
bool needs_attention(const games::Presentation& view){
    return view.tone==games::Tone::warning||view.tone==games::Tone::error||
        std::any_of(view.actions.begin(),view.actions.end(),[](const auto& action){return action.primary;});
}
// Words for a code, from the one table (lab_game_reasons.hpp).
std::string words(std::string_view code){const auto* t=games::code_text(code);return t?std::string(t):"["+std::string(code)+"]";}
std::string action_title(const games::Action& action){
    if(action.name=="make-package")return "准备安装";
    if(action.name=="refresh-package")return "更新安装配置";
    return words(action.label);
}
std::string route_title(const std::string& route){
    if(route=="sl-rr"||route=="ngx-rr")return "DLSS 光线重构";
    if(route=="sl-sr"||route=="ngx-sr")return "DLSS 超分辨率";
    return words("route."+route);
}
std::string short_value(const json& v){
    if(v.is_string()){auto s=v.get<std::string>();if(s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos)s=s.substr(0,12)+"…";return s;}
    if(v.is_number_unsigned())return std::to_string(v.get<std::uint64_t>());
    if(v.is_number_integer())return std::to_string(v.get<std::int64_t>());
    if(v.is_array()){std::string out;for(const auto& x:v){if(!out.empty())out+="、";out+=short_value(x);}return out;}
    return {};}
// One three-state check: "? 无法检查" is never drawn as a pass. The
// marks are ASCII because the page's font range has no check-mark glyph.
void draw_check(const games::Check& c){
    const char* mark=c.outcome==games::Outcome::pass?"+":c.outcome==games::Outcome::fail?"x":"?";
    const ImVec4 colour=c.outcome==games::Outcome::pass?ink(games::Tone::success):c.outcome==games::Outcome::fail?ink(games::Tone::warning):ink(games::Tone::info);
    std::string line=words("check."+c.name)+"："+words(std::string("outcome.")+games::outcome_name(c.outcome));
    if(const auto v=short_value(c.value);!v.empty())line+=" · "+v;
    if(c.outcome==games::Outcome::unknown&&!c.reason.empty())line+="（"+games::render(c.reason,c.value.is_object()?c.value:json{{"value",c.value}})+"）";
    ImGui::TextColored(colour,"%s",mark);ImGui::SameLine();ImGui::TextWrapped("%s",line.c_str());}
std::string launch_text(const games::Status& s){for(const auto& r:s.reasons)if(r.code.rfind("launch-",0)==0)return games::render(r);return {};}
std::vector<games::Status> rows(games::Manager& manager){std::vector<games::Status> all;for(auto& e:manager.list())all.push_back(manager.inspect(e));return all;}
std::string pick(HWND w){const auto result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;std::string path;
    if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))){dialog->SetOptions(FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);dialog->SetTitle(L"选择游戏安装目录");if(SUCCEEDED(dialog->Show(w))){Microsoft::WRL::ComPtr<IShellItem> item;PWSTR value=nullptr;if(SUCCEEDED(dialog->GetResult(&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&value))){path=utf8(value);CoTaskMemFree(value);}}}
    dialog.Reset();if(SUCCEEDED(result))CoUninitialize();return path;}
// The control ids the page has always reported, one per action.
const char* control_id(const std::string& a){
    if(a=="make-package")return "games.makepackage";if(a=="refresh-package")return "games.refreshpackage";if(a=="install")return "games.install";
    if(a=="update")return "games.update";if(a=="uninstall")return "games.uninstall";if(a=="repin")return "games.repin";
    if(a=="forget")return "games.forget";if(a=="open-folder")return "games.folder";return "games.action";}
}
GameManagerPage::GameManagerPage(std::filesystem::path root,bool known):root_(std::move(root)){refresh(known);}
void GameManagerPage::mark(const char* id){const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();controls[id]={a.x,a.y,b.x-a.x,b.y-a.y};}
void GameManagerPage::refresh(bool known){if(busy())return;error_.clear();
    {std::lock_guard guard(live_->lock);live_->text.clear();live_->index=live_->count=0;}
    const auto root=root_;job_=std::async(std::launch::async,[root,known]{games::Manager m(root);if(known)m.import_known_installations();Result r;r.rows=rows(m);r.storage=m.storage_usage();r.has_storage=true;return r;});}
void GameManagerPage::act(const std::string& action,const std::string& id){if(busy())return;error_.clear();notice_.clear();const auto root=root_;const auto live=live_;
    {std::lock_guard guard(live->lock);live->text.clear();live->index=live->count=0;}
    job_=std::async(std::launch::async,[root,action,id,live]{games::Manager m(root);Result r;
    // Progress: the stage now running, in the table's words.
    const games::Progress progress=[live](const games::ProgressEvent& ev){if(ev.status!="start")return;
        std::string t=words("operation."+ev.operation)+" › "+words("stage."+ev.operation+"."+ev.stage);if(!ev.parent.empty())t=words("operation."+ev.parent)+" › "+t;
        std::lock_guard guard(live->lock);live->text=std::move(t);live->index=ev.index;live->count=ev.count;};
    auto after=[&]{for(const auto& e:m.list())if(e.id==id)return m.inspect(e);return games::Status{};};
    // The success message says how THIS game is started, from its load mode.
    if(action=="install"){m.install(id,true,true,"confirmed in the viewer dialog: offline single-player, no anti-cheat, install allowed",progress);r.message=games::render("install-done")+" "+launch_text(after());}
    else if(action=="update"){m.update(id,true,true,"confirmed in the viewer dialog: offline single-player, no anti-cheat, update allowed",progress);r.message=games::render("update-done")+" "+launch_text(after());}
    else if(action=="repin"){const auto retired=m.repin(id,true,true,"confirmed in the viewer dialog: offline single-player, no anti-cheat, re-adapt and install allowed",false,progress);
        r.message=games::render("repin-done",json{{"retired",text(retired)}})+" "+launch_text(after());}
    // The controller's root proxy, never the research root layout (see PackageOptions).
    else if(action=="make-package"){const auto p=m.make_package(id,games::PackageOptions::controller_root_proxy());r.message=games::render("package-made-controller",json{{"package",p.name},{"route",p.route}});}
    else if(action=="refresh-package"){const auto p=m.refresh_package(id);r.message=games::render("package-refreshed",json{{"package",p.name}});}
    else if(action=="uninstall"){m.uninstall(id,true,progress);r.message=games::render("uninstall-done");}
    else if(action=="forget"){m.forget(id);r.message=games::render("forgotten");}
    else throw std::runtime_error("Unknown manager action");r.rows=rows(m);r.storage=m.storage_usage();r.has_storage=true;return r;});}
void GameManagerPage::poll(){using namespace std::chrono_literals;if(!busy()||job_.wait_for(0ms)!=std::future_status::ready)return;
    std::string failed;
    try{auto result=job_.get();if(result.discovering){found_=std::move(result.found);candidate_=0;}else{rows_=std::move(result.rows);if(result.has_storage)storage_=std::move(result.storage);if(!result.selection.empty())selected_=std::move(result.selection);else if(selected_.empty()&&!rows_.empty())selected_=rows_.front().entry.id;}if(!result.message.empty())notice_=std::move(result.message);}
    // A write operation that stopped part-way says where it stopped and where it
    // left the game (an update whose install failed: not installed). The list is
    // then re-read so it shows that state rather than the one before.
    catch(const games::OperationError& e){failed=games::render(e.reason);}
    catch(const std::exception& e){error_=e.what();}
    if(!failed.empty()){refresh();error_=std::move(failed);}}
void GameManagerPage::draw(HWND window,float dpi){
    poll();controls=json::object();
    const float unit=ImGui::GetFrameHeight();
    const bool working=busy();
    auto begin_add=[&]{show_add_=true;found_={};*path_=0;error_.clear();};
    auto tooltip=[&](const games::Action& action){
        if(!action.enabled&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s",games::render(action.why_not).c_str());
    };
    auto invoke=[&](const games::Action& action,const games::Status& status){
        if(action.name=="open-folder"){
            const auto result=ShellExecuteW(window,L"explore",status.entry.exe.parent_path().c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            if(reinterpret_cast<INT_PTR>(result)<=32)error_="Windows 无法打开游戏目录";
        }else if(action.name=="make-package"||action.name=="refresh-package"){
            act(action.name,action.name=="refresh-package"?status.package:status.entry.id);
        }else{
            pending_=status.entry.id;pending_title_=status.entry.title;pending_path_=text(status.entry.exe);
            operation_=action.name;approved_=false;ImGui::OpenPopup("确认插件操作");
        }
    };

    // The first screen is the library and the next available action. All detailed
    // evidence remains accessible further down; the backend owns every status.
    ImGui::BeginGroup();
    heading("游戏库",28);
    muted_text("管理插件安装，在游戏内调节画面。");
    ImGui::EndGroup();
    ImGui::SameLine((std::max)(ImGui::GetCursorPosX(),ImGui::GetWindowContentRegionMax().x-242*dpi));
    ImGui::BeginDisabled(working);
    if(ImGui::Button("重新检查",{108*dpi,40*dpi}))refresh();mark("games.refresh");
    ImGui::SameLine(0,10*dpi);
    if(primary_button("+ 添加游戏",{124*dpi,40*dpi}))begin_add();mark("games.add");
    ImGui::EndDisabled();
    gap(18*dpi);

    if(working){
        std::string stage;unsigned index=0,total=0;
        {std::lock_guard guard(live_->lock);stage=live_->text;index=live_->index;total=live_->count;}
        ImGui::PushStyleColor(ImGuiCol_Text,ink(games::Tone::accent));
        ImGui::TextUnformatted(stage.empty()?"正在读取游戏信息…":stage.c_str());ImGui::PopStyleColor();
        if(total){ImGui::SameLine();ImGui::TextDisabled("%u / %u",index,total);}
        // Only genuine backend stages are shown; no invented percentage.
        gap(8*dpi);
    }
    if(!error_.empty()){
        ImGui::TextColored(ink(games::Tone::error),"操作未完成");
        ImGui::SameLine();
        if(ImGui::SmallButton("收起##games.error"))error_.clear();
        ImGui::TextWrapped("%s",error_.c_str());gap(8*dpi);
    }
    if(!notice_.empty()){
        ImGui::TextColored(ink(games::Tone::success),"操作已完成");
        ImGui::SameLine();if(ImGui::SmallButton("收起##games.notice"))notice_.clear();
        ImGui::TextWrapped("%s",notice_.c_str());gap(8*dpi);
    }

    const float available=ImGui::GetContentRegionAvail().x;
    const float library_width=(std::clamp)(available*.30f,265*dpi,345*dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,12*dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{16*dpi,16*dpi});
    ImGui::PushStyleColor(ImGuiCol_ChildBg,panel);
    ImGui::BeginChild("games.list",{library_width,0},ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextUnformatted("我的游戏");ImGui::SameLine();ImGui::TextDisabled("%zu",rows_.size());
    gap(10*dpi);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##games.search","搜索名称或路径",search_,sizeof(search_));mark("games.search");
    gap(8*dpi);
    const char* filters[]{"全部","已安装","需处理"};
    const float filter_width=(ImGui::GetContentRegionAvail().x-10*dpi)/3.f;
    for(int i=0;i<3;++i){
        if(i)ImGui::SameLine(0,5*dpi);
        const bool active=filter_==i;
        ImGui::PushStyleColor(ImGuiCol_Button,active?ImVec4{.23f,.29f,.20f,1}:ImVec4{.12f,.15f,.17f,1});
        ImGui::PushStyleColor(ImGuiCol_Text,active?ink(games::Tone::accent):muted);
        if(ImGui::Button(filters[i],{filter_width,unit}))filter_=i;
        const std::string id="games.filter."+std::to_string(i);mark(id.c_str());
        ImGui::PopStyleColor(2);
    }
    gap(12*dpi);
    const std::string query=folded(search_);
    std::vector<const games::Status*> visible;
    for(const auto& status:rows_){
        const auto view=games::present(status,working);
        if(filter_==1&&!status.installed)continue;
        if(filter_==2&&!needs_attention(view))continue;
        if(!query.empty()&&folded(status.entry.title+" "+text(status.entry.exe)).find(query)==std::string::npos)continue;
        visible.push_back(&status);
    }
    if(!visible.empty()&&std::none_of(visible.begin(),visible.end(),[&](const auto* s){return s->entry.id==selected_;}))
        selected_=visible.front()->entry.id;
    ImGui::BeginChild("games.rows",{0,0},ImGuiChildFlags_None);
    for(const auto* status:visible){
        const auto& s=*status;const auto view=games::present(s,working);const bool selected=s.entry.id==selected_;
        ImGui::PushID(s.entry.id.c_str());
        const ImVec2 at=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x,height=78*dpi;
        ImGui::PushStyleColor(ImGuiCol_Header,{.18f,.23f,.19f,1});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,{.16f,.20f,.21f,1});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,{.20f,.26f,.22f,1});
        if(ImGui::Selectable("##game",selected,ImGuiSelectableFlags_None,{width,height}))selected_=s.entry.id;
        const std::string id="games.row."+s.entry.id;mark(id.c_str());
        const bool hovered=ImGui::IsItemHovered();ImGui::PopStyleColor(3);
        auto* draw=ImGui::GetWindowDrawList();
        if(selected)draw->AddRectFilled({at.x,at.y+12*dpi},{at.x+3*dpi,at.y+height-12*dpi},ImGui::GetColorU32(ink(games::Tone::accent)),2*dpi);
        const ImVec2 icon{at.x+12*dpi,at.y+19*dpi};
        draw->AddRectFilled(icon,{icon.x+36*dpi,icon.y+36*dpi},IM_COL32(41,49,51,255),9*dpi);
        game_symbol(draw,{icon.x+6*dpi,icon.y+6*dpi},24*dpi,ImGui::GetColorU32(selected?ink(games::Tone::accent):muted));
        const ImVec4 clip{at.x+60*dpi,at.y,at.x+width-10*dpi,at.y+height};
        const float font=ImGui::GetFontSize();
        draw->AddText(ImGui::GetFont(),font,{clip.x,at.y+16*dpi},ImGui::GetColorU32(foreground),s.entry.title.c_str(),nullptr,0,&clip);
        const std::string label=words(view.label)+(s.running?" · 运行中":view.dot?" · 新版本":"");
        draw->AddText(ImGui::GetFont(),font*.86f,{clip.x,at.y+44*dpi},ImGui::GetColorU32(ink(view.tone)),label.c_str(),nullptr,0,&clip);
        if(hovered)ImGui::SetTooltip("%s\n%s",s.entry.title.c_str(),text(s.entry.exe).c_str());
        gap(4*dpi);ImGui::PopID();
    }
    if(visible.empty()){
        gap(18*dpi);
        muted_text(working&&rows_.empty()?"正在读取你的游戏库…":rows_.empty()?"还没有添加游戏":"没有符合条件的游戏");
        gap(7*dpi);
        muted_text(rows_.empty()?"从一个游戏目录开始。":"试试其他名称，或切换到「全部」。");
        if(!rows_.empty()&&ImGui::Button("清除筛选")){*search_=0;filter_=0;}
    }
    ImGui::EndChild();ImGui::EndChild();
    ImGui::PopStyleColor();ImGui::PopStyleVar(2);
    ImGui::SameLine(0,18*dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{24*dpi,20*dpi});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,12*dpi);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,panel);
    ImGui::BeginChild("games.detail",{0,0},ImGuiChildFlags_AlwaysUseWindowPadding);
    const games::Status* chosen=nullptr;
    for(const auto* status:visible)if(status->entry.id==selected_)chosen=status;
    if(chosen){
        const auto& s=*chosen;const auto view=games::present(s,working);
        product::pill(words(view.label).c_str(),ink(view.tone));
        if(s.running){ImGui::SameLine();ImGui::TextDisabled(" · 游戏运行中");}
        gap(6*dpi);heading(s.entry.title.c_str(),30);gap(5*dpi);
        std::string context;
        if(s.preflight.is_object())context=words("store."+s.preflight.value("store","unknown"));
        const std::string route=s.route.empty()&&s.preflight.is_object()?s.preflight.value("route","none"):s.route;
        if(!route.empty()){if(!context.empty())context+="  /  ";context+=route_title(route);}
        if(!context.empty())muted_text(context.c_str());
        gap(14*dpi);
        // These three preparation states get concise navigation copy. The
        // complete backend reasons, including refusals, remain in the fold.
        if(s.state=="needs-package")muted_text("已找到游戏的 DLSS 接入方式。先准备安装配置，再确认安装。");
        else if(s.state=="package-stale")muted_text("本地安装配置需要更新，更新后可继续安装。");
        else if(s.state=="available")muted_text("已识别当前游戏版本。安装前请先关闭游戏。");
        else if(!s.reasons.empty())ImGui::TextWrapped("%s",games::render(s.reasons.front()).c_str());
        if(s.running){gap(6*dpi);ImGui::TextColored(ink(games::Tone::warning),"%s",games::render("game-running").c_str());}
        gap(17*dpi);
        bool action_drawn=false;
        for(const auto& action:view.actions){
            if(!action.primary)continue;
            ImGui::BeginDisabled(!action.enabled);
            const bool clicked=primary_button(action_title(action).c_str(),{(std::min)(240*dpi,ImGui::GetContentRegionAvail().x),44*dpi});
            ImGui::EndDisabled();mark(control_id(action.name));tooltip(action);
            if(clicked)invoke(action,s);
            if(!action.enabled){gap(5*dpi);muted_text(games::render(action.why_not).c_str());}
            action_drawn=true;
        }
        if(action_drawn)gap(10*dpi);
        bool action_row=false;
        for(const auto& action:view.actions){
            if(action.primary)continue;
            const std::string label=action_title(action);
            const float button_width=(std::max)(100*dpi,ImGui::CalcTextSize(label.c_str()).x+24*dpi);
            const float right=ImGui::GetWindowPos().x+ImGui::GetWindowContentRegionMax().x;
            if(action_row&&right-ImGui::GetItemRectMax().x>button_width+10*dpi)ImGui::SameLine(0,10*dpi);
            ImGui::BeginDisabled(!action.enabled);
            const bool clicked=ImGui::Button(label.c_str(),{button_width,36*dpi});ImGui::EndDisabled();
            mark(control_id(action.name));tooltip(action);if(clicked)invoke(action,s);action_row=true;
        }
        gap(22*dpi);ImGui::Separator();gap(15*dpi);

        ImGui::TextDisabled("游戏位置");
        ImGui::SameLine();
        if(ImGui::SmallButton("复制路径"))ImGui::SetClipboardText(text(s.entry.exe).c_str());mark("games.copy-path");
        muted_text(text(s.entry.exe).c_str());
        const bool has_install_path=s.installed||s.state=="available"||s.state=="needs-package"||s.state=="package-stale";
        if(has_install_path){
            gap(20*dpi);ImGui::TextUnformatted("进入游戏后");gap(7*dpi);
            const std::string launch=launch_text(s);
            if(s.load_mode=="root_proxy_d3d12")muted_text("照常从商店启动游戏，进图后按 Insert 打开面板。");
            else if(s.load_mode=="root_proxy_on_insert")muted_text("照常启动游戏。第一次按 Insert 时，插件会加载并打开面板。");
            else if(!launch.empty())ImGui::TextWrapped("%s",launch.c_str());
            else muted_text("完成安装后，启动游戏并开启 DLSS 光线重构、超分辨率或 DLAA。");
            gap(8*dpi);
            product::pill("Insert",muted);ImGui::SameLine();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("打开游戏内面板");
            gap(4*dpi);muted_text("NR 每次启动时默认关闭。开启后可调节效果，并用分屏比较画面。");
        }
        gap(20*dpi);ImGui::Separator();gap(12*dpi);

        const bool show_checks=ImGui::CollapsingHeader("检查明细");mark("games.check-details");
        if(show_checks){
            gap(5*dpi);
            muted_text("只读检查；「无法检查」不代表通过。");
            if(s.preflight.is_object()){
                const auto& pf=s.preflight;
                ImGui::TextWrapped("商店：%s · 路线：%s",words("store."+pf.value("store","unknown")).c_str(),words("route."+pf.value("route","none")).c_str());
                ImGui::TextDisabled("检查结果：%s",pf.value("verdict","unknown").c_str());
                if(pf.contains("modules")&&pf["modules"].is_object()){
                    gap(5*dpi);ImGui::TextDisabled("图形模块");
                    for(auto it=pf["modules"].begin();it!=pf["modules"].end();++it){
                        ImGui::TextWrapped("%s  %s · %s",it.key().c_str(),it.value().value("version","").c_str(),
                            it.value().value("signature","")=="ValidCachedTrust"?"签名已验证":"签名未验证");
                    }
                }
            }
            gap(7*dpi);for(const auto& check:s.checks)draw_check(check);
            if(!s.reasons.empty()){gap(10*dpi);ImGui::TextDisabled("状态说明");for(const auto& reason:s.reasons)ImGui::TextWrapped("%s",games::render(reason).c_str());}
            gap(8*dpi);
        }
        const bool show_install=ImGui::CollapsingHeader("安装与存储");mark("games.install-details");
        if(show_install){
            gap(5*dpi);
            if(!s.package.empty())ImGui::TextWrapped("适配包：%s",s.package.c_str());
            if(!s.load_mode.empty())ImGui::TextWrapped("加载方式：%s",words("load-mode."+s.load_mode).c_str());
            ImGui::TextWrapped("宿主 SHA-256：%s",s.host_hash.empty()?"未安装 / 未读取":s.host_hash.c_str());
            if(!s.host_hash.empty()&&ImGui::SmallButton("复制 SHA-256"))ImGui::SetClipboardText(s.host_hash.c_str());
            ImGui::TextDisabled("状态 %s · 安装 %s",s.state.c_str(),s.install_state.c_str());
            ImGui::TextDisabled("兼容 %s · 健康检查 %s",s.compatibility.c_str(),s.health.c_str());
            for(const auto& r:s.reasons)if(r.params.is_object()&&r.params.contains("message"))ImGui::TextWrapped("原始信息：%s",r.params.value("message","").c_str());
            gap(8*dpi);
            muted_text("仅管理已登记的 Overglaze 文件。卸载按记录的 SHA-256 校验，并先保存恢复副本。");
            for(const auto& g:storage_.games)if(g.id==s.entry.id){
                ImGui::TextWrapped("恢复副本 %llu 份 · %.1f MiB%s",static_cast<unsigned long long>(g.recovery_copies),double(g.recovery_bytes)/1048576.0,g.complete?"":"（统计不完整）");
                ImGui::TextWrapped("其中 %llu 份按最近 %zu 份的规则管理。",static_cast<unsigned long long>(g.managed_recovery_copies),games::kRecoveryCopiesKept);
                ImGui::TextWrapped("暂存目录 %llu 个 · %.1f MiB",static_cast<unsigned long long>(g.staging_directories),double(g.staging_bytes)/1048576.0);
                ImGui::TextWrapped("数据盘可用 %.1f GiB · 安装预留 %llu GiB",double(storage_.data_disk_available)/1073741824.0,static_cast<unsigned long long>(storage_.data_disk_reserve>>30));
            }
            gap(8*dpi);
        }
        if(ImGui::CollapsingHeader("使用范围")){
            muted_text("仅用于离线单人、无反作弊的 DX12 游戏，需要游戏本身提供 DLSS。桌面程序不安装带 Denuvo 标记的游戏；已支持的被动共存方案由命令行显式选择。");
            gap(6*dpi);muted_text("适配检查不代表画质或性能保证。游戏更新后请重新检查；无法自动接入的游戏需要单独配置。");
        }
    }else{
        gap((std::max)(28*dpi,ImGui::GetContentRegionAvail().y*.18f));
        const ImVec2 at=ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(at,{at.x+64*dpi,at.y+64*dpi},IM_COL32(36,45,44,255),16*dpi);
        game_symbol(ImGui::GetWindowDrawList(),{at.x+12*dpi,at.y+12*dpi},40*dpi,ImGui::GetColorU32(ink(games::Tone::accent)));
        ImGui::Dummy({64*dpi,64*dpi});gap(18*dpi);
        heading(working&&rows_.empty()?"正在整理游戏库":rows_.empty()?"从你想玩的游戏开始":"找到你的下一款游戏",25);
        gap(9*dpi);
        muted_text(working&&rows_.empty()?"正在读取已登记游戏的安装状态。":rows_.empty()?"添加游戏目录，检查接入条件，再按提示完成安装。":"调整左侧搜索或筛选，选择游戏查看安装状态。");
        if(rows_.empty()&&!working){gap(18*dpi);if(primary_button("添加第一个游戏",{185*dpi,44*dpi}))begin_add();mark("games.add-empty");}
        gap(24*dpi);muted_text("本地运行 · 无需账号 · NR 默认关闭");
    }

    ImGui::SetNextWindowSize({(std::min)(650*dpi,ImGui::GetIO().DisplaySize.x-40*dpi),0},ImGuiCond_Appearing);
    if(ImGui::BeginPopupModal("确认插件操作",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        heading(operation_=="install"?"安装到这个游戏":operation_=="update"?"更新插件":operation_=="repin"?"重新适配游戏":operation_=="uninstall"?"卸载插件":"移出游戏库",23);
        gap(12*dpi);
        heading(pending_title_.c_str(),19);muted_text(pending_path_.c_str());
        gap(12*dpi);ImGui::Separator();gap(12*dpi);
        ImGui::TextWrapped("%s",operation_=="install"?"将安装已校验的 Overglaze 文件。请先退出游戏。安装可能影响游戏启动或性能。":
            operation_=="update"?"先保存恢复副本并卸载旧插件，再校验适配包并安装新版本。中途失败时游戏可能回到未安装状态，结果会说明停在哪一步。请先退出游戏。":
            operation_=="repin"?"重新检查游戏版本和模块，保存恢复副本并卸载旧插件。旧适配包保留归档；为当前版本生成适配包并重新安装。请先退出游戏。":
            operation_=="uninstall"?"仅移除登记且 SHA-256 未改变的 Overglaze 文件，先保存恢复副本。游戏文件、已有模型和采集数据保留。":
            "仅移除游戏库记录。游戏和采集数据保留。");
        const bool writes=operation_=="install"||operation_=="update"||operation_=="repin";
        gap(12*dpi);
        // User consent is explicit and bound to the named path. A marker scan
        // cannot certify the absence of anti-cheat; backend gates still apply.
        if(writes){
            muted_text("预检不能证明没有反作弊。请仅在确认以下条件后继续。");
            gap(8*dpi);
        }
        ImGui::Checkbox(writes?"我确认这是离线单人、无反作弊的游戏，并允许此次操作":
            operation_=="uninstall"?"确认卸载上述游戏的 Overglaze 插件":"确认移除上述游戏的列表记录",&approved_);
        mark("games.approve");gap(16*dpi);
        ImGui::BeginDisabled(!approved_||busy());
        if(primary_button("确认执行",{150*dpi,40*dpi})){act(operation_,pending_);ImGui::CloseCurrentPopup();}
        mark("games.confirm");ImGui::EndDisabled();ImGui::SameLine(0,10*dpi);
        if(ImGui::Button("取消",{100*dpi,40*dpi}))ImGui::CloseCurrentPopup();mark("games.cancel");
        ImGui::EndPopup();
    }
    ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar(2);

    if(show_add_){ImGui::OpenPopup("添加游戏目录");show_add_=false;}
    ImGui::SetNextWindowSize({(std::min)(720*dpi,ImGui::GetIO().DisplaySize.x-40*dpi),0},ImGuiCond_Appearing);
    if(ImGui::BeginPopupModal("添加游戏目录",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        heading("添加一款游戏",24);gap(8*dpi);
        muted_text("选择安装目录，或粘贴游戏 EXE 路径。先检查，再由你决定安装。");
        gap(16*dpi);ImGui::TextUnformatted("游戏位置");
        ImGui::BeginDisabled(busy());
        ImGui::SetNextItemWidth((std::max)(100*dpi,ImGui::GetContentRegionAvail().x-118*dpi));
        if(ImGui::InputTextWithHint("##gamepath","粘贴游戏文件夹或 EXE 的完整路径",path_,sizeof(path_))){found_={};candidate_=0;}mark("games.path");
        ImGui::SameLine(0,8*dpi);if(ImGui::Button("选择目录",{110*dpi,unit})){const auto p=pick(window);if(!p.empty()&&p.size()<sizeof(path_)){strcpy_s(path_,p.c_str());found_={};candidate_=0;}}mark("games.browse");
        gap(10*dpi);
        ImGui::BeginDisabled(!*path_);
        if(ImGui::Button("检查路径",{140*dpi,38*dpi})){
            error_.clear();found_={};const auto path=std::filesystem::path(wide(path_));
            {std::lock_guard guard(live_->lock);live_->text.clear();live_->index=live_->count=0;}
            job_=std::async(std::launch::async,[path]{Result r;r.discovering=true;r.found=games::discover(path);return r;});
        }
        mark("games.checkpath");ImGui::EndDisabled();ImGui::EndDisabled();
        if(busy()){gap(8*dpi);muted_text("正在检查路径…");}
        if(!error_.empty()){gap(8*dpi);ImGui::TextColored(ink(games::Tone::error),"检查未完成");ImGui::TextWrapped("%s",error_.c_str());}
        if(!found_.notice.empty()){gap(8*dpi);ImGui::TextWrapped("%s",found_.notice.c_str());}
        if(!found_.executables.empty()){
            gap(14*dpi);ImGui::TextUnformatted("选择实际游戏程序");muted_text("选择游戏本体，而不是启动器。");gap(6*dpi);
            ImGui::BeginChild("game.exe.candidates",{0,145*dpi},ImGuiChildFlags_Borders);
            for(std::size_t i=0;i<found_.executables.size();++i){
                ImGui::PushID(static_cast<int>(i));
                if(ImGui::Selectable(text(found_.executables[i]).c_str(),candidate_==int(i)))candidate_=int(i);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        gap(18*dpi);ImGui::Separator();gap(14*dpi);
        ImGui::BeginDisabled(busy()||found_.executables.empty()||!found_.complete);
        if(primary_button("添加到游戏库",{165*dpi,40*dpi})){
            const auto root=root_,exe=found_.executables.at(candidate_);
            job_=std::async(std::launch::async,[root,exe]{games::Manager m(root);const auto added=m.add(exe);Result r;r.selection=added.id;r.rows=rows(m);r.storage=m.storage_usage();r.has_storage=true;r.message=games::render("registered");return r;});
            selected_.clear();*search_=0;filter_=0;ImGui::CloseCurrentPopup();
        }
        mark("games.addconfirm");ImGui::EndDisabled();ImGui::SameLine(0,10*dpi);
        if(ImGui::Button("取消",{100*dpi,40*dpi}))ImGui::CloseCurrentPopup();mark("games.addcancel");ImGui::EndPopup();
    }
}
}
