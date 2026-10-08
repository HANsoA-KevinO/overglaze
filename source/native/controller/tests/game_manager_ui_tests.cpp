// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_game_manager_ui.hpp"
#include "lab_product_ui.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <fstream>
#include <iostream>

// Synthetic UI states live only in this test. No install action is confirmed,
// no game path is read, and the manager starts with an isolated empty registry.
namespace lab {
struct GameManagerPageTestAccess {
    static void finish_read(GameManagerPage& page){if(page.job_.valid())page.job_.wait();page.poll();}
    static void seed(GameManagerPage& page,std::vector<games::Status> rows){
        finish_read(page);page.rows_=std::move(rows);page.selected_.clear();page.error_.clear();
    }
    static const std::string& pending(const GameManagerPage& page){return page.pending_;}
    static const std::string& pending_title(const GameManagerPage& page){return page.pending_title_;}
    static const std::string& pending_path(const GameManagerPage& page){return page.pending_path_;}
    static bool approved(const GameManagerPage& page){return page.approved_;}
    static void select(GameManagerPage& page,const char* id){page.selected_=id;}
    // A synthetic Lab: its model hash, no installation checker, unsigned synthetic modules.
    static void open(GameManagerPage& page,const std::string& model){finish_read(page);page.opening_={model,false,true};page.refresh();finish_read(page);}
    static const GameManagerPage::Launch& launch(const GameManagerPage& page){return page.launch_;}
    static const std::string& notice(const GameManagerPage& page){return page.notice_;}
};
}
namespace {
unsigned checks=0,frames=0;
void need(bool value,const std::string& message){++checks;if(!value)throw std::runtime_error(message);}

void frame(lab::GameManagerPage& page,ImVec2 size={1280,900},float dpi=1){
    // Scaling is absolute, just as it is on desktop startup. apply_theme only
    // sets product overrides; repeatedly scaling the leftover WindowMinSize
    // would grow it exponentially and force a window wider than the viewport.
    ImGui::GetStyle()=ImGuiStyle{};lab::product::apply_theme();
    ImGui::GetStyle().ScaleAllSizes(dpi);ImGui::GetStyle().FontScaleDpi=dpi;
    auto& io=ImGui::GetIO();io.DisplaySize=size;io.DeltaTime=1.f/60.f;
    ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(size);
    ImGui::Begin("Game UI test",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
    page.draw(nullptr,dpi);ImGui::End();ImGui::Render();++frames;
    if(ImGui::GetCurrentContext()->ErrorCountCurrentFrame!=0)throw std::runtime_error("ImGui reported a layout error");
}
void click(lab::GameManagerPage& page,const char* id,ImVec2 size={1280,900},float dpi=1){
    need(page.controls.contains(id),std::string("requested UI control exists: ")+id);
    const auto box=page.controls.at(id);
    auto& io=ImGui::GetIO();io.AddMousePosEvent(box[0].get<float>()+box[2].get<float>()*.5f,box[1].get<float>()+box[3].get<float>()*.5f);
    frame(page,size,dpi);io.AddMouseButtonEvent(0,true);frame(page,size,dpi);io.AddMouseButtonEvent(0,false);frame(page,size,dpi);
    frame(page,size,dpi);
}
void visible(const lab::GameManagerPage& page,const char* id,ImVec2 size={1280,900}){
    need(page.controls.contains(id),std::string("visible control exists: ")+id);const auto b=page.controls.at(id);
    need(b[0].get<float>()>=0&&b[1].get<float>()>=0&&b[0].get<float>()+b[2].get<float>()<=size.x&&b[1].get<float>()+b[3].get<float>()<=size.y,
        std::string("control fits viewport: ")+id+"; frame="+std::to_string(frames)+" dpi="+std::to_string(ImGui::GetStyle().FontScaleDpi)+
        " viewport="+std::to_string(size.x)+"x"+std::to_string(size.y)+" rect="+b.dump());
}
// Minimal x64 PE image, never executed: the section names are what the
// preflight reads (Denuvo virtualisation sections).
std::string pe64(const std::vector<std::string>& sections,const std::string& salt){
    auto put16=[](std::string& d,size_t o,unsigned v){d[o]=char(v&255);d[o+1]=char((v>>8)&255);};
    auto put32=[](std::string& d,size_t o,unsigned v){for(unsigned i=0;i<4;++i)d[o+i]=char((v>>(8*i))&255);};
    std::string d(0x400+sections.size()*0x200,'\0');d[0]='M';d[1]='Z';put32(d,60,0x80);const size_t pe=0x80;d[pe]='P';d[pe+1]='E';
    put16(d,pe+4,0x8664);put16(d,pe+6,unsigned(sections.size()));put16(d,pe+20,0xF0);put16(d,pe+24,0x20b);
    const size_t table=pe+24+0xF0;
    for(size_t i=0;i<sections.size();++i){const size_t at=table+i*40;for(size_t k=0;k<8&&k<sections[i].size();++k)d[at+k]=sections[i][k];
        put32(d,at+8,0x200);put32(d,at+12,unsigned(0x1000*(i+1)));put32(d,at+16,0x200);put32(d,at+20,unsigned(0x400+i*0x200));put32(d,at+36,0x60000020);}
    for(size_t i=0;i<salt.size()&&0x400+i<d.size();++i)d[0x400+i]=salt[i];return d;}
void put(const std::filesystem::path& path,const std::string& value){std::ofstream f(path,std::ios::binary|std::ios::trunc);f<<value;if(!f)throw std::runtime_error("fixture write");}
lab::json read(const std::filesystem::path& path){std::ifstream f(path);return lab::json::parse(f);}
std::string clipboard;
lab::games::Status game(const std::filesystem::path& root,const char* id,const char* title,bool installed){
    lab::games::Status s;s.entry={id,title,root/std::filesystem::path(id)/L"Game.exe"};
    s.state=installed?"installed":"available";s.install_state=installed?"installed":"not-installed";
    s.installed=installed;s.can_install=!installed;s.can_uninstall=installed;s.package=std::string(id)+"-fixture";s.route="sl-rr";
    return s;
}
}

int main(){
    // OVERGLAZE_TEST_TEMP when set, else the user's temp directory (an install
    // below needs the manager's data-disk reserve there).
    const auto base=[]{std::wstring v(32768,L'\0');auto n=GetEnvironmentVariableW(L"OVERGLAZE_TEST_TEMP",v.data(),DWORD(v.size()));
        if(!n||n>=v.size())return std::filesystem::temp_directory_path();v.resize(n);return std::filesystem::path(v);}();
    const auto root=std::filesystem::canonical(base)/(L"overglaze-ui-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try{
        std::filesystem::create_directory(root);
        ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
        io.Fonts->AddFontDefault();unsigned char* pixels=nullptr;int width=0,height=0;
        io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);io.Fonts->SetTexID(ImTextureID(1));
        lab::product::apply_theme();
        {
            lab::GameManagerPage page(root,false);lab::GameManagerPageTestAccess::finish_read(page);
            frame(page);frame(page);
            need(page.count()==0,"fresh portable root has no invented games");
            need(page.error().empty(),"fresh portable root loads without an error");
            visible(page,"games.add");visible(page,"games.add-empty");visible(page,"games.search");
            click(page,"games.add-empty");
            visible(page,"games.path");visible(page,"games.addcancel");
            // Regression: the auto-resizing popup fed widths taken from the available
            // region back into its own width. At a fractional scale (125%) truncation
            // shrank it by a pixel every frame (724, 716, ... 692) until its minimum.
            click(page,"games.addcancel");need(!page.controls.contains("games.path"),"cancel closes add dialog");
            for(const float d:{1.25f,1.5f}){const ImVec2 view{1564*d,941*d};
                frame(page,view,d);frame(page,view,d);click(page,"games.add-empty",view,d);
                auto* popup=ImGui::FindWindowByName("添加游戏目录");need(popup!=nullptr,"add dialog window exists");
                for(int i=0;i<40;++i)frame(page,view,d);
                need(popup->Size.x==std::floor(720*d),"add dialog keeps its set width across frames at "+std::to_string(d)+"x");
                click(page,"games.addcancel",view,d);}
            frame(page);frame(page);

            auto installed=game(root,"alpha","Alpha Game",true);
            auto available=game(root,"beta","Beta Game",false);
            auto blocked=game(root,"gamma","Gamma Game",false);blocked.state="no-dlss";blocked.can_install=false;
            lab::GameManagerPageTestAccess::seed(page,{installed,available,blocked});frame(page);frame(page);
            need(page.controls.contains("games.row.alpha")&&page.controls.contains("games.row.beta")&&page.controls.contains("games.row.gamma"),"all games are listed");
            click(page,"games.filter.1");
            need(page.controls.contains("games.row.alpha")&&!page.controls.contains("games.row.beta"),"installed filter selects installed games");
            click(page,"games.filter.2");
            need(!page.controls.contains("games.row.alpha")&&page.controls.contains("games.row.beta")&&!page.controls.contains("games.row.gamma"),"attention filter uses real available actions");
            click(page,"games.filter.0");click(page,"games.search");io.AddInputCharactersUTF8("aLpHa");frame(page);frame(page);
            need(page.controls.contains("games.row.alpha")&&!page.controls.contains("games.row.beta"),"search is case insensitive");
            io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);frame(page);
            io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);io.AddKeyEvent(ImGuiKey_Backspace,true);frame(page);
            io.AddKeyEvent(ImGuiKey_Backspace,false);frame(page);
            need(page.controls.contains("games.row.beta"),"clearing search restores library");

            click(page,"games.row.gamma");need(!page.controls.contains("games.install"),"blocked game exposes no install action");
            click(page,"games.row.beta");visible(page,"games.install");visible(page,"games.copy-path");
            click(page,"games.install");
            need(lab::GameManagerPageTestAccess::pending(page)=="beta","confirmation binds selected id");
            need(lab::GameManagerPageTestAccess::pending_title(page)==available.entry.title,"confirmation names the actual selected game");
            need(lab::GameManagerPageTestAccess::pending_path(page)==lab::utf8(available.entry.exe.wstring()),"confirmation displays the actual selected path");
            // The install dialog is a reminder, not a gate: the risk notice and the
            // button. No checkbox stands before it; the button is the acknowledgement.
            visible(page,"games.risk-notice");visible(page,"games.confirm");visible(page,"games.cancel");
            need(!page.controls.contains("games.approve"),"no extra checkbox before installing");
            lab::GameManagerPageTestAccess::select(page,"alpha");frame(page);
            need(lab::GameManagerPageTestAccess::pending(page)=="beta","confirmation target does not follow later selection");
            click(page,"games.cancel");need(!page.busy(),"cancel never dispatches an install");
            need(!page.controls.contains("games.confirm"),"cancel closes the confirmation");
            // Uninstall still asks for its own explicit confirmation.
            click(page,"games.row.alpha");click(page,"games.uninstall");visible(page,"games.approve");
            need(!lab::GameManagerPageTestAccess::approved(page),"every confirmation starts without consent");
            click(page,"games.confirm");need(!page.busy(),"confirm without the uninstall checkbox cannot dispatch");
            click(page,"games.approve");need(lab::GameManagerPageTestAccess::approved(page),"explicit consent checkbox responds");
            click(page,"games.cancel");need(!page.busy(),"cancel never dispatches an uninstall");

            // The primary action and library controls remain usable in a compact desktop window.
            const ImVec2 compact{1040,740};frame(page,compact);frame(page,compact);
            click(page,"games.row.beta",compact);visible(page,"games.install",compact);visible(page,"games.add",compact);
            click(page,"games.install",compact);visible(page,"games.cancel",compact);visible(page,"games.risk-notice",compact);
            click(page,"games.cancel",compact);
            const float action_y=page.controls.at("games.install")[1].get<float>();
            for(const float dpi:{1.5f,2.f}){
                const ImVec2 scaled{1040*dpi,740*dpi};frame(page,scaled,dpi);frame(page,scaled,dpi);
                visible(page,"games.search",scaled);visible(page,"games.add",scaled);visible(page,"games.install",scaled);
                need(std::abs(page.controls.at("games.install")[1].get<float>()/dpi-action_y)<4,"headings and spacing scale once at high DPI");
                click(page,"games.install",scaled,dpi);visible(page,"games.risk-notice",scaled);visible(page,"games.confirm",scaled);visible(page,"games.cancel",scaled);
                click(page,"games.cancel",scaled,dpi);need(!page.busy(),"scaled navigation never dispatches install");
            }
            need(!std::filesystem::exists(root/L"beta"),"UI navigation never creates a game directory");
        }
        // ---- the real manager behind the page, on a synthetic Lab: a Denuvo game
        // in a Steam folder and an anti-cheat game, from 生成适配包 to the launch option.
        {
            namespace fs=std::filesystem;const auto lab_root=root/L"lab";
            for(const auto* d:{L"",L"app",L"app/plugin",L"app/tools",L"app/models",L"app/adapters",L"data",L"denuvo",L"online"})fs::create_directories(lab_root/d);
            put(lab_root/L"app/plugin/dxgi.dll","synthetic proxy");put(lab_root/L"app/plugin/overglaze_nvngx.dll","synthetic bridge");
            put(lab_root/L"app/plugin/overglaze_controller.dll","synthetic controller");put(lab_root/L"app/tools/overglaze_install_check.exe","synthetic checker");
            put(lab_root/L"app/models/nvngx_dlssnr.dll","synthetic model");const auto model=lab::sha256(lab_root/L"app/models/nvngx_dlssnr.dll");
            put(lab_root/L"denuvo/Denuvo.exe",pe64({".text",".xtext",".xcode",".rdata"},"denuvo"));put(lab_root/L"denuvo/steam_api64.dll","steam");
            put(lab_root/L"online/Online.exe",pe64({".text",".rdata",".data"},"online"));put(lab_root/L"online/EasyAntiCheat_EOS.dll","eac");
            for(const auto* dir:{L"denuvo",L"online"})for(const auto* n:{L"sl.interposer.dll",L"sl.common.dll",L"sl.dlss_d.dll"})put(lab_root/dir/n,lab::utf8(std::wstring(dir)+n));
            std::string denuvo_id,online_id;
            {lab::games::Manager m(lab_root,std::nullopt,model,false);denuvo_id=m.add(lab_root/L"denuvo/Denuvo.exe").id;online_id=m.add(lab_root/L"online/Online.exe").id;}
            lab::GameManagerPage page(lab_root,false);lab::GameManagerPageTestAccess::open(page,model);frame(page);frame(page);
            need(page.count()==2&&page.error().empty(),"both games listed: neither protection is a refusal");
            ImGui::GetPlatformIO().Platform_SetClipboardTextFn=[](ImGuiContext*,const char* t){clipboard=t?t:"";};
            auto row=[](const std::string& id){return "games.row."+id;};
            auto package_of=[&](const char* exe)->fs::path{for(const auto& d:fs::directory_iterator(lab_root/L"app/adapters"))if(read(d.path()/L"package.json")["executable"]==exe)return d.path();return {};};
            // Denuvo: shown as information, packaged without a dialog, loaded late.
            click(page,row(denuvo_id).c_str());visible(page,"games.risk");visible(page,"games.load-plan");
            click(page,"games.makepackage");need(!page.controls.contains("games.confirm"),"making a package opens no dialog");
            lab::GameManagerPageTestAccess::finish_read(page);need(page.error().empty(),"the Denuvo game is packaged: "+page.error());
            {const auto pkg=package_of("Denuvo.exe");need(!pkg.empty(),"a package for the Denuvo game");const auto man=read(pkg/L"package.json");const auto cfg=read(pkg/L"overglaze.install.json");
             need(man["loader"]["strategy"]=="late_d3d12"&&man["track"]=="controller"&&!man["payload"].contains("dxgi.dll"),"a Denuvo game gets late loading: nothing for its root");
             need(cfg["version"]==4&&cfg["risk"]["anti_tamper"]==true&&!cfg.contains("no_anticheat"),"its config records the risk, no false claim");}
            // Install: the dialog names the protection and the risk; its button installs.
            frame(page);click(page,"games.install");visible(page,"games.risk-notice");need(!page.controls.contains("games.approve"),"no checkbox before the button");
            click(page,"games.confirm");lab::GameManagerPageTestAccess::finish_read(page);need(page.error().empty(),"the dialog's button installs: "+page.error());
            need(fs::exists(lab_root/L"denuvo/overglaze/overglaze_controller.dll")&&!fs::exists(lab_root/L"denuvo/dxgi.dll"),"installed late: nothing in the game's root");
            {const auto tx=read(lab_root/L"data/settings/plugin-manager"/lab::wide(denuvo_id)/L"transaction.json");
             need(tx["risk_acknowledged"]==true&&tx["risks"]==lab::json::array({"anti-tamper"}),"the transaction records the acknowledgement");}
            // The launch card: the exact Steam launch option, from this Lab's own root.
            frame(page);frame(page);const auto option="\""+lab::utf8((fs::canonical(lab_root)/L"app"/L"overglaze_launch.exe").wstring())+"\" %command%";
            visible(page,"games.launch-copy");
            need(lab::GameManagerPageTestAccess::launch(page).via=="steam"&&lab::GameManagerPageTestAccess::launch(page).command==option,"the launch option is built from the real root: "+lab::GameManagerPageTestAccess::launch(page).command);
            clipboard.clear();click(page,"games.launch-copy");need(clipboard==option,"复制 puts the exact option on the clipboard");
            click(page,"games.launch-close");need(!page.controls.contains("games.launch-copy"),"the card closes");
            visible(page,"games.copy-launch");clipboard.clear();click(page,"games.copy-launch");need(clipboard==option,"and stays in the game's detail to copy again");
            // Anti-cheat alone: the root proxy; installs after the same reminder, no launch card.
            click(page,row(online_id).c_str());visible(page,"games.risk");
            click(page,"games.makepackage");lab::GameManagerPageTestAccess::finish_read(page);need(page.error().empty(),"the anti-cheat game is packaged: "+page.error());
            {const auto pkg=package_of("Online.exe");const auto cfg=read(pkg/L"overglaze.install.json");
             need(read(pkg/L"package.json")["loader"]["strategy"]=="root_proxy_d3d12","anti-cheat alone keeps the root proxy");
             need(cfg["risk"]["anticheat"]==lab::json::array({"Easy Anti-Cheat"})&&!cfg.contains("no_anticheat")&&!cfg.contains("offline_single_player"),"the config names the anti-cheat and claims nothing false");}
            frame(page);click(page,"games.install");visible(page,"games.risk-notice");click(page,"games.confirm");
            lab::GameManagerPageTestAccess::finish_read(page);frame(page);frame(page);need(page.error().empty()&&fs::exists(lab_root/L"online/dxgi.dll"),"installed after the reminder");
            need(!page.controls.contains("games.launch-copy"),"a root-proxy game needs no launch option");
            // Uninstalling the late game reminds about the launch option.
            click(page,row(denuvo_id).c_str());click(page,"games.uninstall");click(page,"games.approve");click(page,"games.confirm");
            lab::GameManagerPageTestAccess::finish_read(page);need(page.error().empty()&&!fs::exists(lab_root/L"denuvo/overglaze"),"uninstalled");
            need(lab::GameManagerPageTestAccess::notice(page).find(lab::games::render("launch.uninstall"))!=std::string::npos,"the uninstall notice reminds about the launch option");
            click(page,row(online_id).c_str());click(page,"games.uninstall");click(page,"games.approve");click(page,"games.confirm");lab::GameManagerPageTestAccess::finish_read(page);
            need(!fs::exists(lab_root/L"online/dxgi.dll"),"the anti-cheat game uninstalls too");
            ImGui::GetPlatformIO().Platform_SetClipboardTextFn=nullptr;
        }
        ImGui::DestroyContext();std::filesystem::remove_all(root);
        std::cout<<"game manager UI: "<<checks<<" checks passed; "<<frames<<" frames without ImGui errors\n";return 0;
    }catch(const std::exception& e){
        if(ImGui::GetCurrentContext())ImGui::DestroyContext();
        std::cerr<<e.what()<<"\n";return 1;
    }
}
