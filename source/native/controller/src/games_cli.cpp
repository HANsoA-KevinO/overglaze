// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// overglaze_games: the adaptation controller's command line. Read-only unless the
// verb says otherwise; never launches a game; writes only Lab-owned files and,
// on install/uninstall, the exact-hash owned files of one game directory.
#include "lab_game_manager.hpp"
#include "lab_attach.hpp"
#include "lab_windows_path.hpp"
#include "lab_brand.hpp"
#include "lab_root_locator.hpp"
#include <iostream>
#include <algorithm>
#include <map>
#include <vector>
using lab::json;
namespace lab{inline bool winpath_same(const std::filesystem::path& a,const std::filesystem::path& b){return winpath::same_spelling(a,b);}}
namespace {
// The program name as shown comes from lab_brand.hpp; the verbs are the
// interface and stay as they are.
const wchar_t* kVerbs=L" <verb> ...\n"
    L"  list                              registry entries with live status (read-only)\n"
    L"  packages                          adapter packages under app/adapters (read-only)\n"
    L"  preflight <exe|dir>               read-only preflight of one game (no registry write)\n"
    L"  add <exe>                         register a game and show its status\n"
    L"  inspect <id|exe>                  status of a registered game\n"
    L"  health                            installed games only: do their files still match the current package? (read-only)\n"
    L"  plan-install <id|exe>             dry run of install: files, sizes, hashes, space checks, stages (read-only)\n"
    L"  reason-codes                      the code table every status, check and stage renders through (read-only)\n"
    L"  make-package <id|exe> [--name n] [--title t] [--viewport N] [--linear-depth] [--host-rebind]\n"
    L"                [--binding-preservation] [--exposure S] [--allow-unsigned-modules] [--late|--root-proxy|--root-proxy-on-insert]\n"
    L"  refresh-package <name> [--root-proxy|--root-proxy-on-insert|--late]\n"
    L"                [--denuvo-passive-coexistence]  re-copy the payload, optionally changing how the game loads us;\n"
    L"                the override must be said again here, exactly as for make-package. Refused while the game has\n"
    L"                Lab files installed: use update, which uninstalls, refreshes and installs in one operation\n"
    L"  install <id|exe> --offline --no-anticheat --consent \"<text>\" [--progress]\n"
    L"                [--denuvo-passive-coexistence]  explicitly accept an anti-tamper EXE; we still never patch it\n"
    L"  uninstall <id|exe> --confirm [--progress]\n"
    L"  update <id|exe> --offline --no-anticheat --consent \"<text>\" [--progress] [--denuvo-passive-coexistence]\n"
    L"                when update_available: refresh the package first if it is behind the published host, then\n"
    L"                uninstall the installed payload and install the package's current one, as one operation\n"
    L"  migrate <id|exe> --offline --no-anticheat --consent \"<text>\" [--progress] [--denuvo-passive-coexistence]\n"
    L"                a game installed by the pre-rename build (DLSS Lab, install state legacy): uninstall its files by\n"
    L"                their recorded hashes, rewrite the package under the new names, install -- as one operation\n"
    L"  model                             the user-supplied NR model: where it is looked for, present, reviewed version (read-only)\n"
    L"  import-model <file>                validate and import your model; existing different files are not overwritten\n"
    L"  app-check update|uninstall --root <folder> [--installer-text]    read-only application maintenance check\n"
    L"  repin <id|exe> --offline --no-anticheat --consent \"<text>\" [--progress] [--allow-unsigned-modules]\n"
    L"                [--denuvo-passive-coexistence]  re-adapt a changed game (controller track only): full preflight,\n"
    L"                uninstall, move the old package to app/adapters-retired/, generate the new one, install\n"
    L"  --progress    write each stage as one JSON line on stderr while install/uninstall/update/repin run\n"
    L"  attach <id|exe> --pid N            inject the late-loading controller into a RUNNING game\n"
    L"  watch [--seconds N] [--once]      wait for late-loading games to appear and attach, once each\n"
    L"  forget <id|exe>\n"
    L"  import-known                      register every package whose game directory holds a Lab install\n"
    L"  storage                           what the manager keeps per game: staging and recovery copies (read-only)\n";
std::wstring usage(){return std::wstring(L"Usage: ")+lab::brand::kGamesCliName+kVerbs;}
}
int wmain(int argc,wchar_t** argv){
    const std::vector<std::wstring> a(argv+1,argv+argc);
    auto has=[&](const wchar_t* f){return std::find(a.begin(),a.end(),f)!=a.end();};
    auto value=[&](const wchar_t* f)->std::wstring{auto it=std::find(a.begin(),a.end(),f);if(it==a.end()||it+1==a.end())return {};return *(it+1);};
    try{
        if(a.empty()||a[0]==L"--help"||a[0]==L"-h"){std::wcerr<<usage();return 2;}
        const auto verb=lab::utf8(a[0]);
        if(verb=="app-check"){
            if(a.size()<2||value(L"--root").empty())throw std::runtime_error("app-check needs update|uninstall and --root");
            const auto result=lab::games::app_maintenance_check(value(L"--root"),lab::utf8(a[1]));
            if(has(L"--installer-text"))std::cout<<result.message<<'\n';else std::cout<<lab::games::app_maintenance_json(result).dump(2)<<'\n';
            return result.allowed?0:2;
        }
        if(verb=="preflight"){if(a.size()<2)throw std::runtime_error("preflight needs a path");
            std::filesystem::path p=a[1];if(std::filesystem::is_directory(p)){const auto d=lab::games::discover(p);if(d.executables.size()!=1)throw std::runtime_error("Directory holds "+std::to_string(d.executables.size())+" executables; pass the game EXE");p=d.executables.front();}
            std::cout<<lab::games::preflight(p).to_json().dump(2)<<'\n';return 0;}
        // Where this program sits decides the Lab root (<root>\app\ or a track
        // build directory), unless overglaze-root.json beside it says otherwise.
        // Preflight above needs no root at all.
        const auto root=lab::root::resolve_self().root;
        lab::games::Manager m(root);
        if(verb=="import-model"){if(a.size()<2)throw std::runtime_error("import-model needs a file");auto result=lab::games::model_json(m.import_model(a[1]));
            result["scope"]="User-selected model validated; identical destination reused or a verified copy imported. No DLL loaded.";
            std::cout<<result.dump(2)<<'\n';return 0;}
        // Said out loud on this command line or not at all. It never changes how
        // we treat the anti-tamper -- no patching, spoofing, debugging or dumping,
        // with or without it -- only whether the refusal was deliberate.
        m.allow_denuvo_passive_coexistence(has(L"--denuvo-passive-coexistence"));
        // The observation-only override applied to an "unsupported-route"
        // verdict that no preflight can produce any more; say so instead of
        // silently ignoring the flag.
        if(has(L"--observe-only"))throw std::runtime_error("--observe-only was removed: no preflight verdict produces the route it applied to (unsupported-route)");
        // Progress of a write operation, one JSON line per stage on stderr; the
        // result stays the single JSON document on stdout.
        lab::games::Progress progress;
        if(has(L"--progress"))progress=[](const lab::games::ProgressEvent& ev){std::cerr<<lab::games::progress_json(ev).dump()<<std::endl;};
        auto entry=[&](const std::wstring& key)->lab::games::Entry{const auto k=lab::utf8(key);for(const auto& e:m.list())if(e.id==k||lab::winpath_same(e.exe,std::filesystem::path(key)))return e;throw std::runtime_error("No registered game matches "+k);};
        json out;
        if(verb=="list"){out=json::array();for(const auto& e:m.list())out.push_back(lab::games::status_json(m.inspect(e)));if(!m.package_notes().empty())out.push_back({{"package_notes",m.package_notes()}});}
        // The health check: every installed game, against the
        // package manifest as it is now. Read-only.
        else if(verb=="health"){json games=json::array();unsigned not_ok=0;
            for(const auto& e:m.list()){const auto s=m.inspect(e);if(!s.installed&&s.install_state!="modified"&&s.install_state!="other-copy"&&s.install_state!="incomplete")continue;
                json reasons=json::array();for(const auto& r:s.reasons)reasons.push_back(lab::games::reason_json(r));
                if(s.health!="ok"&&s.health!="not-applicable")++not_ok;
                games.push_back({{"id",e.id},{"title",e.title},{"state",s.state},{"install",s.install_state},{"health",s.health},
                    {"update",lab::games::status_json(s)["update"]},{"reasons",reasons},{"text",lab::games::describe(s)}});}
            out={{"schema","overglaze-game-health-v1"},{"games",games},{"not_ok",not_ok},
                {"scope","installed games only; each is checked the way its in-game host checks itself at start-up: pinned modules, then host, bridge and config against the current package manifest"}};}
        else if(verb=="plan-install"){if(a.size()<2)throw std::runtime_error("plan-install needs an id or EXE");out=m.plan_install(entry(a[1]).id);}
        else if(verb=="reason-codes")out=lab::games::code_table_json();
        else if(verb=="packages"){out=json::array();for(const auto& p:m.policies())out.push_back(lab::games::policy_json(p));if(!m.package_notes().empty())out.push_back({{"package_notes",m.package_notes()}});}
        else if(verb=="add"){if(a.size()<2)throw std::runtime_error("add needs an EXE path");out=lab::games::status_json(m.inspect(m.add(a[1])));}
        else if(verb=="inspect"){if(a.size()<2)throw std::runtime_error("inspect needs an id or EXE");out=lab::games::status_json(m.inspect(entry(a[1])));}
        else if(verb=="make-package"){if(a.size()<2)throw std::runtime_error("make-package needs an id or EXE");lab::games::PackageOptions o;
            o.name=lab::utf8(value(L"--name"));o.title=lab::utf8(value(L"--title"));if(!value(L"--viewport").empty())o.viewport=std::stoul(value(L"--viewport"));
            o.linear_depth=has(L"--linear-depth");o.native_evaluate_host_rebind=has(L"--host-rebind");o.binding_preservation=has(L"--binding-preservation");
            if(!value(L"--exposure").empty())o.default_exposure_stops=std::stof(value(L"--exposure"));o.allow_unsigned_modules=has(L"--allow-unsigned-modules");
            // --late: the payload goes into <game>\\overglaze\\ and is injected into a
            // running process instead of replacing the game's dxgi.dll. The loader
            // must NOT be called dxgi.dll on this path; the installation contract
            // refuses that pairing, because it is the very import redirection late
            // loading exists to avoid.
            if(has(L"--late")){o.loader.strategy="late_d3d12";o.loader.basename="overglaze_controller.dll";o.loader.subdir="overglaze";}
            // --root-proxy: the same payload layout as --late, plus the thin
            // dxgi.dll in the game directory so the GAME loads us and no
            // injector or resident watcher is needed. This is the main line.
            if(has(L"--root-proxy")){o.loader.strategy="root_proxy_d3d12";o.loader.basename="overglaze_controller.dll";o.loader.subdir="overglaze";}
            // --root-proxy-on-insert: the same files as --root-proxy, but the proxy
            // loads nothing while the game starts; the host arrives on the player's
            // first Insert in the game window (made for RE9).
            if(has(L"--root-proxy-on-insert")){o.loader.strategy="root_proxy_on_insert";o.loader.basename="overglaze_controller.dll";o.loader.subdir="overglaze";}
            if(static_cast<int>(has(L"--late"))+has(L"--root-proxy")+has(L"--root-proxy-on-insert")>1)
                throw std::runtime_error("choose one of --late, --root-proxy or --root-proxy-on-insert");
            const auto e=entry(a[1]);out={{"package",lab::games::policy_json(m.make_package(e.id,o))},{"status",lab::games::status_json(m.inspect(e))}};}
        else if(verb=="refresh-package"){if(a.size()<2)throw std::runtime_error("refresh-package needs a package name");
            // Switching strategy is how a game moves between "the game loads us"
            // and "an injector loads us". Spelled out, never inferred.
            std::string strategy;
            if(static_cast<int>(has(L"--late"))+has(L"--root-proxy")+has(L"--root-proxy-on-insert")>1)
                throw std::runtime_error("choose one of --late, --root-proxy or --root-proxy-on-insert");
            if(has(L"--root-proxy"))strategy="root_proxy_d3d12";
            if(has(L"--root-proxy-on-insert"))strategy="root_proxy_on_insert";
            if(has(L"--late"))strategy="late_d3d12";
            out=lab::games::policy_json(m.refresh_package(lab::utf8(a[1]),strategy));}
        else if(verb=="install"){if(a.size()<2)throw std::runtime_error("install needs an id or EXE");const auto consent=lab::utf8(value(L"--consent"));
            if(!has(L"--offline")||!has(L"--no-anticheat")||consent.empty())throw std::runtime_error("install requires --offline --no-anticheat --consent \"<your own words>\"");
            const auto e=entry(a[1]);m.install(e.id,true,true,consent,progress);out=lab::games::status_json(m.inspect(e));}
        else if(verb=="update"){if(a.size()<2)throw std::runtime_error("update needs an id or EXE");const auto consent=lab::utf8(value(L"--consent"));
            if(!has(L"--offline")||!has(L"--no-anticheat")||consent.empty())throw std::runtime_error("update requires --offline --no-anticheat --consent \"<your own words>\"");
            const auto e=entry(a[1]);m.update(e.id,true,true,consent,progress);out=lab::games::status_json(m.inspect(e));}
        else if(verb=="migrate"){if(a.size()<2)throw std::runtime_error("migrate needs an id or EXE");const auto consent=lab::utf8(value(L"--consent"));
            if(!has(L"--offline")||!has(L"--no-anticheat")||consent.empty())throw std::runtime_error("migrate requires --offline --no-anticheat --consent \"<your own words>\"");
            const auto e=entry(a[1]);m.migrate(e.id,true,true,consent,progress);out={{"reason",lab::games::reason_json({"migrate-done"})},{"status",lab::games::status_json(m.inspect(e))}};}
        else if(verb=="model")out=lab::games::model_json(m.model_status());
        else if(verb=="repin"){if(a.size()<2)throw std::runtime_error("repin needs an id or EXE");const auto consent=lab::utf8(value(L"--consent"));
            if(!has(L"--offline")||!has(L"--no-anticheat")||consent.empty())throw std::runtime_error("repin requires --offline --no-anticheat --consent \"<your own words>\"");
            const auto e=entry(a[1]);const auto retired=m.repin(e.id,true,true,consent,has(L"--allow-unsigned-modules"),progress);out={{"retired_package",lab::utf8(retired.wstring())},{"status",lab::games::status_json(m.inspect(e))}};}
        else if(verb=="uninstall"){if(a.size()<2)throw std::runtime_error("uninstall needs an id or EXE");if(!has(L"--confirm"))throw std::runtime_error("uninstall requires --confirm");
            const auto e=entry(a[1]);m.uninstall(e.id,true,progress);out=lab::games::status_json(m.inspect(e));}
        else if(verb=="attach"){
            // Injecting into a process the user started. Every gate is checked here,
            // before anything is written into another process; the injected DLL
            // refuses again on its own terms if no validated installation sits
            // beside it, so neither side trusts the other.
            if(a.size()<2)throw std::runtime_error("attach needs an id or EXE");
            const auto pid_text=value(L"--pid");
            if(pid_text.empty())throw std::runtime_error("attach needs --pid N; start the game yourself first, this never launches one");
            const auto e=entry(a[1]);const auto s=m.inspect(e);
            const lab::games::Policy* policy=nullptr;
            for(const auto& candidate:m.policies())if(candidate.name==s.package)policy=&candidate;
            if(!policy)throw std::runtime_error("that game has no adapter package");
            if(policy->legacy)throw std::runtime_error("that game is installed by the pre-rename build; run migrate first");
            if(policy->loader.strategy!="late_d3d12")throw std::runtime_error("that package does not use late loading; attach applies to late_d3d12 only");
            if(!s.installed)throw std::runtime_error("install the package first; attach never installs");
            lab::games::AttachGates gates;
            gates.expected_executable=e.exe;
            gates.expected_executable_sha256=policy->pins.at(policy->executable);
            gates.expected_loader_sha256=policy->payload.at(policy->loader.basename);
            const auto loader=(e.exe.parent_path()/policy->loader.subdir/lab::wide(policy->loader.basename)).lexically_normal();
            const auto r=lab::games::attach(static_cast<DWORD>(std::stoul(pid_text)),loader,gates);
            out={{"pid",r.pid},{"executable",lab::utf8(r.executable.wstring())},{"loader",lab::utf8(loader.wstring())},
                 {"loader_sha256",r.loader_sha256},{"loaded",r.loaded},{"note",r.note},
                 {"next","overglazectl <pid> GetStatus confirms the host actually started; loading is not starting."}};
        }
        else if(verb=="watch"){
            // Same gates as attach, just without a human typing the PID. It waits
            // for a registered late-loading game to appear and injects once per
            // process. It never starts a game, never installs, and never touches a
            // game that is not registered with a late_d3d12 package.
            //
            // Retrying matters: most refusals here are the game still booting --
            // d3d12/dxgi not loaded yet, no visible window yet -- so a refusal is
            // reported once and then retried rather than treated as final.
            struct Target {std::string id,package; std::filesystem::path exe,loader; lab::games::AttachGates gates;};
            std::vector<Target> targets;
            for(const auto& e:m.list()){
                const auto s=m.inspect(e);
                if(!s.installed)continue;
                const lab::games::Policy* policy=nullptr;
                for(const auto& candidate:m.policies())if(candidate.name==s.package)policy=&candidate;
                if(!policy||policy->legacy||policy->loader.strategy!="late_d3d12")continue; // a legacy install is migrated first
                Target t;t.id=e.id;t.package=s.package;t.exe=e.exe;
                t.loader=(e.exe.parent_path()/policy->loader.subdir/lab::wide(policy->loader.basename)).lexically_normal();
                t.gates.expected_executable=e.exe;
                t.gates.expected_executable_sha256=policy->pins.at(policy->executable);
                t.gates.expected_loader_sha256=policy->payload.at(policy->loader.basename);
                targets.push_back(std::move(t));
            }
            if(targets.empty())throw std::runtime_error("no installed late-loading game is registered; nothing to watch");
            json listed=json::array();
            for(const auto& t:targets)listed.push_back({{"id",t.id},{"package",t.package},{"executable",lab::utf8(t.exe.wstring())}});
            std::cout<<json{{"event","watching"},{"games",listed},
                {"scope","waits for these to start and injects once per process; never launches, installs or modifies a game"}}.dump()<<std::endl;
            const auto seconds=value(L"--seconds");
            const auto deadline=seconds.empty()?0ULL:GetTickCount64()+std::stoull(seconds)*1000ULL;
            const bool once=has(L"--once");
            std::map<DWORD,std::string> handled;   // pid -> last reported note
            bool any=false;
            for(;;){
                for(const auto& t:targets){
                    const auto pid=lab::games::find_process(t.exe);
                    if(!pid)continue;
                    const auto seen=handled.find(pid);
                    if(seen!=handled.end()&&seen->second=="attached")continue;
                    try{
                        const auto r=lab::games::attach(pid,t.loader,t.gates);
                        handled[pid]="attached";any=true;
                        std::cout<<json{{"event","attached"},{"id",t.id},{"package",t.package},{"pid",r.pid},
                            {"loader_sha256",r.loader_sha256},{"loaded",r.loaded},{"note",r.note},
                            {"next","the host asks for NR preparation itself on this path, then the panel attaches; Insert opens it"}}.dump()<<std::endl;
                    }catch(const std::exception& e){
                        const std::string note=e.what();
                        if(seen==handled.end()||seen->second!=note){
                            handled[pid]=note;
                            std::cout<<json{{"event","waiting"},{"id",t.id},{"pid",pid},{"reason",note}}.dump()<<std::endl;
                        }
                    }
                }
                if(once&&any)break;
                if(deadline&&GetTickCount64()>=deadline)break;
                Sleep(2000);
            }
            std::cout<<json{{"event","stopped"},{"attached_any",any}}.dump()<<std::endl;return any?0:3;
        }
        else if(verb=="forget"){if(a.size()<2)throw std::runtime_error("forget needs an id or EXE");const auto e=entry(a[1]);m.forget(e.id);out={{"forgotten",e.id}};}
        else if(verb=="import-known"){m.import_known_installations();out=json::array();for(const auto& e:m.list())out.push_back(lab::games::status_json(m.inspect(e)));}
        // Read-only: what the manager keeps under data\settings\plugin-manager per
        // registered game, which of it the retention rule manages, and the data disk.
        else if(verb=="storage"){out=lab::games::storage_json(m.storage_usage(),m.list());out["lab_root"]=lab::utf8(root.wstring());}
        else{std::wcerr<<usage();return 2;}
        std::cout<<out.dump(2)<<'\n';return 0;
    }catch(const lab::games::OperationError& e){std::cout<<lab::games::operation_error_json(e).dump(2)<<'\n';return 1;}
    catch(const std::exception& e){std::cout<<json{{"ok",false},{"error",e.what()}}.dump(2)<<'\n';return 1;}
}
