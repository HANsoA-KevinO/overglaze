// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_game_manager_ui.hpp"
#include "lab_game_presentation.hpp"
#include <imgui.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
namespace lab {
namespace {
std::string text(const std::filesystem::path& p){return utf8(p.wstring());}
// Colour meaning in the page's existing colours; neutral grey and
// information blue are the only two added.
ImVec4 ink(games::Tone t){switch(t){
    case games::Tone::success:return {.60f,.80f,.22f,1};case games::Tone::accent:return {.72f,.82f,.62f,1};
    case games::Tone::warning:return {.83f,.67f,.40f,1};case games::Tone::error:return {1,.60f,.35f,1};
    case games::Tone::neutral:return {.62f,.64f,.66f,1};case games::Tone::info:return {.55f,.72f,.86f,1};}
    return {.62f,.64f,.66f,1};}
// Words for a code, from the one table (lab_game_reasons.hpp).
std::string words(std::string_view code){const auto* t=games::code_text(code);return t?std::string(t):"["+std::string(code)+"]";}
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
void GameManagerPage::refresh(bool known){if(busy())return;error_.clear();const auto root=root_;job_=std::async(std::launch::async,[root,known]{games::Manager m(root);if(known)m.import_known_installations();Result r;r.rows=rows(m);r.storage=m.storage_usage();r.has_storage=true;return r;});}
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
    try{auto result=job_.get();if(result.discovering){found_=std::move(result.found);candidate_=0;}else{rows_=std::move(result.rows);if(result.has_storage)storage_=std::move(result.storage);if(selected_.empty()&&!rows_.empty())selected_=rows_.front().entry.id;}if(!result.message.empty())notice_=std::move(result.message);}
    // A write operation that stopped part-way says where it stopped and where it
    // left the game (an update whose install failed: not installed). The list is
    // then re-read so it shows that state rather than the one before.
    catch(const games::OperationError& e){failed=games::render(e.reason);}
    catch(const std::exception& e){error_=e.what();}
    if(!failed.empty()){refresh();error_=std::move(failed);}}
void GameManagerPage::draw(HWND window,float dpi){poll();controls=json::object();
    // No store is assumed here; each game's own text says how it starts.
    ImGui::Spacing();ImGui::TextUnformatted("游戏管理");ImGui::SameLine();ImGui::TextDisabled("   安装一次，以后照常启动游戏；每款游戏怎么启动见右侧说明");
    ImGui::SameLine(ImGui::GetWindowWidth()-245*dpi);ImGui::BeginDisabled(busy());
    if(ImGui::Button("添加游戏",{110*dpi,0})){show_add_=true;found_={};*path_=0;error_.clear();}mark("games.add");ImGui::SameLine();if(ImGui::Button("重新检查"))refresh();mark("games.refresh");ImGui::EndDisabled();
    ImGui::Spacing();
    if(busy()){std::string stage;unsigned index=0,total=0;{std::lock_guard guard(live_->lock);stage=live_->text;index=live_->index;total=live_->count;}
        if(stage.empty())ImGui::TextDisabled("正在检查文件 / 执行操作… 不启动游戏，不加载游戏 DLL。");
        else ImGui::TextDisabled("正在执行：%s（%u/%u）",stage.c_str(),index,total);}
    if(!error_.empty()){ImGui::TextColored({1,.60f,.35f,1},"操作未完成");ImGui::TextWrapped("%s",error_.c_str());}
    if(!notice_.empty())ImGui::TextWrapped("%s",notice_.c_str());
    ImGui::BeginChild("games.list",{295*dpi,0},ImGuiChildFlags_Borders);ImGui::TextDisabled("%zu 个游戏",rows_.size());ImGui::Spacing();
    for(const auto& s:rows_){const auto view=games::present(s,busy());ImGui::PushID(s.entry.id.c_str());if(ImGui::Selectable(s.entry.title.c_str(),selected_==s.entry.id,0,{0,30*dpi}))selected_=s.entry.id;
        ImGui::TextColored(ink(view.tone),"%s%s%s",words(view.label).c_str(),view.dot?"  ·  新版本":"",s.running?"  /  游戏运行中":"");ImGui::TextDisabled("%s",text(s.entry.exe.filename()).c_str());ImGui::Spacing();ImGui::Separator();ImGui::PopID();}
    if(rows_.empty()){ImGui::TextWrapped("添加游戏目录，检查可用的 NR 接入配置。");ImGui::Spacing();ImGui::TextDisabled("不扫描整块磁盘。\n不自动下载或改动游戏。");}ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("games.detail",{0,0});const games::Status* chosen=nullptr;for(const auto& s:rows_)if(s.entry.id==selected_)chosen=&s;
    if(chosen){const auto& s=*chosen;const auto view=games::present(s,busy());
        ImGui::Spacing();ImGui::TextUnformatted(s.entry.title.c_str());ImGui::SameLine();ImGui::TextColored(ink(view.tone),"  %s",words(view.label).c_str());
        if(view.dot){ImGui::SameLine();const auto at=ImGui::GetCursorScreenPos();const float r=4*dpi;ImGui::GetWindowDrawList()->AddCircleFilled({at.x+r,at.y+ImGui::GetTextLineHeight()*.5f},r,ImGui::GetColorU32(ink(games::Tone::accent)));ImGui::Dummy({2*r+2,ImGui::GetTextLineHeight()});}
        ImGui::Spacing();ImGui::TextWrapped("%s",text(s.entry.exe).c_str());
        ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
        // What the backend reported, as codes, in the table's words.
        for(const auto& r:s.reasons)ImGui::TextWrapped("%s",games::render(r).c_str());
        if(s.running)ImGui::TextColored(ink(games::Tone::warning),"%s",games::render("game-running").c_str());
        if(s.preflight.is_object()){const auto& pf=s.preflight;ImGui::Spacing();ImGui::TextDisabled("只读预检");
            ImGui::TextWrapped("商店：%s · 路线：%s · 判定：%s",words("store."+pf.value("store","unknown")).c_str(),words("route."+pf.value("route","none")).c_str(),pf.value("verdict","?").c_str());
            std::string mods;if(pf.contains("modules"))for(auto it=pf["modules"].begin();it!=pf["modules"].end();++it)mods+=(mods.empty()?"":"  ")+it.key()+" "+it.value().value("version","")+(it.value().value("signature","")=="ValidCachedTrust"?"":" (未签名)");
            if(!mods.empty())ImGui::TextWrapped("模块：%s",mods.c_str());
            // Every preflight item three-state: "? 无法检查" is shown as such.
            for(const auto& c:s.checks)draw_check(c);}
        if(!s.package.empty()){ImGui::Spacing();ImGui::TextDisabled("适配包：%s · 路线：%s%s%s",s.package.c_str(),s.route.c_str(),s.load_mode.empty()?"":" · ",s.load_mode.empty()?"":words("load-mode."+s.load_mode).c_str());}
        ImGui::Spacing();ImGui::Spacing();
        // Only the actions this state offers; a disabled one says why
        // on hover. Write actions first, in the large size the page always used.
        bool row=false;
        for(const auto& a:view.actions){const bool large=a.writes||a.primary;if(!large)continue;if(row)ImGui::SameLine();row=true;
            ImGui::BeginDisabled(!a.enabled);const bool clicked=ImGui::Button(words(a.label).c_str(),{180*dpi,44*dpi});ImGui::EndDisabled();
            if(!a.enabled&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",games::render(a.why_not).c_str());mark(control_id(a.name));
            if(clicked){if(a.name=="make-package"||a.name=="refresh-package")act(a.name,a.name=="refresh-package"?s.package:s.entry.id);
                else{pending_=s.entry.id;operation_=a.name;approved_=false;ImGui::OpenPopup("确认插件操作");}}}
        if(row)ImGui::Spacing();row=false;
        for(const auto& a:view.actions){const bool large=a.writes||a.primary;if(large)continue;if(row)ImGui::SameLine();row=true;
            ImGui::BeginDisabled(!a.enabled);const bool clicked=ImGui::Button(words(a.label).c_str());ImGui::EndDisabled();
            if(!a.enabled&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",games::render(a.why_not).c_str());mark(control_id(a.name));
            if(!clicked)continue;
            if(a.name=="open-folder"){const auto result=ShellExecuteW(window,L"explore",s.entry.exe.parent_path().c_str(),nullptr,nullptr,SW_SHOWNORMAL);if(reinterpret_cast<INT_PTR>(result)<=32)error_="Windows 无法打开游戏目录";}
            else if(a.name=="make-package"||a.name=="refresh-package")act(a.name,a.name=="refresh-package"?s.package:s.entry.id);
            else{pending_=s.entry.id;operation_=a.name;approved_=false;ImGui::OpenPopup("确认插件操作");}}
        ImGui::Spacing();ImGui::Spacing();ImGui::Separator();ImGui::Spacing();ImGui::TextDisabled("游戏内控制");ImGui::TextUnformatted("Insert 打开面板  /  NR 开关  /  Tone · Structure  /  Style 0 · 1 · 2");
        ImGui::TextWrapped("插件加载不等于 NR 已开启。游戏启动后自动待命，默认不运行 NR、不进行重型采样；独立控制台不需要一直开着。");
        ImGui::Spacing();ImGui::TextDisabled("适配边界");ImGui::TextWrapped("只安装离线单人、无反作弊、无 Denuvo 的 DLSS 游戏：Streamline 的光线重构或超分（有光线重构用光线重构），以及虚幻 5 直连 NGX 的光线重构或超分；Xbox app 游戏按系统包身份钉住。viewport 与深度类型在运行时从游戏自己的调用读出。游戏更新后需重新检查并适配；只能晚加载的游戏（如 RE9）用命令行生成适配包并配合 Steam 启动器。");
        if(!s.preflight.is_object()&&!s.checks.empty()&&ImGui::CollapsingHeader("检查明细"))for(const auto& c:s.checks)draw_check(c);
        if(ImGui::CollapsingHeader("安装细节")){ImGui::TextWrapped("宿主 SHA-256：%s",s.host_hash.empty()?"未安装 / 未读取":s.host_hash.c_str());ImGui::TextWrapped("只管理本次登记的 Lab 文件：游戏目录里的转发器 dxgi.dll，以及 overglaze\\ 子目录里的宿主、桥、配置和本次新装的 NR 模型。不覆盖其他加载器、不改 EXE、不改驱动或安全设置。");
            ImGui::TextDisabled("状态 %s · 安装 %s · 兼容 %s · 健康检查 %s",s.state.c_str(),s.install_state.c_str(),s.compatibility.c_str(),s.health.c_str());
            // The raw message of a refusal stays available, only not as the headline.
            for(const auto& r:s.reasons)if(r.params.is_object()&&r.params.contains("message"))ImGui::TextWrapped("原始信息：%s",r.params.value("message","").c_str());
            // What the manager keeps for this game (read-only count).
            for(const auto& g:storage_.games)if(g.id==s.entry.id){const auto mib=[](std::uint64_t b){return double(b)/1048576.0;};
                ImGui::TextWrapped("管理器为本游戏保存：恢复副本 %llu 份（%.1f MiB，其中 %llu 份按「每款保留最近 %zu 份」管理），暂存目录 %llu 个（%.1f MiB）%s。数据盘可用 %.1f GiB，安装要求保留 %llu GiB。",
                    static_cast<unsigned long long>(g.recovery_copies),mib(g.recovery_bytes),static_cast<unsigned long long>(g.managed_recovery_copies),games::kRecoveryCopiesKept,
                    static_cast<unsigned long long>(g.staging_directories),mib(g.staging_bytes),g.complete?"":"（统计不完整）",
                    double(storage_.data_disk_available)/1073741824.0,static_cast<unsigned long long>(storage_.data_disk_reserve>>30));}}
    }else{ImGui::Spacing();ImGui::TextUnformatted("先选择一个游戏");ImGui::Spacing();ImGui::TextWrapped("左侧管理安装；进入游戏后用 Insert 面板调节 NR。画面与实验结果仍在顶部「采集浏览」中查看。");}
    ImGui::SetNextWindowSize({590*dpi,0},ImGuiCond_Appearing);
    if(ImGui::BeginPopupModal("确认插件操作",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::TextWrapped("%s",operation_=="install"?"将向选中的游戏目录安装经过校验的 Lab 文件。请先退出游戏。可能影响启动或性能；不修改游戏 EXE、画质、驱动或安全设置。":
            operation_=="update"?"将先卸载已登记的旧插件文件（保存恢复副本）；适配包落后于已发布插件时先重新预检并刷新适配包；再装入新版本。中途失败时游戏会回到未安装状态，会如实告诉你停在哪一步。请先退出游戏。不修改游戏 EXE、画质、驱动或安全设置。":
            operation_=="repin"?"将重新预检这款游戏（Denuvo、反作弊、模块签名），卸载旧插件文件（保存恢复副本），把旧适配包移到 adapters-retired（不删除），按新版本生成适配包后重新安装。请先退出游戏。不修改游戏 EXE、画质、驱动或安全设置。":
            operation_=="uninstall"?"只卸载已登记且 SHA-256 未变的 Lab 文件，先保存恢复副本。已有模型（非本管理器新装）、游戏和采集数据不删除。":"仅移除列表记录，不删除游戏或采集数据。");
        const bool writes=operation_=="install"||operation_=="update"||operation_=="repin";
        // One confirmation per operation: the offline, no-anti-cheat scope is
        // stated here instead of ticked; preflight still refuses anti-cheat markers
        // and Denuvo before a package can exist at all.
        if(writes)ImGui::TextDisabled("仅用于离线单人、无反作弊的游戏；带反作弊标记或 Denuvo 的游戏在预检时已被拦下。");
        ImGui::Checkbox(operation_=="install"?"允许向该游戏目录安装插件":operation_=="update"?"允许更新该游戏目录里的插件":operation_=="repin"?"允许重新适配并安装到该游戏目录":operation_=="uninstall"?"确认卸载该游戏的 Lab 插件":"确认移除列表记录",&approved_);mark("games.approve");ImGui::Spacing();
        ImGui::BeginDisabled(!approved_);if(ImGui::Button("确认执行",{150*dpi,0})){act(operation_,pending_);ImGui::CloseCurrentPopup();}mark("games.confirm");ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("取消",{100*dpi,0}))ImGui::CloseCurrentPopup();mark("games.cancel");ImGui::EndPopup();}
    ImGui::EndChild();
    if(show_add_){ImGui::OpenPopup("添加游戏目录");show_add_=false;}
    ImGui::SetNextWindowSize({680*dpi,0},ImGuiCond_Appearing);
    if(ImGui::BeginPopupModal("添加游戏目录",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){ImGui::TextWrapped("粘贴安装路径，或选择目录。也可以直接填入游戏 EXE 路径。此步只登记和检查，不安装。");
        ImGui::BeginDisabled(busy());ImGui::SetNextItemWidth(495*dpi);ImGui::InputTextWithHint("##gamepath","D:\\Games\\游戏",path_,sizeof(path_));mark("games.path");ImGui::SameLine();if(ImGui::Button("选择目录")){const auto p=pick(window);if(p.size()<sizeof(path_))strcpy_s(path_,p.c_str());}mark("games.browse");
        if(ImGui::Button("检查路径",{140*dpi,0})){error_.clear();found_={};const auto path=std::filesystem::path(wide(path_));job_=std::async(std::launch::async,[path]{Result r;r.discovering=true;r.found=games::discover(path);return r;});}mark("games.checkpath");ImGui::EndDisabled();
        if(busy())ImGui::TextDisabled("检查中…");if(!error_.empty())ImGui::TextWrapped("%s",error_.c_str());if(!found_.notice.empty())ImGui::TextWrapped("%s",found_.notice.c_str());
        if(!found_.executables.empty()){ImGui::TextDisabled("选择实际游戏程序（不是启动器）");ImGui::BeginChild("game.exe.candidates",{630*dpi,150*dpi},ImGuiChildFlags_Borders);for(std::size_t i=0;i<found_.executables.size();++i){if(ImGui::Selectable(text(found_.executables[i]).c_str(),candidate_==int(i)))candidate_=int(i);}ImGui::EndChild();}
        ImGui::BeginDisabled(busy()||found_.executables.empty()||!found_.complete);if(ImGui::Button("添加到游戏库",{160*dpi,0})){const auto root=root_,exe=found_.executables.at(candidate_);job_=std::async(std::launch::async,[root,exe]{games::Manager m(root);m.add(exe);Result r;r.rows=rows(m);r.storage=m.storage_usage();r.has_storage=true;r.message=games::render("registered");return r;});selected_.clear();ImGui::CloseCurrentPopup();}mark("games.addconfirm");ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("取消",{100*dpi,0}))ImGui::CloseCurrentPopup();mark("games.addcancel");ImGui::EndPopup();}
}
}
