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
    if(action.name=="make-package")return "生成适配包";
    if(action.name=="refresh-package")return "刷新适配包";
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
bool has_risk(const games::Status& s,const char* risk){return std::find(s.risks.begin(),s.risks.end(),risk)!=s.risks.end();}
// What the checks found, one line each: information, never an error state.
void risk_lines(const games::Status& s){
    if(has_risk(s,"anti-tamper"))muted_text(games::render("risk-anti-tamper").c_str());
    if(has_risk(s,"anticheat"))muted_text(games::render("risk-anticheat",json{{"names",s.anticheat}}).c_str());}
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
    const auto root=root_;const auto opening=opening_;
    job_=std::async(std::launch::async,[root,known,opening]{games::Manager m(root,std::nullopt,opening.model_sha256,opening.run_checker);if(known)m.import_known_installations();Result r;r.rows=rows(m);r.storage=m.storage_usage();r.has_storage=true;return r;});}
// What the install / update dialog's button means, recorded in the receipt: the
// dialog showed what the checks found and the risk paragraph, and the user went on.
constexpr const char* kRiskConsent="acknowledged in the viewer's confirmation dialog: online, anti-cheat and anti-tamper risks are the user's own; Overglaze bypasses no protection";
void GameManagerPage::act(const std::string& action,const std::string& id,bool late_package){if(busy())return;error_.clear();notice_.clear();const auto root=root_;const auto live=live_;const auto opening=opening_;
    {std::lock_guard guard(live->lock);live->text.clear();live->index=live->count=0;}
    job_=std::async(std::launch::async,[root,action,id,live,opening,late_package]{games::Manager m(root,std::nullopt,opening.model_sha256,opening.run_checker);Result r;
    // Progress: the stage now running, in the table's words.
    const games::Progress progress=[live](const games::ProgressEvent& ev){if(ev.status!="start")return;
        std::string t=words("operation."+ev.operation)+" › "+words("stage."+ev.operation+"."+ev.stage);if(!ev.parent.empty())t=words("operation."+ev.parent)+" › "+t;
        std::lock_guard guard(live->lock);live->text=std::move(t);live->index=ev.index;live->count=ev.count;};
    auto after=[&]{for(const auto& e:m.list())if(e.id==id)return m.inspect(e);return games::Status{};};
    // The success message says how THIS game is started, from its load mode; a
    // late-loading game also gets the launch card with the text to paste.
    auto started=[&](std::string done){const auto s=after();
        if(s.installed&&!s.launch_command.empty()){r.launch=Launch{s.entry.title,s.launch_via,s.launch_command};return done;}
        return done+" "+launch_text(s);};
    // The dialog's button is the user's risk acknowledgement for this one operation.
    if(action=="install"){m.install(id,true,kRiskConsent,progress);r.message=started(games::render("install-done"));}
    else if(action=="update"){m.update(id,true,kRiskConsent,progress);r.message=started(games::render("update-done"));}
    else if(action=="repin"){const auto retired=m.repin(id,true,kRiskConsent,false,progress);
        r.message=started(games::render("repin-done",json{{"retired",text(retired)}}));}
    // The controller's root proxy, never the research root layout (see
    // PackageOptions); late loading for a game whose EXE carries Denuvo.
    else if(action=="make-package"){auto options=games::PackageOptions::for_viewer(late_package);options.allow_unsigned_modules=opening.allow_unsigned_modules;
        const auto p=m.make_package(id,options);r.message=games::render("package-made-controller",json{{"package",p.name},{"route",p.route}});}
    else if(action=="refresh-package"){const auto p=m.refresh_package(id);r.message=games::render("package-refreshed",json{{"package",p.name}});}
    else if(action=="uninstall"){const auto before=after();m.uninstall(id,true,progress);r.message=games::render("uninstall-done");
        if(before.launch_via=="steam")r.message+=" "+games::render("launch.uninstall");}
    else if(action=="forget"){m.forget(id);r.message=games::render("forgotten");}
    else throw std::runtime_error("Unknown manager action");r.rows=rows(m);r.storage=m.storage_usage();r.has_storage=true;return r;});}
void GameManagerPage::poll(){using namespace std::chrono_literals;if(!busy()||job_.wait_for(0ms)!=std::future_status::ready)return;
    std::string failed;
    try{auto result=job_.get();if(result.discovering){found_=std::move(result.found);candidate_=0;}else{rows_=std::move(result.rows);if(result.has_storage)storage_=std::move(result.storage);if(!result.selection.empty())selected_=std::move(result.selection);else if(selected_.empty()&&!rows_.empty())selected_=rows_.front().entry.id;}if(!result.message.empty())notice_=std::move(result.message);
        if(result.launch){launch_=std::move(*result.launch);show_launch_=true;}}
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
    // How a late-loading game is started: where to paste it, the text itself
    // (read-only, selectable) with a copy button, and what follows from it.
    auto launch_card=[&](const std::string& via,const std::string& command,const char* copy_id){
        ImGui::PushID(copy_id);
        muted_text(games::render(via=="steam"?"launch.steam":"launch.watch").c_str());gap(6*dpi);
        std::string shown=command;
        ImGui::SetNextItemWidth((std::max)(120*dpi,ImGui::GetContentRegionAvail().x-100*dpi));
        ImGui::InputText("##command",shown.data(),shown.size()+1,ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine(0,8*dpi);
        if(ImGui::Button("复制",{92*dpi,0}))ImGui::SetClipboardText(command.c_str());
        ImGui::PopID();mark(copy_id);
        if(via=="steam"){gap(6*dpi);muted_text(games::render("launch.insert").c_str());muted_text(games::render("launch.remove").c_str());}
    };
    auto tooltip=[&](const games::Action& action){
        if(!action.enabled&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s",games::render(action.why_not).c_str());
    };
    auto invoke=[&](const games::Action& action,const games::Status& status){
        if(action.name=="open-folder"){
            const auto result=ShellExecuteW(window,L"explore",status.entry.exe.parent_path().c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            if(reinterpret_cast<INT_PTR>(result)<=32)error_="Windows 无法打开游戏目录";
        }else if(action.name=="make-package"||action.name=="refresh-package"){
            // Writes only Overglaze's own folder: no dialog. A Denuvo game's
            // package loads late (nothing in the game's root).
            act(action.name,action.name=="refresh-package"?status.package:status.entry.id,has_risk(status,"anti-tamper"));
        }else{
            pending_=status.entry.id;pending_title_=status.entry.title;pending_path_=text(status.entry.exe);pending_status_=status;
            operation_=action.name;approved_=false;ImGui::OpenPopup("确认插件操作");
        }
    };

    // The first screen is the library and the next available action. All detailed
    // evidence remains accessible further down; the backend owns every status.
    ImGui::BeginGroup();
    heading("游戏库",28);
    ImGui::EndGroup();
    ImGui::SameLine((std::max)(ImGui::GetCursorPosX(),ImGui::GetWindowContentRegionMax().x-242*dpi));
    ImGui::BeginDisabled(working);
    if(ImGui::Button("重新检查",{108*dpi,40*dpi}))refresh();mark("games.refresh");
    ImGui::SameLine(0,10*dpi);
    if(primary_button("添加游戏",{124*dpi,40*dpi}))begin_add();mark("games.add");
    ImGui::EndDisabled();
    gap(18*dpi);

    if(working){
        std::string stage;unsigned index=0,total=0;
        {std::lock_guard guard(live_->lock);stage=live_->text;index=live_->index;total=live_->count;}
        ImGui::PushStyleColor(ImGuiCol_Text,ink(games::Tone::accent));
        ImGui::TextUnformatted(stage.empty()?"读取游戏信息…":stage.c_str());ImGui::PopStyleColor();
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
        ImGui::TextColored(ink(games::Tone::success),"完成");
        ImGui::SameLine();if(ImGui::SmallButton("收起##games.notice"))notice_.clear();
        ImGui::TextWrapped("%s",notice_.c_str());gap(8*dpi);
    }

    const float available=ImGui::GetContentRegionAvail().x;
    const float library_width=(std::clamp)(available*.30f,265*dpi,345*dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,12*dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{16*dpi,16*dpi});
    ImGui::PushStyleColor(ImGuiCol_ChildBg,panel);
    ImGui::BeginChild("games.list",{library_width,0},ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextUnformatted("游戏");ImGui::SameLine();ImGui::TextDisabled("%zu",rows_.size());
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
        std::string label=words(view.label)+(s.running?" · 运行中":view.dot?" · 新版本":"");
        for(const auto& risk:s.risks)label+=" · "+words("tag."+risk); // information, never a state
        draw->AddText(ImGui::GetFont(),font*.86f,{clip.x,at.y+44*dpi},ImGui::GetColorU32(ink(view.tone)),label.c_str(),nullptr,0,&clip);
        if(hovered)ImGui::SetTooltip("%s\n%s",s.entry.title.c_str(),text(s.entry.exe).c_str());
        gap(4*dpi);ImGui::PopID();
    }
    if(visible.empty()){
        gap(18*dpi);
        muted_text(working&&rows_.empty()?"读取游戏库…":rows_.empty()?"未添加游戏":"无匹配游戏");
        gap(7*dpi);
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
        // Protections the checks found: neutral tags and one line each. They
        // never block; the install dialog repeats them with the risk notice.
        if(!s.risks.empty()){gap(8*dpi);
            for(std::size_t i=0;i<s.risks.size();++i){if(i)ImGui::SameLine(0,6*dpi);product::pill(words("tag."+s.risks[i]).c_str(),ink(games::Tone::neutral));if(!i)mark("games.risk");}
            gap(4*dpi);risk_lines(s);}
        gap(14*dpi);
        // These three preparation states get concise navigation copy. The
        // complete backend reasons, including refusals, remain in the fold.
        if(s.state=="needs-package")muted_text("已识别 DLSS，尚无适配包。");
        else if(s.state=="package-stale")muted_text("适配包需要刷新。");
        else if(s.state=="available")muted_text("当前游戏版本可安装。");
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
            gap(20*dpi);ImGui::TextUnformatted("启动方式");gap(7*dpi);
            const std::string launch=launch_text(s);
            // Before there is a package: the loading method 生成适配包 will choose.
            if(s.state=="needs-package"){
                muted_text(games::render("load-plan",json{{"load_mode",has_risk(s,"anti-tamper")?"late_d3d12":"root_proxy_d3d12"}}).c_str());mark("games.load-plan");
                if(has_risk(s,"anti-tamper"))muted_text(games::render("load-late-anti-tamper").c_str());
                gap(4*dpi);}
            // An installed late-loading game: the text to paste stays here, to copy again.
            if(s.installed&&!s.launch_command.empty())launch_card(s.launch_via,s.launch_command,"games.copy-launch");
            else if(s.load_mode=="root_proxy_d3d12")muted_text("从商店启动游戏。");
            else if(s.load_mode=="root_proxy_on_insert")muted_text("首次按 Insert 加载插件并打开面板。");
            else if(!launch.empty())ImGui::TextWrapped("%s",launch.c_str());
            else muted_text("安装后启用游戏的 DLSS 光线重构、超分辨率或 DLAA。");
            gap(8*dpi);
            product::pill("Insert",muted);ImGui::SameLine();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("打开游戏内面板");
            gap(4*dpi);muted_text("NR 默认关闭，运行状态以游戏内面板为准。");
        }
        gap(20*dpi);ImGui::Separator();gap(12*dpi);

        const bool show_checks=ImGui::CollapsingHeader("检查明细");mark("games.check-details");
        if(show_checks){
            gap(5*dpi);
            muted_text("「无法检查」不代表通过。");
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
            muted_text("卸载前校验已登记文件并保存恢复副本。");
            for(const auto& g:storage_.games)if(g.id==s.entry.id){
                ImGui::TextWrapped("恢复副本 %llu 份 · %.1f MiB%s",static_cast<unsigned long long>(g.recovery_copies),double(g.recovery_bytes)/1048576.0,g.complete?"":"（统计不完整）");
                ImGui::TextWrapped("自动管理 %llu 份 · 保留最近 %zu 份",static_cast<unsigned long long>(g.managed_recovery_copies),games::kRecoveryCopiesKept);
                ImGui::TextWrapped("暂存目录 %llu 个 · %.1f MiB",static_cast<unsigned long long>(g.staging_directories),double(g.staging_bytes)/1048576.0);
                ImGui::TextWrapped("数据盘可用 %.1f GiB · 安装预留 %llu GiB",double(storage_.data_disk_available)/1073741824.0,static_cast<unsigned long long>(storage_.data_disk_reserve>>30));
            }
            gap(8*dpi);
        }
        if(ImGui::CollapsingHeader("使用范围")){
            muted_text("适合离线单人、支持 DLSS 的 DX12 游戏。联网、带反作弊或反篡改的游戏也可安装，风险自负：可能无法启动、被踢出或被处罚。Overglaze 不绕过任何保护。");
            gap(6*dpi);muted_text("适配检查不保证画质或性能。游戏更新后需重新检查；部分游戏需单独配置。");
        }
    }else{
        gap((std::max)(28*dpi,ImGui::GetContentRegionAvail().y*.18f));
        const ImVec2 at=ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(at,{at.x+64*dpi,at.y+64*dpi},IM_COL32(36,45,44,255),16*dpi);
        game_symbol(ImGui::GetWindowDrawList(),{at.x+12*dpi,at.y+12*dpi},40*dpi,ImGui::GetColorU32(ink(games::Tone::accent)));
        ImGui::Dummy({64*dpi,64*dpi});gap(18*dpi);
        heading(working&&rows_.empty()?"读取游戏库…":rows_.empty()?"未添加游戏":"无匹配游戏",25);
        if(rows_.empty()&&!working){gap(18*dpi);if(primary_button("添加游戏",{185*dpi,44*dpi}))begin_add();mark("games.add-empty");}
    }

    // Width fixed every frame, height from the content: AlwaysAutoResize alone
    // feeds widths taken from the available region back into the window's own
    // width, and truncation then shrinks the popup a little every frame.
    {const float w=(std::min)(650*dpi,ImGui::GetIO().DisplaySize.x-40*dpi);ImGui::SetNextWindowSizeConstraints({w,0},{w,FLT_MAX});}
    if(ImGui::BeginPopupModal("确认插件操作",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        heading(operation_=="install"?"安装插件":operation_=="update"?"更新插件":operation_=="repin"?"重新适配":operation_=="uninstall"?"卸载插件":"移出游戏库",23);
        gap(12*dpi);
        heading(pending_title_.c_str(),19);muted_text(pending_path_.c_str());
        gap(12*dpi);ImGui::Separator();gap(12*dpi);
        ImGui::TextWrapped("%s",operation_=="install"?"退出游戏后安装。可能影响游戏启动或性能。":
            operation_=="update"?"退出游戏后，保存恢复副本、卸载旧插件并安装新版。失败时可能处于未安装状态。":
            operation_=="repin"?"退出游戏后，重新检查版本、保存恢复副本并重装。旧适配包归档保留。":
            operation_=="uninstall"?"移除已登记的插件文件并保存恢复副本。保留游戏本体和采集数据。":
            "仅移除游戏库记录，保留游戏和采集数据。");
        const bool writes=operation_=="install"||operation_=="update"||operation_=="repin";
        gap(12*dpi);
        // An operation that writes into the game: what the checks found, then the
        // one risk paragraph. Pressing the button below is the user's risk
        // acknowledgement for this operation (a reminder, not a gate); the
        // backend gates still apply. A marker scan proves no game free of them.
        if(writes){
            if(pending_status_.risks.empty())muted_text(games::render("risk-unproven").c_str());else risk_lines(pending_status_);
            gap(6*dpi);
            ImGui::PushStyleColor(ImGuiCol_Text,ink(games::Tone::warning));ImGui::TextWrapped("%s",games::render("risk-notice").c_str());ImGui::PopStyleColor();
            mark("games.risk-notice");
            gap(6*dpi);muted_text(games::render("risk-accept").c_str());gap(16*dpi);
        }else{
            if(operation_=="uninstall"&&pending_status_.launch_via=="steam"){muted_text(games::render("launch.uninstall").c_str());gap(8*dpi);}
            ImGui::Checkbox(operation_=="uninstall"?"确认卸载此游戏的 Overglaze 插件":"确认移除列表记录",&approved_);
            mark("games.approve");gap(16*dpi);
        }
        ImGui::BeginDisabled((!writes&&!approved_)||busy());
        if(primary_button(operation_=="install"?"安装":operation_=="update"?"更新":operation_=="repin"?"重新适配":operation_=="uninstall"?"卸载":"移除",{150*dpi,40*dpi})){act(operation_,pending_);ImGui::CloseCurrentPopup();}
        mark("games.confirm");ImGui::EndDisabled();ImGui::SameLine(0,10*dpi);
        if(ImGui::Button("取消",{100*dpi,40*dpi}))ImGui::CloseCurrentPopup();mark("games.cancel");
        ImGui::EndPopup();
    }
    ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar(2);

    if(show_add_){ImGui::OpenPopup("添加游戏目录");show_add_=false;}
    {const float w=(std::min)(720*dpi,ImGui::GetIO().DisplaySize.x-40*dpi);ImGui::SetNextWindowSizeConstraints({w,0},{w,FLT_MAX});}
    if(ImGui::BeginPopupModal("添加游戏目录",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        heading("添加游戏",24);gap(8*dpi);
        gap(16*dpi);ImGui::TextUnformatted("游戏位置");
        ImGui::BeginDisabled(busy());
        ImGui::SetNextItemWidth((std::max)(100*dpi,ImGui::GetContentRegionAvail().x-118*dpi));
        if(ImGui::InputTextWithHint("##gamepath","游戏目录或 EXE 完整路径",path_,sizeof(path_))){found_={};candidate_=0;}mark("games.path");
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
            gap(14*dpi);ImGui::TextUnformatted("游戏 EXE（非启动器）");gap(6*dpi);
            ImGui::BeginChild("game.exe.candidates",{0,145*dpi},ImGuiChildFlags_Borders);
            for(std::size_t i=0;i<found_.executables.size();++i){
                ImGui::PushID(static_cast<int>(i));
                if(ImGui::Selectable(text(found_.executables[i]).c_str(),candidate_==int(i)))candidate_=int(i);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        gap(18*dpi);ImGui::Separator();gap(14*dpi);
        // A partial scan still lists what it found, best candidate first; the
        // notice above says the scan was partial.
        ImGui::BeginDisabled(busy()||found_.executables.empty());
        if(primary_button("添加",{165*dpi,40*dpi})){
            const auto root=root_,exe=found_.executables.at(candidate_);const auto opening=opening_;
            job_=std::async(std::launch::async,[root,exe,opening]{games::Manager m(root,std::nullopt,opening.model_sha256,opening.run_checker);const auto added=m.add(exe);Result r;r.selection=added.id;r.rows=rows(m);r.storage=m.storage_usage();r.has_storage=true;r.message=games::render("registered");return r;});
            selected_.clear();*search_=0;filter_=0;ImGui::CloseCurrentPopup();
        }
        mark("games.addconfirm");ImGui::EndDisabled();ImGui::SameLine(0,10*dpi);
        if(ImGui::Button("取消",{100*dpi,40*dpi}))ImGui::CloseCurrentPopup();mark("games.addcancel");ImGui::EndPopup();
    }

    // After installing a late-loading game: how to start it, with the exact
    // text to paste. The same card stays in that game's 启动方式 afterwards.
    if(show_launch_){ImGui::OpenPopup("启动方式");show_launch_=false;}
    {const float w=(std::min)(720*dpi,ImGui::GetIO().DisplaySize.x-40*dpi);ImGui::SetNextWindowSizeConstraints({w,0},{w,FLT_MAX});}
    if(ImGui::BeginPopupModal("启动方式",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        heading(launch_.via=="steam"?"设置 Steam 启动选项":"启动方式",23);gap(8*dpi);
        heading(launch_.title.c_str(),19);gap(12*dpi);ImGui::Separator();gap(12*dpi);
        launch_card(launch_.via,launch_.command,"games.launch-copy");
        gap(18*dpi);
        if(primary_button("完成",{150*dpi,40*dpi}))ImGui::CloseCurrentPopup();mark("games.launch-close");
        ImGui::EndPopup();
    }
}
}
