// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_game_manager.hpp"
#include "lab_windows_path.hpp"
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
namespace {
unsigned checks=0;
void need(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void put(const fs::path& p,const std::string& s){fs::create_directories(p.parent_path());std::ofstream out(p,std::ios::binary);out<<s;need(bool(out),"fixture written");}
void put(const fs::path& p,const lab::json& j){put(p,j.dump());}
template<class F>void refuses(F f,const char* why){bool caught=false;try{f();}catch(const std::exception&){caught=true;}need(caught,why);}
bool allows(const fs::path& root,const char* op){return lab::games::app_maintenance_check(root,op).allowed;}
}
int main(){fs::path base;try{
    const auto temp=fs::temp_directory_path();base=temp/lab::wide("overglaze-maintenance-test-"+lab::uuid());
    need(fs::create_directory(base),"unique fixture directory");put(base/L"PURPOSE.json",lab::json{{"synthetic",true},{"game_started",false},{"nr_executed",false}});
    const auto root=base/L"installed app 中文";
    need(allows(root,"update")&&!fs::exists(root),"fresh preflight never creates missing target");
    need(!allows(root,"uninstall"),"missing target refuses uninstall");
    fs::create_directory(root);need(allows(root,"update")&&fs::is_empty(root),"empty target preflight is read-only");
    put(root/L"unrelated.txt",std::string("keep me"));need(!allows(root,"update"),"foreign nonempty directory refused");
    need(fs::file_size(root/L"unrelated.txt")==7,"foreign content unchanged");fs::remove(root/L"unrelated.txt");
    put(root/L"app/release-manifest.json",lab::json{{"schema","overglaze-release-v2"},{"platform","windows-x64"},{"version","0.2.0-preview.2"}});
    need(allows(root,"update")&&allows(root,"uninstall"),"known application permits maintenance");
    need(!fs::exists(root/L"data"),"recognised-root preflight does not create a data directory");
    const auto store=root/L"data/settings/plugin-manager";const std::string id="{D17BE316-A35A-44AA-BF60-F46368171D41}";
    const auto game=base/L"synthetic-game/Game.exe";const auto box=store/lab::wide(id);
    const auto library=lab::json{{"schema","overglaze-game-library-v1"},{"entries",lab::json::array({{{"id",id},{"title","Synthetic game 中文"},{"exe",lab::utf8(game.wstring())}}})}};
    put(store/L"games.json",library);
    lab::json transaction={{"schema","overglaze-install-transaction-v1"},{"id",id},{"exe",lab::utf8(game.wstring())},{"state","installed"}};
    put(box/L"transaction.json",transaction);
    need(allows(root,"update")&&!allows(root,"uninstall"),"installed plugin permits app update but blocks removal");
    transaction["state"]="installing";put(box/L"transaction.json",transaction);
    need(!allows(root,"update")&&!allows(root,"uninstall"),"incomplete game transaction blocks both operations");
    transaction["state"]="removed";put(box/L"transaction.json",transaction);
    need(allows(root,"uninstall"),"removed transaction permits app uninstall");
    put(box/L"install-receipt.json",lab::json{{"state","installed"}});need(!allows(root,"uninstall"),"live receipt still blocks uninstall");
    put(box/L"install-receipt.json",lab::json{{"state","uninstalled"}});need(allows(root,"uninstall"),"uninstalled receipt permits removal");
    put(store/L"games.json",lab::json{{"schema","overglaze-game-library-v1"},{"entries",lab::json::array()}});
    transaction["state"]="installed";put(box/L"transaction.json",transaction);
    need(!allows(root,"uninstall"),"orphan installed transaction is not hidden by an empty registry");
    transaction["state"]="removed";put(box/L"transaction.json",transaction);
    put(store/L"games.json",std::string("{broken"));need(!allows(root,"update")&&!allows(root,"uninstall"),"corrupt library fails closed");
    put(store/L"games.json",library);
    put(game.parent_path()/L"overglaze/overglaze.install.json",lab::json{{"output_root",lab::utf8((root/L"data").wstring())}});
    need(!allows(root,"uninstall"),"a surviving game configuration blocks removal despite removed transaction");
    fs::remove(game.parent_path()/L"overglaze/overglaze.install.json");
    put(root/L"data/settings/application-install.json",lab::json{{"schema","overglaze-application-install-v1"},{"product","overglaze"},{"root",lab::utf8(root.wstring())},{"version","0.2.0-preview.2"}});
    fs::remove(root/L"app/release-manifest.json");need(allows(root,"update"),"persistent owned marker supports reinstall after uninstall");
    put(root/L"data/settings/application-install.json",lab::json{{"schema","overglaze-application-install-v1"},{"product","overglaze"},{"root",lab::utf8(base.wstring())}});
    need(!allows(root,"update"),"marker for another root refused");fs::remove(root/L"data/settings/application-install.json");

    const auto model_root=base/L"model-app";fs::create_directory(model_root);
    const auto source=base/L"my model 中文.dll";put(source,std::string("Synthetic model fixture, never loaded as DLL"));
    const auto hash=lab::sha256(source);lab::games::Manager manager(model_root,std::vector<lab::games::Policy>{},hash,false);
    const auto imported=manager.import_model(source);need(imported.present&&imported.known&&imported.sha256==hash,"reviewed source copied and recognised");
    need(lab::sha256(source)==hash&&lab::sha256(manager.model_file())==hash,"source and destination bytes match and source untouched");
    need(manager.import_model(source).sha256==hash,"identical destination reused");
    const auto unknown=base/L"unsupported.dll";put(unknown,std::string("Unknown synthetic model"));
    refuses([&]{manager.import_model(unknown);},"unknown version cannot replace the model");need(lab::sha256(manager.model_file())==hash,"refused import preserves destination");
    put(manager.model_file(),std::string("User existing different content"));const auto before=lab::sha256(manager.model_file());
    refuses([&]{manager.import_model(source);},"different existing destination not overwritten");need(lab::sha256(manager.model_file())==before,"different destination preserved");
    const auto hardlink=base/L"alias.dll";need(CreateHardLinkW(hardlink.c_str(),source.c_str(),nullptr)!=FALSE,"hard-link fixture created");
    refuses([&]{manager.import_model(hardlink);},"hard-linked model source refused");fs::remove(hardlink);
    unsigned temporary=0;for(const auto& item:fs::directory_iterator(manager.models_dir()))if(item.path().filename().native().find(L".model-import-")==0)++temporary;
    need(temporary==0,"no import staging files left behind");
    // Only the unique synthetic directory this process created is removed.
    need(base.parent_path()==fs::canonical(temp)&&base.filename().native().find(L"overglaze-maintenance-test-")==0,"cleanup stays in the owned fixture");
    lab::winpath::require_no_reparse(base);for(const auto& item:fs::recursive_directory_iterator(base))lab::winpath::require_no_reparse(item.path());
    const auto removed=fs::remove_all(base);
    std::cout<<"PASS app maintenance/model import: "<<checks<<" checks; synthetic_files_removed="<<removed<<"; GPU=0; games=0; NR=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\nfixture="<<lab::utf8(base.wstring())<<'\n';return 1;}}
