// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_game_manager.hpp"
#include "lab_game_presentation.hpp"
#include "lab_root_locator.hpp"
#include "lab_windows_path.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace fs=std::filesystem;
using lab::json;
namespace {
unsigned checks=0;
void need(bool b,const char* what){++checks;if(!b)throw std::runtime_error(what);}
void need(bool b,const std::string& what){++checks;if(!b)throw std::runtime_error(what);}
template<class F>void reject(F f,const char* what){bool bad=false;try{f();}catch(const std::exception&){bad=true;}need(bad,what);}
void put(const fs::path& path,const std::string& value){std::ofstream f(path,std::ios::binary|std::ios::trunc);f<<value;if(!f)throw std::runtime_error("fixture write");}
json read(const fs::path& path){std::ifstream f(path);return json::parse(f);}
void put16(std::string& d,size_t o,unsigned v){d[o]=char(v&255);d[o+1]=char((v>>8)&255);}
void put32(std::string& d,size_t o,unsigned v){for(unsigned i=0;i<4;++i)d[o+i]=char((v>>(8*i))&255);}
// Minimal x64 PE image: never executed, only parsed. Section names are the
// evidence the preflight reads (Denuvo virtualisation sections).
std::string pe64(const std::vector<std::string>& sections,const std::string& salt){
    std::string d(0x400+sections.size()*0x200,'\0');d[0]='M';d[1]='Z';put32(d,60,0x80);const size_t pe=0x80;d[pe]='P';d[pe+1]='E';
    put16(d,pe+4,0x8664);put16(d,pe+6,unsigned(sections.size()));put16(d,pe+20,0xF0);put16(d,pe+24,0x20b);
    const size_t table=pe+24+0xF0;
    for(size_t i=0;i<sections.size();++i){const size_t s=table+i*40;for(size_t k=0;k<8&&k<sections[i].size();++k)d[s+k]=sections[i][k];
        put32(d,s+8,0x200);put32(d,s+12,unsigned(0x1000*(i+1)));put32(d,s+16,0x200);put32(d,s+20,unsigned(0x400+i*0x200));put32(d,s+36,0x60000020);}
    for(size_t i=0;i<salt.size()&&0x400+i<d.size();++i)d[0x400+i]=salt[i];return d;}
const std::vector<std::string> standard{".text",".rdata",".data",".pdata",".rsrc",".reloc"};
// ---- the status schema: every status the flow below produces
// passes through this. Field set, types, enumerations, and every code is in the
// one table (lab_game_reasons.hpp).
bool in(const std::string& v,std::initializer_list<const char*> set){for(const auto* x:set)if(v==x)return true;return false;}
bool coded(const json& r){return r.is_object()&&r.contains("code")&&r["code"].is_string()&&lab::games::code_text(r["code"].get<std::string>())!=nullptr&&r.contains("params")&&r["params"].is_object();}
void schema(const json& j){
    const std::string at=j.value("title","?")+" "+j.value("state","?");
    need(j.value("schema","")=="overglaze-game-status-v2","status schema: "+at);
    for(const auto* k:{"id","title","exe","state","detail","package","route","track","host_sha256","health"})need(j.contains(k)&&j[k].is_string(),std::string("string field ")+k+": "+at);
    for(const auto* k:{"installed","can_install","can_uninstall","can_make_package","can_refresh_package","running","update_available","can_update","can_repin"})need(j.contains(k)&&j[k].is_boolean(),std::string("bool field ")+k+": "+at);
    need(j["preflight"].is_null()||(j["preflight"].is_object()&&j["preflight"]["schema"]=="overglaze-game-preflight-v3"&&j["preflight"]["checks"].is_array()),"preflight null or v3: "+at);
    const auto& axes=j["axes"];need(axes.is_object()&&axes.size()==3,"three axes: "+at);
    need(in(axes["compatibility"],{"supported","unsupported","unknown"}),"compatibility value: "+at);
    need(in(axes["install"],{"not-installed","installed","update-available","needs-update","game-changed","incomplete","modified","external","research-managed","other-copy","unknown","legacy"}),"install value: "+at);
    need(in(axes["running"],{"running","not-running"})&&(axes["running"]=="running")==j["running"].get<bool>(),"running axis: "+at);
    need(lab::games::known_backend_state(j["state"],axes["install"]),"a listed backend state: "+at+"/"+axes["install"].get<std::string>());
    need(j["load_mode"].is_null()||in(j["load_mode"],{"root_dxgi_minimal","root_proxy_d3d12","root_proxy_on_insert","late_d3d12","reframework_plugin"}),"load mode: "+at);
    if(j["load_mode"].is_string())need(lab::games::code_text("load-mode."+j["load_mode"].get<std::string>())!=nullptr,"load mode named: "+at);
    const auto& u=j["update"];need(u.is_object()&&u.size()==6,"update parts: "+at);
    for(const auto* k:{"available","host","bridge","config","proxy","package_behind_published"})need(u[k].is_boolean(),std::string("update.")+k+": "+at);
    need(u["available"]==j["update_available"],"update.available mirrors update_available: "+at);
    need(in(j["health"],{"ok","needs-update","game-changed","unknown","not-applicable"}),"health value: "+at);
    need(j["reasons"].is_array()&&!j["reasons"].empty(),"at least one reason: "+at);
    for(const auto& r:j["reasons"])need(coded(r),"reason code in the table: "+at+" "+r.dump());
    need(j["checks"].is_array(),"checks: "+at);
    for(const auto& c:j["checks"]){need(c["name"].is_string()&&lab::games::code_text("check."+c["name"].get<std::string>())!=nullptr,"check name in the table: "+at+" "+c.dump());
        need(c["ok"].is_boolean()||c["ok"].is_null(),"check ok is true/false/null: "+at);
        if(c["ok"]==true)need(c["reason"].is_null(),"a pass carries no reason: "+at+" "+c.dump());
        else need(c["reason"].is_string()&&lab::games::code_text(c["reason"].get<std::string>())!=nullptr,"a non-pass names its reason: "+at+" "+c.dump());}
    need(j["refusals"].is_object(),"refusals: "+at);for(const auto& [k,v]:j["refusals"].items())need(coded(v),"refusal code in the table: "+at+" "+k);
    const auto& pr=j["presentation"];need(pr.is_object()&&lab::games::code_text(pr["label"].get<std::string>())!=nullptr&&in(pr["tone"],{"success","accent","warning","error","neutral","info"}),"presentation: "+at);
    for(const auto& a:pr["actions"]){need(lab::games::code_text(a["label"].get<std::string>())!=nullptr,"action label: "+at);need(a["enabled"].is_boolean()&&(a["enabled"]==true)==a["why_not"].is_null(),"disabled action says why: "+at);
        if(a["why_not"].is_object())need(coded(a["why_not"]),"why-not in the table: "+at);}
    need(j["detail"].get<std::string>().find("[")==std::string::npos,"detail renders without unknown codes: "+at);
    need(j["state"]!="unsupported-route","the unreachable state is gone: "+at);}
std::set<std::string> reached; // state/install pairs the flow below produced
lab::games::Status st(lab::games::Manager& m,const lab::games::Entry& e){auto s=m.inspect(e);schema(lab::games::status_json(s));reached.insert(s.state+"/"+s.install_state);return s;}
bool has_reason(const lab::games::Status& s,const char* code){for(const auto& r:s.reasons)if(r.code==code)return true;return false;}
const lab::games::Check* check_named(const std::vector<lab::games::Check>& v,const char* name){for(const auto& c:v)if(c.name==name)return &c;return nullptr;}
// Top-level stages an operation reported as started, in order.
std::vector<std::string> started(const std::vector<lab::games::ProgressEvent>& ev,const std::string& op,const std::string& parent){std::vector<std::string> out;
    for(const auto& e:ev)if(e.operation==op&&e.parent==parent&&e.status=="start")out.push_back(e.stage);return out;}
}
int wmain(int argc,wchar_t** argv){fs::path root;try{
    // Read-only inspection against the real Lab, whose root comes from where this
    // test program sits (a track build directory), never from a literal.
    if(argc==3&&std::wstring(argv[1])==L"--inspect"){lab::games::Manager manager(lab::root::resolve_self().root);lab::games::Entry e{lab::uuid(),"Read-only inspection",argv[2]};std::cout<<lab::games::status_json(manager.inspect(e)).dump(2);return 0;}
    if(argc!=1)throw std::runtime_error("Unknown fixture argument");
    // The synthetic Lab lives in OVERGLAZE_TEST_TEMP when set, else in the user's
    // temp directory: nothing here depends on where the checkout or build is.
    const auto base=[]{std::wstring v(32768,L'\0');auto n=GetEnvironmentVariableW(L"OVERGLAZE_TEST_TEMP",v.data(),DWORD(v.size()));
        if(!n||n>=v.size()){n=GetTempPathW(DWORD(v.size()),v.data());need(n&&n<v.size(),"temp path");}
        v.resize(n);const auto p=fs::canonical(fs::path(v));need(fs::is_directory(p),"fixture base is a directory");lab::winpath::require_no_reparse(p);return p;}();
    root=base/lab::wide("overglaze-manager-test-"+lab::uuid());need(fs::create_directory(root),"new unique fixture root");
    // No docs directory: the manager must not create one.
    for(const auto* p:{L"app",L"app/plugin",L"app/research",L"app/research/host",L"app/tools",L"app/models",L"app/adapters",L"data",L"game",L"game2",L"game3",L"game4",L"game5",L"game6"})fs::create_directory(root/p);
    put(root/L"PURPOSE.json",json{{"purpose","functional-verification"},{"synthetic",true},{"game_started",false}}.dump());
    const auto game_bytes=pe64(standard,"synthetic game; never executed");
    put(root/L"game/TestGame.exe",game_bytes);put(root/L"game/sl.interposer.dll","synthetic SL interposer");put(root/L"game/sl.common.dll","synthetic SL common");put(root/L"game/sl.dlss_d.dll","synthetic SL dlss_d");
    // Two published hosts with DIFFERENT bytes. app/plugin is the controller
    // track, app/research/host is the research track. Packages default to
    // research, so a package that ever picks up the controller host is a bug the
    // fixture must catch rather than hide behind one shared file.
    put(root/L"app/plugin/dxgi.dll","synthetic CONTROLLER host");put(root/L"app/plugin/overglaze_nvngx.dll","synthetic CONTROLLER bridge");
    put(root/L"app/research/host/dxgi.dll","synthetic host");put(root/L"app/research/host/overglaze_nvngx.dll","synthetic bridge");
    // The "installation checker" is a copy of this test program: a real image that,
    // given the checker's two arguments, refuses at once (exit 1, "Unknown fixture
    // argument"). That makes the checker-refusal path deterministic; a non-image
    // file here made CreateProcessW's failure mode depend on the environment.
    {std::wstring self(32768,L'\0');const auto n=GetModuleFileNameW(nullptr,self.data(),DWORD(self.size()));need(n&&n<self.size(),"module path");self.resize(n);
     need(CopyFileW(self.c_str(),(root/L"app/tools/overglaze_install_check.exe").c_str(),TRUE)!=FALSE,"checker fixture");}
    put(root/L"app/models/nvngx_dlssnr.dll","synthetic model");
    const auto model_hash=lab::sha256(root/L"app/models/nvngx_dlssnr.dll");
    lab::games::Manager m(root,std::nullopt,model_hash,false);need(m.list().empty()&&m.policies().empty(),"empty registry and no packages");
    // ---- unknown Streamline-RR game: preflight verdict, no side effects
    auto e=m.add(root/L"game/TestGame.exe");need(m.add(root/L"game/TestGame.exe").id==e.id&&m.list().size()==1,"duplicate add idempotent");
    {auto s=st(m,e);need(s.state=="needs-package"&&s.can_make_package&&!s.can_install&&s.preflight.is_object(),"unknown RR game needs a package");
     need(s.preflight["route"]=="sl-rr"&&s.preflight["verdict"]=="sl-rr-ready"&&s.preflight["pe_valid"]==true&&s.preflight["pe_x64"]==true&&s.preflight["denuvo_suspected"]==false,"preflight reads the PE and the modules");
     need(s.preflight["modules_signed"]==false&&s.preflight["modules"].contains("sl.dlss_d.dll")&&s.preflight["store"]=="unknown","synthetic modules are unsigned and store unknown");
     need(s.preflight["sections"].size()==6&&s.preflight["executable_sha256"]==lab::sha256(root/L"game/TestGame.exe"),"sections and executable hash reported");}
    reject([&]{m.make_package(e.id,{});},"unsigned Streamline modules need explicit acknowledgement");need(!fs::exists(root/L"app/adapters/testgame"),"no package written on refusal");
    reject([&]{m.install(e.id,true,true,"x");},"no package: cannot install");
    // ---- generate the package from the published host
    lab::games::PackageOptions opt;opt.allow_unsigned_modules=true;opt.viewport=2;opt.default_exposure_stops=1.5f;opt.linear_depth=true;
    auto policy=m.make_package(e.id,opt);
    const auto pkg=root/L"app/adapters/testgame";
    need(policy.name=="testgame"&&policy.profile=="testgame-rr-v1"&&policy.directory==pkg&&policy.game_root==root/L"game","package named from the executable");
    for(const auto* n:{L"package.json",L"overglaze.install.json",L"dxgi.dll",L"overglaze_nvngx.dll",L"overglaze_install_check.exe"})need(fs::is_regular_file(pkg/n),"package file present");
    {auto man=read(pkg/L"package.json");auto cfg=read(pkg/L"overglaze.install.json");
     need(man["schema"]=="overglaze-adapter-package-v1"&&man["name"]=="testgame"&&man["pins"].size()==4&&man["pins"]["TestGame.exe"]==lab::sha256(root/L"game/TestGame.exe")&&man["pins"].contains("sl.dlss_d.dll"),"manifest pins exe and modules");
     need(man["payload"]["dxgi.dll"]==lab::sha256(root/L"app/research/host/dxgi.dll")&&man["payload"]["nvngx_dlssnr.dll"]==model_hash&&man["config_sha256"]==lab::sha256(pkg/L"overglaze.install.json")&&man["checker_sha256"]==lab::sha256(root/L"app/tools/overglaze_install_check.exe"),"manifest carries payload, checker and config hashes");
     need(man["facts"]["viewport"]==2&&man["facts"]["linear_depth"]==true&&man["reviewed_row"]==false&&man["preflight"]["verdict"]=="sl-rr-ready","default-hypothesis facts recorded with the preflight");
     // The track is recorded, defaults to research, and the payload really came
     // from the research host, not from app/plugin.
     need(man["track"]=="research","generated package is research-track");
     need(man["payload"]["dxgi.dll"]!=lab::sha256(root/L"app/plugin/dxgi.dll"),"a research package never carries the controller host");
     need(policy.track=="research","policy reports the package track");
     need(cfg["version"]==3&&cfg.size()==12&&cfg["package"]=="testgame"&&cfg["profile"]=="testgame-rr-v1"&&cfg["facts"]["executable"]=="TestGame.exe"&&cfg["facts"]["settings_file"]=="overlay-testgame.json"&&
          cfg["facts"]["capture_origin"]=="testgame-controlled-rr-stage"&&cfg["facts"]["modules"].size()==3&&cfg["exception_diagnostics"]==false&&cfg["output_root"]==lab::utf8((root/L"data").wstring()),"V3 config generated");
     need(std::fabs(cfg["facts"]["default_exposure_stops"].get<float>()-1.5f)<1e-6f,"exposure option recorded");}
    reject([&]{m.make_package(e.id,opt);},"existing package directory never overwritten");
    need(m.policies().size()==1&&st(m,e).state=="available"&&st(m,e).package=="testgame"&&st(m,e).can_install,"generated package makes the game available");
    // ---- consent, conflicts, foreign loaders, single writer
    reject([&]{m.install(e.id,false,true,"c");},"offline consent mandatory");reject([&]{m.install(e.id,true,false,"c");},"no AC consent mandatory");reject([&]{m.install(e.id,true,true,"");},"consent text mandatory");need(!fs::exists(root/L"game/dxgi.dll"),"no side effects without confirmation");
    put(root/L"game/ReShade.ini","existing user mod");need(!st(m,e).can_install&&st(m,e).state=="blocked","loader conflict rejected");
    {auto s=st(m,e);need(s.reasons.front().code=="loader-conflict"&&s.install_state=="not-installed"&&s.reasons.front().params["files"].size()==1&&lab::games::present(s).label=="label.loader-conflict","a blocked state names its refusal");}fs::remove(root/L"game/ReShade.ini");
    put(root/L"game/dxgi.dll","another loader");need(!st(m,e).can_install&&!st(m,e).can_uninstall,"foreign host no overwrite/delete");need(st(m,e).reasons.front().code=="foreign-loader","a foreign loader is named");reject([&]{m.install(e.id,true,true,"c");},"foreign install refused");need(fs::file_size(root/L"game/dxgi.dll")==14,"foreign bytes preserved");fs::remove(root/L"game/dxgi.dll");
    put(pkg/L"dxgi.dll","tampered package");need(!st(m,e).can_install,"package payload hash bound");need(st(m,e).reasons.front().code=="package-payload-mismatch","a tampered package is named");fs::remove(pkg/L"dxgi.dll");fs::copy_file(root/L"app/research/host/dxgi.dll",pkg/L"dxgi.dll");
    const auto store=root/L"data/settings/plugin-manager",tx=store/lab::wide(e.id)/L"transaction.json";
    {lab::Handle other(CreateFileW((store/L"writer.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,0,nullptr));need(other.valid(),"lock fixture acquired");reject([&]{m.install(e.id,true,true,"c");},"single writer enforcement");}
    // ---- stale package: published host moved on
    put(root/L"app/research/host/dxgi.dll","synthetic host v2");
    {auto s=st(m,e);need(s.state=="package-stale"&&s.can_refresh_package&&!s.can_install,"package lagging the published host is not installable");
     need(s.reasons.front().code=="package-stale"&&check_named(s.checks,"published")&&check_named(s.checks,"published")->outcome==lab::games::Outcome::fail,"stale is a named reason and a failed check");}
    reject([&]{m.refresh_package("nope");},"unknown package cannot refresh");
    {auto man=read(pkg/L"package.json");man["consent"]="original consent words";put(pkg/L"package.json",man.dump());}
    lab::games::Manager reloaded(root,std::nullopt,model_hash,false);auto refreshed=reloaded.refresh_package("testgame");need(refreshed.payload.at("dxgi.dll")==lab::sha256(root/L"app/research/host/dxgi.dll")&&read(pkg/L"package.json")["notes"].size()==2&&read(pkg/L"package.json")["facts"]["viewport"]==2&&read(pkg/L"package.json")["consent"]=="original consent words","refresh re-copies payload, keeps facts and consent");
    m=lab::games::Manager(root,std::nullopt,model_hash,false);
    need(st(m,e).state=="available","refreshed package available again");
    // ---- a failed install keeps its staging copy for inspection (retention)
    const auto box=store/lab::wide(e.id);
    auto made=[&](const wchar_t* prefix){std::vector<fs::path> v;std::error_code ec;if(fs::is_directory(box,ec))for(auto& d:fs::directory_iterator(box))if(d.is_directory()&&d.path().filename().wstring().starts_with(prefix))v.push_back(d.path());std::sort(v.begin(),v.end());return v;};
    {// The package's checker is a text file here, so the real checker run refuses
     // AFTER staging and activation: the documented failure path.
     lab::games::Manager checked(root,std::nullopt,model_hash,true);
     reject([&]{checked.install(e.id,true,true,"fixture consent: checker refusal");},"a refused checker fails the install");
     need(read(tx)["state"]=="removed"&&!fs::exists(root/L"game/dxgi.dll")&&!fs::exists(root/L"game/overglaze.install.json"),"the refused install removed what it copied");}
    const auto failed_staging=made(L"package-");
    need(failed_staging.size()==1&&fs::is_regular_file(failed_staging[0]/L"dxgi.dll")&&fs::is_regular_file(failed_staging[0]/L"overglaze.install.json"),"the failed install's staging is kept");
    need(!fs::exists(box/L"install-receipt.json"),"no second receipt for a failed install");
    // A recovery copy made before the retention rule existed: never in the ledger,
    // so no later uninstall may delete it (the existing ones are kept until the
    // owner decides what to do with them).
    const auto legacy_copy=box/lab::wide("uninstall-"+lab::uuid());need(fs::create_directory(legacy_copy),"legacy recovery copy");put(legacy_copy/L"dxgi.dll","pre-rule recovery copy");
    // ---- install: transaction + second receipt beside it, in the manager's own store
    m.install(e.id,true,true,"fixture consent: offline single-player, no anti-cheat");
    {auto s=st(m,e);need(s.state=="installed"&&s.installed&&s.can_uninstall&&!s.can_install,"installed state");need(read(tx)["files"].size()==4&&read(tx)["package"]=="testgame","new model owned, package recorded");
     need(lab::sha256(root/L"game/dxgi.dll")==refreshed.payload.at("dxgi.dll")&&lab::sha256(root/L"game/overglaze.install.json")==refreshed.config_sha256,"installed payload exact");
     auto docs=read(box/L"install-receipt.json");need(docs["state"]=="installed"&&docs["after"].size()==4&&docs["game_pins"].size()==4&&docs["package"]=="testgame"&&docs["adopted"]==false,"second receipt in the Manage-LabGame schema, beside the transaction");
     need(!fs::exists(root/L"docs"),"the manager creates no docs directory in the program root");
     need(made(L"package-")==failed_staging&&read(tx)["recovery"].is_null()&&read(tx)["staging_removed"].is_string()&&!fs::exists(fs::path(lab::wide(read(tx)["staging_removed"].get<std::string>()))),"a successful install removes its own staging, and only that");}
    auto managed_copies=[&]{auto v=made(L"uninstall-");v.erase(std::remove(v.begin(),v.end(),legacy_copy),v.end());return v;};
    need(managed_copies().empty(),"no recovery copy before any uninstall");
    // ---- update: a newer published host reaches the installed game through its package
    need(!st(m,e).update_available,"a current install reports no update");
    reject([&]{m.update(e.id,true,true,"c");},"no update without a newer package");
    std::vector<fs::path> copies_seen; // recovery copies in the order the uninstalls made them
    {auto s=st(m,e);need(s.install_state=="installed"&&s.health=="ok"&&!s.can_update&&has_reason(s,"installed")&&has_reason(s,"launch-root-layout"),"a current install: health ok, how to start it");
     need(lab::games::present(s).label=="label.installed","a current install is labelled current");}
    for(const char* host:{"synthetic host v3","synthetic host v2"}){
        put(root/L"app/research/host/dxgi.dll",host);
        m=lab::games::Manager(root,std::nullopt,model_hash,false);
        // Refreshing the package under the installed game would leave a
        // game whose host refuses to start; the refresh is refused instead.
        {const auto before=lab::sha256(pkg/L"package.json");reject([&]{m.refresh_package("testgame");},"no refresh under an installed game");need(lab::sha256(pkg/L"package.json")==before,"the refused refresh wrote nothing");}
        // The installed game sees that its package is behind the published host.
        {auto s=st(m,e);need(s.state=="installed"&&s.update_available&&s.install_state=="update-available"&&s.update.package_behind_published&&!s.update.host&&s.health=="ok"&&s.can_update,
             "an installed game whose package is behind the published host has an update and still works");
         need(s.reasons.front().code=="update-available"&&has_reason(s,"package-behind-published")&&lab::games::present(s).label=="label.update-available"&&lab::games::present(s).dot,"有更新, with the dot");}
        std::vector<lab::games::ProgressEvent> ev;const lab::games::Progress cb=[&](const lab::games::ProgressEvent& x){ev.push_back(x);};
        m.update(e.id,true,true,"fixture consent: offline single-player, no anti-cheat",cb);
        {auto s=st(m,e);need(s.state=="installed"&&!s.update_available&&s.install_state=="installed"&&lab::sha256(root/L"game/dxgi.dll")==lab::sha256(root/L"app/research/host/dxgi.dll"),
             "update installs the package's current host");
         need(lab::sha256(pkg/L"dxgi.dll")==lab::sha256(root/L"app/research/host/dxgi.dll"),"one operation: the update refreshed the package itself");}
        // Progress: the update's own stages, then each nested operation's, in order.
        need(started(ev,"update","")==lab::games::operation_stages("update"),"update reports every stage in order");
        {auto expected=lab::games::operation_stages("install");expected.erase(std::find(expected.begin(),expected.end(),"verify"));
         need(started(ev,"install","update")==expected,"the nested install reports its stages (verify skipped: no checker in this fixture)");}
        need(started(ev,"uninstall","update")==lab::games::operation_stages("uninstall"),"the nested uninstall reports its stages");
        for(const auto& x:ev)need(x.status!="failed","a successful update reports no failure");
        // An update is uninstall + install: one recovery copy, and no staging left.
        for(const auto& c:managed_copies())if(std::find(copies_seen.begin(),copies_seen.end(),c)==copies_seen.end())copies_seen.push_back(c);
        need(made(L"package-")==failed_staging,"an update leaves no staging behind");
    }
    need(copies_seen.size()==2&&managed_copies().size()==2,"two updates, two recovery copies");
    need(lab::sha256(root/L"game/dxgi.dll")==refreshed.payload.at("dxgi.dll"),"back on the v2 host the assertions below expect");
    reject([&]{m.install(e.id,true,true,"c");},"duplicate install rejected");reject([&]{m.forget(e.id);},"cannot forget active ownership");reject([&]{m.uninstall(e.id,false);},"uninstall confirmation mandatory");
    put(root/L"game/dxgi.dll","modified foreign bytes");need(!st(m,e).can_uninstall,"tamper detected");need(st(m,e).install_state=="modified"&&st(m,e).reasons.front().code=="installed-files-modified","tamper is named");reject([&]{m.uninstall(e.id,true);},"no destructive tamper recovery");fs::remove(root/L"game/dxgi.dll");fs::copy_file(pkg/L"dxgi.dll",root/L"game/dxgi.dll");
    auto valid=read(tx),forged=valid;forged["files"]["../important.txt"]=std::string(64,'a');put(tx,forged.dump());need(!st(m,e).can_uninstall,"receipt path traversal refused");need(st(m,e).reasons.front().code=="transaction-invalid","a forged receipt is named");put(tx,valid.dump());
    put(root/L"game/user-save.dat","user save");m.uninstall(e.id,true);need(read(tx)["state"]=="removed"&&!fs::exists(root/L"game/dxgi.dll")&&!fs::exists(root/L"game/nvngx_dlssnr.dll"),"owned files uninstalled");need(fs::exists(root/L"game/user-save.dat")&&fs::exists(root/L"game/TestGame.exe")&&fs::exists(root/L"game/sl.interposer.dll"),"game and user data preserved");
    need(read(box/L"install-receipt.json")["state"]=="uninstalled","the second receipt follows the uninstall");
    const auto backup=fs::path(lab::wide(read(tx)["recovery"]));need(fs::is_regular_file(backup/L"dxgi.dll")&&lab::sha256(backup/L"dxgi.dll")==refreshed.payload.at("dxgi.dll"),"verified recovery copy");need(st(m,e).can_install,"can reinstall after uninstall");
    // ---- retention: after the THIRD uninstall only the newest two recovery copies remain
    {const auto now=managed_copies();
     need(now.size()==2&&!fs::exists(copies_seen[0])&&fs::exists(copies_seen[1])&&fs::exists(backup)&&std::find(now.begin(),now.end(),backup)!=now.end(),"third uninstall: the oldest ledger copy is gone, the newest two stay");
     need(fs::is_regular_file(legacy_copy/L"dxgi.dll"),"a recovery copy made before the rule is never deleted by it");
     const auto ledger=read(box/L"retention.json");need(ledger["schema"]=="overglaze-manager-retention-v1"&&ledger["recovery_copies"].size()==2&&ledger["pruned"]==1,"ledger records the two kept copies and one pruned");}
    // ---- the model is kept once: recovery copies refer to one shared copy by hash
    {for(const auto& c:managed_copies())need(!fs::exists(c/L"nvngx_dlssnr.dll"),"a recovery copy does not carry the model");
     const auto where=read(tx)["recovery_shared"]["nvngx_dlssnr.dll"];
     need(where["sha256"]==model_hash&&fs::path(lab::wide(where["path"].get<std::string>()))==store/L"_models"/lab::wide(model_hash)/L"nvngx_dlssnr.dll","the transaction names the shared model by hash");
     std::size_t shared=0;for(const auto& d:fs::directory_iterator(store/L"_models")){++shared;need(lab::sha256(d.path()/L"nvngx_dlssnr.dll")==model_hash,"the shared model is intact");}
     need(shared==1,"three uninstalls, one shared model copy");
     const auto u=m.storage_usage();need(u.shared_models==1&&u.shared_model_bytes==fs::file_size(root/L"app/models/nvngx_dlssnr.dll"),"storage counts the shared model once");}
    // ---- the cross-tool guard reads the legacy per-package install receipt that the
    // research tools' install script (Manage-LabGame.ps1) keeps inside the program root
    fs::create_directories(root/L"docs"/L"maintenance");const auto legacy=root/L"docs"/L"maintenance"/L"testgame-install.json";
    const auto game_dir=lab::utf8((root/L"game").wstring());
    put(legacy,json{{"version",1},{"state","installed"},{"game_directory",game_dir},{"package","testgame"}}.dump(2));
    reject([&]{m.install(e.id,true,true,"c");},"a legacy receipt that still says installed blocks the install");need(!fs::exists(root/L"game/dxgi.dll"),"nothing written past the guard");
    put(legacy,json{{"version",1},{"state","uninstalled"},{"game_directory",game_dir},{"package","testgame"}}.dump(2));const auto legacy_bytes=lab::sha256(legacy);
    // ---- reinstall archives the manager's own old receipt instead of replaying it; the legacy one is only read
    m.install(e.id,true,true,"second consent");unsigned archived=0;for(auto& f:fs::directory_iterator(box))if(f.path().filename().wstring().starts_with(L"install-receipt-uninstalled-"))++archived;
    // Three: one from each of the two updates above, and this reinstall's.
    need(archived==3&&read(box/L"install-receipt.json")["state"]=="installed"&&read(box/L"install-receipt.json")["consent"]=="second consent","old receipt archived, new one written");
    need(lab::sha256(legacy)==legacy_bytes&&std::distance(fs::directory_iterator(root/L"docs"/L"maintenance"),fs::directory_iterator{})==1,"install neither moves nor rewrites the legacy receipt or anything beside it");
    // A legacy receipt left saying "installed" for this game (as an older manager
    // wrote it) is set to "uninstalled" once, so it never blocks the next install.
    put(legacy,json{{"version",1},{"state","installed"},{"game_directory",game_dir},{"package","testgame"}}.dump(2));
    m.uninstall(e.id,true);
    need(read(legacy)["state"]=="uninstalled"&&read(legacy)["uninstalled_by"]=="overglaze-games native controller","the transitional legacy receipt is kept truthful");
    need(managed_copies().size()==2&&fs::exists(backup)&&!fs::exists(copies_seen[1]),"every later uninstall keeps exactly the newest two");
    // ---- existing identical model is used but never acquired as owned content
    fs::copy_file(root/L"app/models/nvngx_dlssnr.dll",root/L"game/nvngx_dlssnr.dll");m.install(e.id,true,true,"c");need(read(tx)["files"].size()==3,"reused model not owned");m.uninstall(e.id,true);need(fs::exists(root/L"game/nvngx_dlssnr.dll"),"reused model survives uninstall");
    // ---- interrupted install: explicit cleanup, never automatic retry
    m.install(e.id,true,true,"c");auto interrupted=read(tx);interrupted["state"]="installing";put(tx,interrupted.dump());fs::remove(root/L"game/dxgi.dll");need(st(m,e).state=="incomplete"&&st(m,e).can_uninstall,"interrupted install recoverable");m.uninstall(e.id,true);need(!fs::exists(root/L"game/overglaze_nvngx.dll"),"partial cleanup");
    // ---- game update: pins fail, nothing installs or enables; safe removal stays
    m.install(e.id,true,true,"c");put(root/L"game/TestGame.exe",pe64(standard,"updated game"));need(st(m,e).state=="changed"&&st(m,e).can_uninstall,"game update does not prevent safe removal");m.uninstall(e.id,true);
    need(st(m,e).state=="changed"&&!st(m,e).can_install&&!st(m,e).can_make_package,"updated game cannot reinstall against the old package");
    {auto s=st(m,e);need(!s.can_repin&&s.refusals.contains("repin")&&s.refusals.at("repin").code=="repin-research-track"&&s.install_state=="game-changed","a research-track package is never repinned here");
     const auto view=lab::games::present(s);need(std::none_of(view.actions.begin(),view.actions.end(),[](const lab::games::Action& a){return a.name=="repin";}),"and the page does not offer it");
     reject([&]{m.repin(e.id,true,true,"c",true);},"repin refuses a research-track package");}put(root/L"game/TestGame.exe",game_bytes);need(st(m,e).state=="available","restored build available again");
    // ---- existing installation without our transaction is recognised (adopted), removed by handle
    m.install(e.id,true,true,"c");fs::remove(tx);need(st(m,e).state=="existing"&&st(m,e).can_uninstall&&st(m,e).host_hash==refreshed.payload.at("dxgi.dll"),"legacy install recognized through the package");
    {auto s=st(m,e);need(s.install_state=="research-managed"&&has_reason(s,"existing-research")&&s.load_mode=="root_dxgi_minimal","a research install is research-managed, with no script to run");
     const auto view=lab::games::present(s);need(view.label=="label.research-managed"&&std::none_of(view.actions.begin(),view.actions.end(),[](const lab::games::Action& a){return a.writes;}),"and read-only on the page");}
    m.uninstall(e.id,true);need(!fs::exists(root/L"game/dxgi.dll")&&fs::exists(root/L"game/nvngx_dlssnr.dll"),"adoption removes Lab files, preserves pre-existing model");
    // ---- verdicts for games the controller must refuse or defer
    put(root/L"game2/Denuvo.exe",pe64({".text",".xtext",".xcode",".rdata",".xtls"},"denuvo-like"));put(root/L"game2/sl.interposer.dll","sl");put(root/L"game2/sl.dlss_d.dll","dlss_d");
    auto d=m.add(root/L"game2/Denuvo.exe");{auto s=st(m,d);need(s.state=="denuvo-blocked"&&!s.can_make_package&&!s.can_install&&s.preflight["denuvo_suspected"]==true&&s.route=="sl-rr","Denuvo sections block before any install");
     need(check_named(s.checks,"denuvo")&&check_named(s.checks,"denuvo")->outcome==lab::games::Outcome::fail&&s.compatibility=="unsupported"&&lab::games::present(s).tone==lab::games::Tone::neutral,"Denuvo: a failed check, unsupported, grey not red");}
    reject([&]{lab::games::PackageOptions o;o.allow_unsigned_modules=true;m.make_package(d.id,o);},"Denuvo game gets no package");
    // NGX-direct + Denuvo (LEGO Batman's shape). The passive-coexistence
    // override must apply to this route too, not just sl-rr, once said out loud.
    // It is still refused WITHOUT the flag, and the flag changes nothing but
    // that refusal.
    put(root/L"game6/DenuvoNgx.exe",pe64({".text",".xtext",".xcode",".rdata",".xtls"},"denuvo-ngx"));
    put(root/L"game6/nvngx_dlss.dll","dlss");put(root/L"game6/nvngx_dlssd.dll","dlssd");
    auto dn=m.add(root/L"game6/DenuvoNgx.exe");
    {auto s=st(m,dn);need(s.state=="denuvo-blocked"&&s.route=="ngx-rr"&&!s.can_make_package,"an NGX-direct Denuvo game is blocked by default");}
    reject([&]{lab::games::PackageOptions o;o.allow_unsigned_modules=true;m.make_package(dn.id,o);},"NGX Denuvo game gets no package without the override");
    m.allow_denuvo_passive_coexistence(true);
    {lab::games::PackageOptions o;o.allow_unsigned_modules=true;o.name="denuvo-ngx";
     const auto pkg=m.make_package(dn.id,o);need(pkg.route=="ngx-rr","the override packages an NGX-direct Denuvo game");}
    m.allow_denuvo_passive_coexistence(false);
    // Leave the registry and the package set exactly as the assertions below expect.
    m.forget(dn.id);fs::remove_all(root/L"app/adapters/denuvo-ngx");
    put(root/L"game3/Cheat.exe",pe64(standard,"cheat"));put(root/L"game3/sl.interposer.dll","sl");put(root/L"game3/sl.dlss_d.dll","dlss_d");put(root/L"game3/EasyAntiCheat_EOS.dll","eac");
    auto c=m.add(root/L"game3/Cheat.exe");{auto s=st(m,c);need(s.state=="anticheat-blocked"&&!s.can_make_package&&s.preflight["anticheat_markers"].size()==1,"anti-cheat marker blocks");
     need(check_named(s.checks,"anticheat")->outcome==lab::games::Outcome::fail&&s.reasons.front().params["markers"].size()==1,"the marker is a failed check and a named reason");}
    put(root/L"game4/SrOnly.exe",pe64(standard,"sr"));put(root/L"game4/sl.interposer.dll","sl");put(root/L"game4/sl.dlss.dll","dlss");
    auto sr=m.add(root/L"game4/SrOnly.exe");{auto s=st(m,sr);need(s.state=="needs-package"&&s.route=="sl-sr"&&s.can_make_package,"an SR-only game can be packaged now");}
    put(root/L"game4/sl.common.dll","common");
    {lab::games::PackageOptions o;o.allow_unsigned_modules=true;o.name="sr-only";
     const auto pkg=m.make_package(sr.id,o);
     need(pkg.route=="sl-sr"&&pkg.facts.modules.contains("sl.dlss.dll")&&!pkg.facts.modules.contains("sl.dlss_d.dll"),"an SR package pins the SR plugin it drives");
     need(pkg.facts.capture_origin=="sr-only-controlled-sr-stage","its capture origin carries the SR stage the installation contract derives");}
    // The viewer's "生成适配包" asks for the controller root proxy:
    // never the research root layout that the default PackageOptions still means.
    put(root/L"app/plugin/overglaze_controller.dll","synthetic CONTROLLER loader");
    {auto o=lab::games::PackageOptions::controller_root_proxy();
     need(o.loader.strategy=="root_proxy_d3d12"&&o.loader.basename=="overglaze_controller.dll"&&o.loader.subdir==L"overglaze","the viewer asks for the controller root proxy");
     o.allow_unsigned_modules=true;o.name="viewer-made";
     const auto pkg=m.make_package(sr.id,o);
     need(pkg.loader.root_proxy()&&!pkg.loader.root()&&pkg.track=="controller","a viewer-made package is a controller root-proxy package, not the research root layout");
     need(pkg.payload.contains("overglaze_controller.dll"),"it carries the controller host");}
    fs::remove_all(root/L"app/adapters/viewer-made");fs::remove(root/L"app/plugin/overglaze_controller.dll");
    // Leave the registry and the package set exactly as the assertions below expect.
    fs::remove_all(root/L"app/adapters/sr-only"); // the registry entry is forgotten with the others below
    put(root/L"game5/Plain.exe",pe64(standard,"plain"));auto plain=m.add(root/L"game5/Plain.exe");need(st(m,plain).state=="no-dlss"&&st(m,plain).route=="none","no DLSS modules");
    put(root/L"game5/MicrosoftGame.config","<Game/>");need(st(m,plain).preflight["store"]=="gdk","store marker detected");
    // An installed OS package identity means Xbox app / GDK, whatever the
    // directory looks like (an Unreal GDK title keeps its marker far above the EXE).
    need(lab::games::store_kind(root/L"game4",true)=="gdk"&&lab::games::store_kind(root/L"game4",false)=="unknown"&&lab::games::store_kind(root/L"game5",false)=="gdk","package identity decides the store");
    // ---- discovery is bounded and explicit
    auto disc=lab::games::discover(root/L"game5");need(disc.complete&&disc.executables.size()==1,"directory discovery");need(lab::games::discover(root/L"game5/Plain.exe").executables.size()==1,"direct EXE discovery");
    reject([&]{lab::games::discover(root/L"game2/../game");},"noncanonical input rejected");reject([&]{lab::games::discover(root.root_path());},"whole drive rejected");
    m.forget(plain.id);need(m.list().size()==4&&fs::exists(root/L"game5/Plain.exe"),"forget registry only");
    // ---- hardlinks can alias a protected file even without a reparse point
    fs::remove(root/L"game/TestGame.exe");need(CreateHardLinkW((root/L"game/TestGame.exe").c_str(),(root/L"game5/Plain.exe").c_str(),nullptr),"hardlink fixture");need(!st(m,e).can_install,"hardlink identity rejected");fs::remove(root/L"game/TestGame.exe");put(root/L"game/TestGame.exe",game_bytes);
    // ---- malformed packages are skipped and named, never repaired
    fs::create_directory(root/L"app/adapters/broken");put(root/L"app/adapters/broken/package.json","x");
    {lab::games::Manager again(root,std::nullopt,model_hash,false);need(again.policies().size()==1&&again.package_notes().size()==1&&again.package_notes()[0].starts_with("broken"),"broken package noted, valid one kept");
     again.import_known_installations();need(again.list().size()==4,"import registers nothing without a Lab install in the game root");}
    m.forget(e.id);m.forget(d.id);m.forget(c.id);m.forget(sr.id);need(m.list().empty(),"remove uninstalled registry entries");
    // ================================================================ a controller root-proxy game end to end
    // plan -> install -> update (package behind) -> health negative -> update
    // failure back to not installed -> install -> game update -> repin -> uninstall.
    put(root/L"app/plugin/overglaze_controller.dll","synthetic CONTROLLER loader");
    for(const auto* p:{L"game7",L"game8",L"game9",L"game9/data",L"game10"})fs::create_directory(root/p);
    put(root/L"game7/Seven.exe",pe64(standard,"seven v1"));put(root/L"game7/sl.interposer.dll","sl seven");put(root/L"game7/sl.common.dll","common seven");put(root/L"game7/sl.dlss_d.dll","dlss_d seven");
    auto g=m.add(root/L"game7/Seven.exe");
    {auto s=st(m,g);need(s.state=="needs-package"&&s.compatibility=="supported"&&s.reasons.front().code=="package-missing"&&has_reason(s,"modules-unsigned"),"a new game: package missing, named");
     const auto plan=m.plan_install(g.id);need(plan["schema"]=="overglaze-install-plan-v1"&&plan["package"].is_null()&&plan["can_install"]==false&&plan["files"].empty(),"no package, no plan");}
    {auto o=lab::games::PackageOptions::controller_root_proxy();o.allow_unsigned_modules=true;o.name="seven";const auto p=m.make_package(g.id,o);
     need(p.track=="controller"&&p.loader.strategy=="root_proxy_d3d12","a controller root-proxy package");
     const auto cfg=read(root/L"app/adapters/seven/overglaze.install.json");need(cfg["exception_diagnostics"]==false&&cfg.size()==13,"the controller package says exception_diagnostics=false too; 13 keys with the loader block");}
    const auto seven=root/L"app/adapters/seven";
    // ---- plan_install: a dry run that writes nothing
    {const auto box=store/lab::wide(g.id);need(!fs::exists(box),"no manager box before the plan");
     const auto plan=m.plan_install(g.id);
     need(plan["can_install"]==true&&plan["package"]=="seven"&&plan["load_mode"]=="root_proxy_d3d12"&&plan["conflicts"]==false&&plan["stages"].size()==lab::games::operation_stages("install").size(),"plan of an installable game");
     need(plan["files"].size()==5,"five files: proxy, host, bridge, model, config");
     std::uint64_t bytes=0,staged=0;
     for(const auto& f:plan["files"]){need(f["action"]=="copy"&&f["bytes"].is_number_unsigned()&&f["sha256"].is_string(),"every file copied, sized, hashed");bytes+=f["bytes"].get<std::uint64_t>();
        if(f["name"]!="nvngx_dlssnr.dll")staged+=f["bytes"].get<std::uint64_t>(); // the model is not staged
        const fs::path dest=lab::wide(f["destination"].get<std::string>());
        need(f["name"]=="dxgi.dll"?dest==(root/L"game7/dxgi.dll").lexically_normal():dest.parent_path()==(root/L"game7/overglaze").lexically_normal(),"the proxy beside the EXE, the rest in the overglaze subdirectory");
        need(fs::file_size(fs::path(lab::wide(f["source"].get<std::string>())))==f["bytes"].get<std::uint64_t>(),"the size is the source's");}
     need(plan["space"]["game_disk"]["writes"]==bytes&&plan["space"]["data_disk"]["writes"]==staged&&staged<bytes&&plan["space"]["game_disk"]["ok"].is_boolean()&&plan["space"]["data_disk"]["required_free"]==lab::games::kDataDiskReserveBytes,"space checks with what would be written");
     need(plan["directories_created"].size()==1&&plan["launch"]["code"]=="launch-root-proxy","plan names the new subdirectory and how to start the game");
     need(!fs::exists(box)&&!fs::exists(root/L"game7/overglaze")&&!fs::exists(root/L"game7/dxgi.dll"),"the plan wrote nothing");}
    // ---- install, with progress
    std::vector<lab::games::ProgressEvent> ev;const lab::games::Progress cb=[&](const lab::games::ProgressEvent& x){ev.push_back(x);};
    m.install(g.id,true,true,"fixture consent: controller game",cb);
    {auto expected=lab::games::operation_stages("install");expected.erase(std::find(expected.begin(),expected.end(),"verify"));
     need(started(ev,"install","")==expected,"install reports its stages in order");
     unsigned done=0,skipped=0;for(const auto& x:ev){need(x.status!="failed","no failure");if(x.status=="done")++done;if(x.status=="skipped"){++skipped;need(x.stage=="verify","only the checker is skipped");}
        need(x.count==lab::games::operation_stages("install").size()&&x.index>=1&&x.index<=x.count,"index of count");}
     need(done==expected.size()&&skipped==1,"every started stage finishes");}
    {auto s=st(m,g);need(s.state=="installed"&&s.install_state=="installed"&&s.health=="ok"&&s.load_mode=="root_proxy_d3d12"&&fs::exists(root/L"game7/dxgi.dll")&&fs::exists(root/L"game7/overglaze/overglaze_controller.dll"),"installed, healthy");
     need(check_named(s.checks,"health.host")->outcome==lab::games::Outcome::pass&&check_named(s.checks,"update.proxy")->outcome==lab::games::Outcome::pass,"health checks recorded");
     bool launch=false;for(const auto& r:s.reasons)if(r.code=="launch-root-proxy"&&r.params["subdir"]=="overglaze")launch=true;need(launch,"how to start a root-proxy game");}
    reject([&]{m.refresh_package("seven");},"no refresh under an installed controller game");
    // ---- package behind the published host again, through the proxy: a newer published proxy is an update
    put(root/L"app/plugin/dxgi.dll","synthetic CONTROLLER proxy v2");
    m=lab::games::Manager(root,std::nullopt,model_hash,false);
    {auto s=st(m,g);need(s.install_state=="update-available"&&s.update.package_behind_published&&s.health=="ok"&&s.reasons.front().params["parts"].size()==1&&s.reasons.front().params["parts"][0]=="proxy","a newer proxy is an update, named as such");}
    ev.clear();m.update(g.id,true,true,"fixture consent: controller game",cb);
    {auto s=st(m,g);need(s.install_state=="installed"&&lab::sha256(root/L"game7/dxgi.dll")==lab::sha256(root/L"app/plugin/dxgi.dll"),"the update carried the new proxy");
     need(started(ev,"update","")==lab::games::operation_stages("update"),"update stages");}
    // ---- negative: the package was refreshed behind the game's back (an
    // older tool, a hand edit). Health check: 需要更新才能使用.
    {put(seven/L"overglaze_controller.dll","synthetic CONTROLLER loader v3");put(root/L"app/plugin/overglaze_controller.dll","synthetic CONTROLLER loader v3");
     auto man=read(seven/L"package.json");man["payload"]["overglaze_controller.dll"]=lab::sha256(seven/L"overglaze_controller.dll");put(seven/L"package.json",man.dump(2));
     m=lab::games::Manager(root,std::nullopt,model_hash,false);
     auto s=st(m,g);
     need(s.state=="installed"&&s.install_state=="needs-update"&&s.health=="needs-update"&&s.update.host&&!s.update.package_behind_published&&s.update_available&&s.can_update,"the health check catches a package refreshed without the game");
     need(s.reasons.front().code=="needs-update"&&lab::games::render(s.reasons.front()).find("宿主")!=std::string::npos&&check_named(s.checks,"health.host")->outcome==lab::games::Outcome::fail,"named, with the part that differs");
     const auto view=lab::games::present(s);need(view.label=="label.needs-update"&&lab::games::code_text(view.label)==std::string("需要更新才能使用")&&view.tone==lab::games::Tone::warning&&view.actions.front().name=="update"&&view.actions.front().enabled,"需要更新才能使用, with update as the one primary action");
     m.update(g.id,true,true,"fixture consent: controller game");
     need(st(m,g).install_state=="installed"&&st(m,g).health=="ok","update repairs it");}
    // ---- an update that fails after its uninstall: back to not installed, said so
    put(root/L"app/plugin/overglaze_controller.dll","synthetic CONTROLLER loader v4");
    {lab::games::Manager checked(root,std::nullopt,model_hash,true); // the package's checker refuses
     need(st(checked,g).install_state=="update-available","an update is available");
     ev.clear();bool reported=false;
     try{checked.update(g.id,true,true,"fixture consent: controller game",cb);}
     catch(const lab::games::OperationError& x){reported=true;
        need(x.operation=="update"&&x.stage=="install"&&x.install_after=="not-installed"&&x.reason.code=="update-failed-uninstalled","the failure says where it stopped and that the game is not installed");
        need(fs::is_directory(fs::path(lab::wide(x.reason.params["recovery"].get<std::string>())))&&std::string(x.what()).find("安装检查器")!=std::string::npos,"with the recovery copy and the checker's refusal");
        const auto j=lab::games::operation_error_json(x);need(j["install_after"]=="not-installed"&&j["text"].get<std::string>().find("未安装")!=std::string::npos,"the CLI error carries it too");}
     need(reported,"an OperationError, not a bare exception");
     bool nested=false;for(const auto& x:ev)if(x.operation=="install"&&x.parent=="update"&&x.stage=="verify"&&x.status=="failed")nested=true;need(nested,"progress names the failing nested stage");
     // The refused root-proxy install took its proxy and its subdirectory with it
     // (a proxy left behind would block any reinstall).
     need(!fs::exists(root/L"game7/dxgi.dll")&&!fs::exists(root/L"game7/overglaze"),"nothing of ours is left in the game");
     auto s=st(checked,g);need(s.state=="available"&&s.install_state=="not-installed"&&s.can_install,"really not installed, and installable again");}
    m=lab::games::Manager(root,std::nullopt,model_hash,false);m.install(g.id,true,true,"fixture consent: controller game");need(st(m,g).install_state=="installed","reinstalled");
    // ---- the game updates: repin
    const auto old_exe=lab::sha256(root/L"game7/Seven.exe");put(root/L"game7/Seven.exe",pe64(standard,"seven v2"));
    {auto s=st(m,g);need(s.state=="changed"&&s.install_state=="game-changed"&&s.installed&&s.health=="game-changed"&&s.can_uninstall,"a changed controller game");
     need(!s.can_repin&&s.refusals.at("repin").code=="repin-unsigned-modules","synthetic modules are unsigned: repin needs it said");
     const auto view=lab::games::present(s);const auto it=std::find_if(view.actions.begin(),view.actions.end(),[](const lab::games::Action& a){return a.name=="repin";});
     need(it!=view.actions.end()&&!it->enabled&&it->why_not.code=="repin-unsigned-modules","shown, disabled, with the reason on hover");
     need(s.preflight.is_object()&&check_named(s.checks,"module-signatures")->outcome==lab::games::Outcome::fail,"the fresh preflight is shown");}
    reject([&]{m.repin(g.id,false,true,"c",true);},"repin needs the confirmations");
    // A package whose profile is a compiled review row is refused (the host would
    // demand that row's EXE hash): same package, the profile id of a reviewed row.
    {auto policies=m.policies();for(auto& p:policies)if(p.name=="seven")p.profile="re9-rr-v1";
     lab::games::Manager reviewed(root,policies,model_hash,false);auto s=st(reviewed,g);
     need(s.refusals.at("repin").code=="repin-compiled-row"&&!s.can_repin,"a compiled review row is never repinned automatically");
     const auto view=lab::games::present(s);need(std::none_of(view.actions.begin(),view.actions.end(),[](const lab::games::Action& a){return a.name=="repin";}),"and not offered");
     bool refused=false;try{reviewed.repin(g.id,true,true,"c",true);}catch(const lab::games::OperationError& x){refused=x.reason.code=="operation-failed"&&x.install_after=="game-changed";}need(refused,"repin refuses it before touching anything");
     need(fs::exists(root/L"game7/dxgi.dll")&&fs::is_directory(seven),"nothing touched");}
    ev.clear();const auto retired=m.repin(g.id,true,true,"fixture consent: re-adapt",true,cb);
    need(started(ev,"repin","")==lab::games::operation_stages("repin"),"repin reports every stage in order");
    need(lab::winpath::same_spelling(retired.parent_path(),(root/L"app/adapters-retired").lexically_normal())&&retired.filename().wstring().starts_with(L"seven-repin-")&&read(retired/L"package.json")["pins"]["Seven.exe"]==old_exe,"the old package is retired, not deleted");
    need(fs::is_regular_file(retired/L"overglaze_controller.dll")&&fs::is_regular_file(retired/L"overglaze.install.json"),"retired with its payload");
    {const auto man=read(seven/L"package.json");need(man["pins"]["Seven.exe"]==lab::sha256(root/L"game7/Seven.exe")&&man["track"]=="controller"&&man["loader"]["strategy"]=="root_proxy_d3d12","the new package pins the new EXE, same track and loader");
     bool note=false;for(const auto& n:man["notes"])if(n.get<std::string>().rfind("repinned ",0)==0)note=true;need(note,"the repin is recorded in the package notes");
     need(read(seven/L"overglaze.install.json")["exception_diagnostics"]==false,"a repinned package carries exception_diagnostics=false too");}
    {auto s=st(m,g);need(s.state=="installed"&&s.install_state=="installed"&&s.health=="ok"&&fs::exists(root/L"game7/dxgi.dll"),"repinned and installed");}
    reject([&]{m.repin(g.id,true,true,"c",true);},"nothing to repin when nothing changed");
    // ---- uninstall with progress, then forget
    ev.clear();m.uninstall(g.id,true,cb);need(started(ev,"uninstall","")==lab::games::operation_stages("uninstall"),"uninstall reports every stage in order");
    need(!fs::exists(root/L"game7/dxgi.dll")&&!fs::exists(root/L"game7/overglaze"),"uninstalled");m.forget(g.id);
    // ---- a package made under the Denuvo override: refreshing it needs the flag
    // said again, so the page shows the refresh disabled with that reason
    put(root/L"game10/DenuvoSl.exe",pe64({".text",".xtext",".rdata"},"denuvo-sl"));put(root/L"game10/sl.interposer.dll","sl ten");put(root/L"game10/sl.common.dll","common ten");put(root/L"game10/sl.dlss_d.dll","dlss_d ten");
    {auto dz=m.add(root/L"game10/DenuvoSl.exe");
     m.allow_denuvo_passive_coexistence(true);{auto o=lab::games::PackageOptions::controller_root_proxy();o.allow_unsigned_modules=true;o.name="denuvo-sl";need(m.make_package(dz.id,o).denuvo_override,"the override is recorded on the package");}
     m.allow_denuvo_passive_coexistence(false);
     put(root/L"app/plugin/overglaze_controller.dll","synthetic CONTROLLER loader v5");
     m=lab::games::Manager(root,std::nullopt,model_hash,false);
     {auto s=st(m,dz);need(s.state=="package-stale"&&!s.can_refresh_package&&s.refusals.at("refresh-package").code=="refresh-needs-denuvo-flag","a Denuvo package's refresh is gated, and says why");
      const auto view=lab::games::present(s);need(view.actions.front().name=="refresh-package"&&!view.actions.front().enabled&&view.actions.front().why_not.code=="refresh-needs-denuvo-flag","shown disabled with the reason");}
     reject([&]{m.refresh_package("denuvo-sl");},"refresh refused without the flag");
     m.allow_denuvo_passive_coexistence(true);
     {auto s=st(m,dz);need(s.can_refresh_package&&!s.refusals.contains("refresh-package"),"with the flag said, refresh is offered");}
     m.refresh_package("denuvo-sl");need(st(m,dz).state=="available","and works");
     m.allow_denuvo_passive_coexistence(false);m.forget(dz.id);}
    // ================================================================ rename / host contract V4
    // A game installed by the pre-rename build: legacy package, legacy transaction,
    // legacy files in game11\dlsslab\. It is recognised, shown as "legacy", and
    // migrated in one operation: uninstall by recorded hashes, the package
    // rewritten under the new names, install.
    fs::create_directory(root/L"game11");
    put(root/L"game11/Eleven.exe",pe64(standard,"eleven"));put(root/L"game11/sl.interposer.dll","sl eleven");put(root/L"game11/sl.common.dll","common eleven");put(root/L"game11/sl.dlss_d.dll","dlss_d eleven");
    {auto g11=m.add(root/L"game11/Eleven.exe");
     {auto o=lab::games::PackageOptions::controller_root_proxy();o.allow_unsigned_modules=true;o.name="eleven";m.make_package(g11.id,o);}
     const auto pkg11=root/L"app/adapters/eleven";
     // Turn it into what the pre-rename manager wrote: same bytes, old names and schema.
     const std::pair<const wchar_t*,const wchar_t*> renames[]{{L"overglaze_controller.dll",L"dlsslab_controller.dll"},{L"overglaze_nvngx.dll",L"dlsslab_nvngx.dll"},
        {L"overglaze.install.json",L"dlsslab.install.json"},{L"overglaze_install_check.exe",L"dlsslab_install_check.exe"}};
     for(const auto& [now,old]:renames)fs::rename(pkg11/now,pkg11/old);
     {auto man=read(pkg11/L"package.json");man["schema"]="dlsslab-adapter-package-v1";
      man["payload"]["dlsslab_controller.dll"]=man["payload"]["overglaze_controller.dll"];man["payload"].erase("overglaze_controller.dll");
      man["payload"]["dlsslab_nvngx.dll"]=man["payload"]["overglaze_nvngx.dll"];man["payload"].erase("overglaze_nvngx.dll");
      man["loader"]["basename"]="dlsslab_controller.dll";man["loader"]["subdir"]="dlsslab";put(pkg11/L"package.json",man.dump(2));}
     // ... and what it installed: the proxy beside the EXE, the rest in dlsslab\.
     fs::create_directory(root/L"game11/dlsslab");json files=json::object();
     for(const auto* n:{L"dlsslab_controller.dll",L"dlsslab_nvngx.dll",L"dlsslab.install.json"}){fs::copy_file(pkg11/n,root/L"game11/dlsslab"/n);files[lab::utf8(n)]=lab::sha256(root/L"game11/dlsslab"/n);}
     fs::copy_file(root/L"app/models/nvngx_dlssnr.dll",root/L"game11/dlsslab/nvngx_dlssnr.dll");files["nvngx_dlssnr.dll"]=model_hash;
     fs::copy_file(pkg11/L"dxgi.dll",root/L"game11/dxgi.dll");files["dxgi.dll"]=lab::sha256(root/L"game11/dxgi.dll");
     fs::create_directory(store/lab::wide(g11.id));
     put(store/lab::wide(g11.id)/L"transaction.json",json{{"schema","dlsslab-install-transaction-v1"},{"id",g11.id},{"exe",lab::utf8((root/L"game11/Eleven.exe").wstring())},{"state","installed"},
        {"files",files},{"recovery",nullptr},{"game_started",false},{"package","eleven"},{"consent","pre-rename fixture"},
        {"loader_subdirectory","dlsslab"},{"loader_basename","dlsslab_controller.dll"},{"strategy","root_proxy_d3d12"}}.dump(2));
     m=lab::games::Manager(root,std::nullopt,model_hash,false);
     {const lab::games::Policy* legacy=nullptr;for(const auto& q:m.policies())if(q.name=="eleven")legacy=&q;
      need(legacy&&legacy->legacy&&legacy->loader.basename=="overglaze_controller.dll"&&legacy->loader.subdir==L"overglaze"&&legacy->legacy_basename=="dlsslab_controller.dll"&&legacy->legacy_subdir==L"dlsslab",
           "a pre-rename package is loaded, described under the new names, and remembers its old ones");}
     {auto s=st(m,g11);need(s.state=="installed"&&s.install_state=="legacy"&&s.installed&&s.update_available&&s.can_update&&s.can_uninstall&&has_reason(s,"legacy-install"),"a pre-rename install is recognised as legacy, migratable");
      const auto view=lab::games::present(s);need(view.label=="label.legacy"&&view.actions.front().name=="update"&&view.actions.front().label=="action.migrate"&&view.actions.front().enabled,"the page offers the migration");}
     reject([&]{m.install(g11.id,true,true,"c");},"a legacy install is never installed over");
     ev.clear();m.migrate(g11.id,true,true,"fixture consent: migrate",cb);
     need(started(ev,"update","")==lab::games::operation_stages("update"),"the migration is the update operation, every stage");
     {auto s=st(m,g11);need(s.state=="installed"&&s.install_state=="installed"&&s.health=="ok","migrated, installed, healthy");}
     need(!fs::exists(root/L"game11/dlsslab")&&fs::is_regular_file(root/L"game11/overglaze/overglaze_controller.dll")&&fs::is_regular_file(root/L"game11/overglaze/overglaze_nvngx.dll")&&
          fs::is_regular_file(root/L"game11/overglaze/overglaze.install.json")&&fs::is_regular_file(root/L"game11/overglaze/nvngx_dlssnr.dll")&&fs::is_regular_file(root/L"game11/dxgi.dll"),"the old files are gone; the new names are installed");
     {const auto man=read(pkg11/L"package.json");need(man["schema"]=="overglaze-adapter-package-v1"&&man["payload"].contains("overglaze_controller.dll")&&!man["payload"].contains("dlsslab_nvngx.dll")&&man["loader"]["subdir"]=="overglaze","the package is rewritten under the new names");
      for(const auto* n:{L"dlsslab_controller.dll",L"dlsslab_nvngx.dll",L"dlsslab.install.json",L"dlsslab_install_check.exe"})need(!fs::exists(pkg11/n),"the package's old files are gone");}
     {bool legacy_copy_kept=false;for(const auto& d:fs::directory_iterator(store/lab::wide(g11.id)))if(d.is_directory()&&d.path().filename().wstring().starts_with(L"uninstall-")&&fs::is_regular_file(d.path()/L"dlsslab_controller.dll"))legacy_copy_kept=true;
      need(legacy_copy_kept,"the recovery copy keeps the legacy files under their names");}
     reject([&]{m.migrate(g11.id,true,true,"c");},"nothing to migrate twice");
     m.uninstall(g11.id,true);need(!fs::exists(root/L"game11/overglaze")&&!fs::exists(root/L"game11/dxgi.dll"),"uninstalled after the migration");m.forget(g11.id);}
    // The user-supplied model against the reviewed-version table: the
    // synthetic model is not a reviewed version; a missing one is said as such.
    {lab::games::Manager table(root,std::nullopt,{},false);
     const auto ms=table.model_status();need(ms.present&&!ms.known&&ms.sha256==model_hash,"the synthetic model is not a reviewed version");
     const auto mj=lab::games::model_json(ms);
     need(mj["reason"]["code"]=="model-unknown-version"&&mj["known_versions"].size()==1&&mj["known_versions"][0]["sha256"]=="e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e","the status names the unknown version and the table");
     fs::create_directory(root/L"game12");put(root/L"game12/Twelve.exe",pe64(standard,"twelve"));put(root/L"game12/sl.interposer.dll","sl twelve");put(root/L"game12/sl.dlss_d.dll","dlss_d twelve");
     auto g12=table.add(root/L"game12/Twelve.exe");
     {auto s=st(table,g12);need(s.state=="needs-package"&&!s.can_make_package&&has_reason(s,"model-unknown-version")&&s.refusals.at("make-package").code=="model-unknown-version","an unknown model stops packaging, named");}
     {lab::games::PackageOptions o;o.allow_unsigned_modules=true;reject([&]{table.make_package(g12.id,o);},"no package is made around an unknown model");}
     fs::rename(root/L"app/models/nvngx_dlssnr.dll",root/L"app/models/nvngx_dlssnr.dll.away");
     {const auto gone=table.model_status();need(!gone.present&&lab::games::model_json(gone)["reason"]["code"]=="model-missing","a missing model is said as missing");
      auto s=st(table,g12);need(has_reason(s,"model-missing")&&!s.can_make_package,"and stops packaging");
      const auto text=lab::games::render(s.refusals.at("make-package"));need(text.find("不包含、也不分发")!=std::string::npos&&text.find("\xe6\xb3\x84")==std::string::npos,"the words say we do not include it, and nothing else");}
     fs::rename(root/L"app/models/nvngx_dlssnr.dll.away",root/L"app/models/nvngx_dlssnr.dll");
     table.forget(g12.id);}
    // ================================================================ a check that cannot run is "unknown"
    put(root/L"game8/Locked.exe",pe64(standard,"locked"));put(root/L"game8/sl.interposer.dll","sl");put(root/L"game8/sl.dlss_d.dll","dlss_d");
    {lab::Handle hold(CreateFileW((root/L"game8/Locked.exe").c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr));need(hold.valid(),"exclusive fixture handle");
     const auto pre=lab::games::preflight(root/L"game8/Locked.exe");
     need(!pre.executable_readable&&pre.verdict=="unsupported-store","an unreadable EXE without a package identity is unsupported, as before");
     for(const auto* n:{"denuvo","pe","identity"})need(check_named(pre.checks,n)&&check_named(pre.checks,n)->outcome==lab::games::Outcome::unknown,std::string("unreadable EXE leaves ")+n+" unknown, never a pass");
     need(check_named(pre.checks,"denuvo")->reason=="exe-unreadable"&&pre.to_json()["checks"][2]["ok"].is_null(),"cannot-check is ok:null with its reason");}
    for(unsigned i=0;i<4200;++i)put(root/L"game9/data"/lab::wide("f"+std::to_string(i)+".bin"),"x");
    put(root/L"game9/Big.exe",pe64(standard,"big"));put(root/L"game9/sl.interposer.dll","sl");put(root/L"game9/sl.dlss_d.dll","dlss_d");
    {const auto pre=lab::games::preflight(root/L"game9/Big.exe");const auto* a=check_named(pre.checks,"anticheat");
     need(a&&a->outcome==lab::games::Outcome::unknown&&a->reason=="scan-truncated"&&a->value["limit"]==4096&&!pre.scan_complete,"a truncated marker scan is unknown, never a pass");
     need(pre.verdict=="sl-rr-ready","the verdict ladder itself is unchanged by it");
     auto big=m.add(root/L"game9/Big.exe");auto s=st(m,big);
     need(s.state=="needs-package"&&has_reason(s,"preflight-incomplete")&&s.reasons.back().params["check"][0]=="anticheat","the status says a check could not be completed");
     m.forget(big.id);}
    // ---- every listed backend state the synthetic flow can reach was reached
    for(const auto* pair:{"needs-package/not-installed","package-stale/not-installed","available/not-installed","installed/installed","installed/update-available","installed/needs-update","installed/legacy",
        "existing/research-managed","changed/game-changed","incomplete/incomplete","blocked/modified","blocked/not-installed","blocked/unknown","denuvo-blocked/not-installed",
        "anticheat-blocked/not-installed","no-dlss/not-installed"})need(reached.contains(pair),std::string("the flow reached ")+pair);
    put(store/L"games.json","broken");reject([&]{m.add(root/L"game5/Plain.exe");},"malformed registry not overwritten");need(fs::file_size(store/L"games.json")==6,"malformed registry preserved");
    // ---- storage statistic (read-only) and the disk rule for uninstall
    {const auto u=m.storage_usage();const lab::games::StorageUsage::Game* g=nullptr;for(const auto& x:u.games)if(x.id==e.id)g=&x;
     need(g&&g->complete&&g->recovery_copies==3&&g->managed_recovery_copies==2&&g->staging_directories==1&&g->staging_bytes>0&&g->recovery_bytes>g->managed_recovery_bytes&&g->other_bytes>0,
          "storage counts two managed copies, the pre-rule copy and the failed install's staging");
     const auto j=lab::games::storage_json(u);need(j["schema"]=="overglaze-manager-storage-v1"&&j["recovery_copies_kept_per_game"]==2&&j["data_disk_reserve_bytes"]==lab::games::kDataDiskReserveBytes&&u.total_bytes>=g->recovery_bytes+g->staging_bytes,"storage report shape");
     need(fs::is_regular_file(legacy_copy/L"dxgi.dll")&&made(L"package-")==failed_staging,"the statistic deletes nothing");}
    need(lab::games::recovery_space_needed(200ULL<<20)==(200ULL<<20)+lab::games::kRecoveryCopyMarginBytes&&lab::games::recovery_space_needed(200ULL<<20)<lab::games::kDataDiskReserveBytes,
         "uninstall needs the recovery copy plus a margin, not the 30 GiB install reserve");
    // Only this fixture's exact unique directory is removed after success.
    need(root.parent_path()==base&&root.filename().wstring().starts_with(L"overglaze-manager-test-"),"cleanup confinement");for(auto& f:fs::recursive_directory_iterator(root))lab::winpath::require_no_reparse(f.path());const auto removed=fs::remove_all(root);
    std::cout<<json{{"passed",true},{"checks",checks},{"states_reached",reached},{"game_started",false},{"nr_executed",false},{"synthetic_files_removed",removed},{"research_files_removed",0}}.dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\nfixture="<<lab::utf8(root.wstring())<<'\n';return 1;}}
