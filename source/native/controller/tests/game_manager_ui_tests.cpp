// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_game_manager_ui.hpp"
#include "lab_product_ui.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
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
lab::games::Status game(const std::filesystem::path& root,const char* id,const char* title,bool installed){
    lab::games::Status s;s.entry={id,title,root/std::filesystem::path(id)/L"Game.exe"};
    s.state=installed?"installed":"available";s.install_state=installed?"installed":"not-installed";
    s.installed=installed;s.can_install=!installed;s.can_uninstall=installed;s.package=std::string(id)+"-fixture";s.route="sl-rr";
    return s;
}
}

int main(){
    const auto root=std::filesystem::temp_directory_path()/(L"overglaze-ui-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
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
            click(page,"games.addcancel");need(!page.controls.contains("games.path"),"cancel closes add dialog");

            auto installed=game(root,"alpha","Alpha Game",true);
            auto available=game(root,"beta","Beta Game",false);
            auto blocked=game(root,"gamma","Gamma Game",false);blocked.state="anticheat-blocked";blocked.can_install=false;
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
            need(!lab::GameManagerPageTestAccess::approved(page),"every confirmation starts without consent");
            visible(page,"games.approve");visible(page,"games.confirm");visible(page,"games.cancel");
            click(page,"games.confirm");need(!page.busy(),"confirm without offline consent cannot dispatch install");
            lab::GameManagerPageTestAccess::select(page,"alpha");frame(page);
            need(lab::GameManagerPageTestAccess::pending(page)=="beta","confirmation target does not follow later selection");
            click(page,"games.approve");need(lab::GameManagerPageTestAccess::approved(page),"explicit consent checkbox responds");
            click(page,"games.cancel");need(!page.busy(),"cancel never dispatches an install");
            need(!page.controls.contains("games.confirm"),"cancel closes the confirmation");

            // The primary action and library controls remain usable in a compact desktop window.
            const ImVec2 compact{1040,740};frame(page,compact);frame(page,compact);
            click(page,"games.row.beta",compact);visible(page,"games.install",compact);visible(page,"games.add",compact);
            click(page,"games.install",compact);visible(page,"games.cancel",compact);
            need(!lab::GameManagerPageTestAccess::approved(page),"consent is reset when reopened");
            click(page,"games.cancel",compact);
            const float action_y=page.controls.at("games.install")[1].get<float>();
            for(const float dpi:{1.5f,2.f}){
                const ImVec2 scaled{1040*dpi,740*dpi};frame(page,scaled,dpi);frame(page,scaled,dpi);
                visible(page,"games.search",scaled);visible(page,"games.add",scaled);visible(page,"games.install",scaled);
                need(std::abs(page.controls.at("games.install")[1].get<float>()/dpi-action_y)<4,"headings and spacing scale once at high DPI");
                click(page,"games.install",scaled,dpi);visible(page,"games.approve",scaled);visible(page,"games.confirm",scaled);visible(page,"games.cancel",scaled);
                click(page,"games.cancel",scaled,dpi);need(!page.busy(),"scaled navigation never dispatches install");
            }
            need(!std::filesystem::exists(root/L"beta"),"UI navigation never creates a game directory");
        }
        ImGui::DestroyContext();std::filesystem::remove_all(root);
        std::cout<<"game manager UI: "<<checks<<" checks passed; "<<frames<<" frames without ImGui errors\n";return 0;
    }catch(const std::exception& e){
        if(ImGui::GetCurrentContext())ImGui::DestroyContext();
        std::cerr<<e.what()<<"\n";return 1;
    }
}
