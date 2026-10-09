// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_installation.hpp"
#include "lab_module_search.hpp"
#include "lab_package_identity.hpp"
#include "lab_game_profile.hpp"
#include "lab_windows_path.hpp"
#include "lab_local_path.hpp"
#include "lab_identity.hpp"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <array>
#include <optional>
namespace lab {
// One bounded search, shared with the manager (lab_module_search.hpp).
namespace {
void demand(bool b,const char* why){if(!b)throw std::runtime_error(why);}
bool hash(const std::string& s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::filesystem::path absolute(const std::string& s){auto p=std::filesystem::path(wide(s));
    demand(p.is_absolute()&&p==p.lexically_normal()&&s.find('\0')==std::string::npos,"Installation path must be absolute and normalized");return p;}
void no_reparse(const std::filesystem::path& p){winpath::require_no_reparse(p);}
bool plain_name(const std::string& s,std::size_t max){if(s.empty()||s.size()>max)return false;
    for(char c:s)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.'||c=='_'))return false;return (s[0]>='a'&&s[0]<='z')||(s[0]>='0'&&s[0]<='9');}
bool ascii_exe(const std::string& s){if(s.size()<5||s.size()>128)return false;for(char c:s)if(c<0x21||c>0x7e||c=='\\'||c=='/'||c==':')return false;
    auto lower=[](char c){return c>='A'&&c<='Z'?char(c+32):c;};const auto tail=s.substr(s.size()-4);return lower(tail[0])=='.'&&lower(tail[1])=='e'&&lower(tail[2])=='x'&&lower(tail[3])=='e';}
bool same_ascii(std::string_view a,std::string_view b){if(a.size()!=b.size())return false;auto lower=[](char c){return c>='A'&&c<='Z'?char(c+32):c;};
    for(std::size_t i=0;i<a.size();++i)if(lower(a[i])!=lower(b[i]))return false;return true;}
// V3 facts: shape-checked here, trusted only through the package hash chain in
// load_installation_file. A compiled row, when present, must agree exactly.
profiles::Facts parse_facts(const json& f,const std::string& id,const std::string& package,const std::string& game_sha256,const profiles::Game* row){
    demand(f.is_object()&&f.size()==11,"V3 facts must carry exactly the eleven game facts");
    for(const auto* k:{"executable","title","route","settings_file","capture_origin"})demand(f.contains(k)&&f.at(k).is_string(),"V3 fact string missing");
    for(const auto* k:{"linear_depth","native_evaluate_host_rebind","binding_preservation"})demand(f.contains(k)&&f.at(k).is_boolean(),"V3 fact flag missing");
    demand(f.contains("viewport")&&f.at("viewport").is_number_unsigned()&&f.at("viewport").get<unsigned>()<=15,"V3 viewport must be 0..15");
    demand(f.contains("default_exposure_stops")&&f.at("default_exposure_stops").is_number()&&std::fabs(f.at("default_exposure_stops").get<double>())<=10.0,"V3 exposure default outside -10..10 stops");
    demand(f.contains("modules")&&f.at("modules").is_object()&&!f.at("modules").empty()&&f.at("modules").size()<=8,"V3 modules must pin 1..8 game modules");
    demand(plain_name(package,64)&&plain_name(id,64),"V3 package/profile ids must be plain lowercase identifiers");
    demand(id!="cyberpunk-rr-experimental-v1"&&id!="synthetic-standalone-fixture","Reserved profile id");
    profiles::Facts x;x.id=id;x.package=package;x.executable=f.at("executable");x.title=f.at("title");x.route=f.at("route");
    x.settings_file=f.at("settings_file");x.capture_origin=f.at("capture_origin");x.viewport=f.at("viewport");
    x.linear_depth=f.at("linear_depth");x.native_evaluate_host_rebind=f.at("native_evaluate_host_rebind");x.binding_preservation=f.at("binding_preservation");
    x.default_exposure_stops=f.at("default_exposure_stops").get<float>();x.executable_sha256=game_sha256;
    // The four routes the installation format accepts. On ngx-rr the host
    // admits through the NGX observer's evaluate detour; on ngx-sr
    // it only observes -- no provider, no receiver, nothing can be admitted.
    // Accepting a route here is a statement about the format, never about
    // admission: that is decided by the host from the route.
    const bool streamline=x.route=="sl-rr"||x.route=="sl-sr";
    const bool ngx_direct=x.route=="ngx-rr"||x.route=="ngx-sr";
    demand(streamline||ngx_direct,"V3 route must be sl-rr, sl-sr, ngx-rr or ngx-sr");
    const std::string stage=(x.route=="sl-rr"||x.route=="ngx-rr")?"rr":"sr";
    demand(ascii_exe(x.executable)&&x.title.size()<=128,"V3 executable/title malformed");
    demand(x.settings_file=="overlay-"+package+".json","V3 settings file must be overlay-<package>.json");
    demand(x.capture_origin==package+"-controlled-"+stage+"-stage","V3 capture origin must derive from package and route");
    for(auto it=f.at("modules").begin();it!=f.at("modules").end();++it){
        demand(it.key().size()>4&&it.key().size()<=64&&same_ascii(it.key().substr(it.key().size()-4),".dll")&&it.value().is_string()&&hash(it.value().get<std::string>()),"V3 module pin malformed");
        x.modules[it.key()]=it.value().get<std::string>();}
    // Pin the module the route actually drives. Requiring sl.interposer.dll
    // everywhere only worked for Halo because it happens to ship Streamline
    // for frame generation; a pure-NGX title has no interposer at all.
    if(streamline)demand(x.modules.contains("sl.interposer.dll"),"V3 Streamline route must pin sl.interposer.dll");
    else demand(x.modules.contains(x.route=="ngx-rr"?"nvngx_dlssd.dll":"nvngx_dlss.dll"),
                "V3 NGX-direct route must pin the NGX model it drives");
    if(row){const auto r=profiles::Facts::from(*row);
        demand(same_ascii(x.executable,r.executable)&&x.executable_sha256==r.executable_sha256&&x.viewport==r.viewport&&x.linear_depth==r.linear_depth&&
            x.native_evaluate_host_rebind==r.native_evaluate_host_rebind&&x.binding_preservation==r.binding_preservation&&
            x.default_exposure_stops==r.default_exposure_stops&&x.settings_file==r.settings_file&&x.capture_origin==r.capture_origin,"V3 facts diverge from the reviewed profile");
        x.reviewed=true;}
    return x;
}
}
std::filesystem::path module_path(HMODULE module){std::wstring path(32768,0);auto n=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
    demand(n&&n<path.size(),"Module path unavailable");path.resize(n);return path;}
bool has_installation(HMODULE module){return std::filesystem::is_regular_file(module_path(module).parent_path()/identity::kInstallationFile);}
namespace {
// What the receipt beside the host says about where the product lives, read
// without validating anything else. Bounded and exception-free for callers.
struct RecordedRoot {std::filesystem::path data;bool package=false;};
std::optional<RecordedRoot> read_recorded_root(const std::filesystem::path& config) noexcept {
    try{
        std::error_code ec;if(!std::filesystem::is_regular_file(config,ec)||ec)return std::nullopt;
        const auto size=std::filesystem::file_size(config,ec);if(ec||size>16384)return std::nullopt;
        std::ifstream in(config,std::ios::binary);const auto j=json::parse(in,nullptr,false);
        if(!j.is_object()||!j.contains("output_root")||!j.at("output_root").is_string())return std::nullopt;
        const std::filesystem::path root(wide(j.at("output_root").get<std::string>()));
        if(!root.is_absolute()||root!=root.lexically_normal()||!root.has_relative_path())return std::nullopt;
        return RecordedRoot{root,j.contains("package")};
    }catch(...){return std::nullopt;}
}
std::filesystem::path config_beside(HMODULE module){return resolve_package_layout(module_path(module)).parent_path()/identity::kInstallationFile;}
}
std::optional<std::filesystem::path> recorded_output_root(const std::filesystem::path& config) noexcept {
    if(auto r=read_recorded_root(config))return r->data;
    return std::nullopt;
}
std::optional<std::filesystem::path> recorded_output_root(HMODULE module) noexcept {
    try{return recorded_output_root(config_beside(module));}catch(...){return std::nullopt;}
}
bool installation_root_missing(HMODULE module) noexcept {
    try{return installation_root_missing(config_beside(module));}catch(...){return false;}
}
bool installation_root_missing(const std::filesystem::path& config) noexcept {
    const auto r=read_recorded_root(config);if(!r)return false; // unreadable: the full contract reports it
    std::error_code ec;
    if(!std::filesystem::is_directory(r->data,ec))return true;
    // A package-bearing receipt is trusted through <root>\..\app\adapters: the
    // program folder itself. Its absence means the program was removed.
    if(r->package&&!std::filesystem::is_directory(r->data.parent_path()/L"app",ec))return true;
    return false;
}
std::filesystem::path fixture_data_root(){
    std::array<wchar_t,32768> value{};
    const auto n=GetEnvironmentVariableW(L"OVERGLAZE_LIVE_DATA_PATH",value.data(),static_cast<DWORD>(value.size()));
    demand(n&&n<value.size(),"Fixture live data path missing");
    const std::filesystem::path run(std::wstring(value.data(),n));
    demand(run.is_absolute()&&run==run.lexically_normal()&&run.has_relative_path()&&run.parent_path().has_relative_path(),
        "Fixture live data path must be an absolute run directory below a data root");
    const auto root=run.parent_path();
    localpath::require_local_fixed(root,true,{},"夹具数据目录不可用","fixture-root");
    return root;
}
// A subdirectory component: a plain name, no dots, no separators, no drive
// spelling. Three components at most. Anything else is refused rather than
// normalised, because a config that can walk out of the game directory is a
// config that can point the loader at a system DLL.
bool plain_component(const std::wstring& s){
    if(s.empty()||s.size()>64)return false;
    for(const auto c:s){
        const bool ok=(c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'_'||c==L'-';
        if(!ok)return false;
    }
    return true;
}
Loader parse_loader(const json& j){
    demand(j.is_object()&&j.size()==3,"Loader block must be strategy, basename and subdir");
    Loader l;
    l.strategy=j.at("strategy").get<std::string>();
    l.basename=j.at("basename").get<std::string>();
    const auto subdir=j.at("subdir").get<std::string>();
    demand(l.strategy=="root_dxgi_minimal"||l.strategy=="root_proxy_d3d12"||l.strategy=="root_proxy_on_insert"||l.strategy=="late_d3d12"||l.strategy=="reframework_plugin","Unsupported loader strategy");
    // The basename is checked against the strategy, not merely for shape: a
    // late loader named dxgi.dll would be picked up by the import redirection
    // it is supposed to avoid, and a root loader with any other name would
    // never be loaded at all.
    if(l.strategy=="root_dxgi_minimal")demand(l.basename=="dxgi.dll"&&subdir.empty(),"The root strategy is dxgi.dll in the game directory");
    // The root-proxy and late strategies name the same host in the same place.
    // They differ only in who loads it: the game, through the thin proxy this
    // strategy also installs, or an injector.
    else if(l.strategy=="root_proxy_d3d12"||l.strategy=="root_proxy_on_insert"||l.strategy=="late_d3d12")demand(l.basename=="overglaze_controller.dll"&&!subdir.empty(),"These strategies use overglaze_controller.dll in a subdirectory");
    else demand(l.basename=="overglaze_reframework.dll"&&!subdir.empty(),"The REFramework plugin uses overglaze_reframework.dll in its plugin directory");
    if(!subdir.empty()){
        const std::filesystem::path relative=wide(subdir);
        demand(relative.is_relative()&&!relative.has_root_name()&&relative==relative.lexically_normal(),"Loader subdirectory must be relative and normalized");
        unsigned components=0;
        for(const auto& part:relative){demand(plain_component(part.wstring()),"Loader subdirectory component is not a plain name");++components;}
        demand(components>=1&&components<=3,"Loader subdirectory must have one to three components");
        l.subdir=relative;
    }
    return l;
}
Installation parse_installation(const json& j,const json& exe,const json& host){
    demand(j.is_object(),"Unsupported installation contract");
    const unsigned version=j.value("version",0u);
    // V4 is V3 with the scope stated differently: instead of the user's
    // offline / no-anti-cheat declaration it carries the user's risk
    // acknowledgement and the facts the checks found, so a game with
    // anti-cheat is never recorded as having none. Everything else is V3.
    const bool acknowledged=version==4;
    const bool embedded=version==2,data_driven=version==3||acknowledged;
    // V3/V4 (controller-generated) always state exception_diagnostics and name
    // their adapter package plus the facts the host would otherwise compile in.
    const bool diagnostics=(embedded&&j.contains("exception_diagnostics"))||data_driven;
    // V3 is 12 keys, or 13 with the optional loader block; V4 is 11, or 12. A
    // config without the block is the original root dxgi.dll layout and stays
    // valid unchanged.
    // The extra key is the loader block and nothing else: a V3 config carrying
    // V4's risk block, or the reverse, is neither contract.
    const std::size_t loader_key=j.contains("loader")?1:0;
    // V4 may also pin an unrecognized model the user allowed (model_sha256).
    // Absent -- every install without that opt-in -- is the config as before.
    const std::size_t model_key=j.contains("model_sha256")?1:0;
    demand((!embedded&&!data_driven&&j.size()==11&&j.at("version")==1)||(embedded&&j.size()==(diagnostics?10:9))||
        (version==3&&j.size()==12+loader_key)||(acknowledged&&j.size()==11+loader_key+model_key),"Unsupported installation contract");
    demand(!j.contains("loader")||data_driven,"Only the V3/V4 contract carries a loader block");
    demand(!model_key||acknowledged,"Only the V4 contract pins a model");
    const auto id=j.at("profile").get<std::string>();const auto* profile=profiles::game(id);
    demand(profile||data_driven,"Unapproved game profile");
    if(acknowledged){
        // The acknowledgement is the user's, given at install (the manager
        // refuses an install without it); the facts are what the checks found.
        // Shape only: the host treats no protection differently for them.
        const auto& r=j.at("risk");
        demand(r.is_object()&&r.size()==3&&r.at("acknowledged")==true&&r.at("anti_tamper").is_boolean()&&r.at("anticheat").is_array()&&r.at("anticheat").size()<=16,"V4 risk acknowledgement malformed");
        for(const auto& name:r.at("anticheat"))demand(name.is_string()&&!name.get<std::string>().empty()&&name.get<std::string>().size()<=128,"V4 anti-cheat name malformed");
    }else demand(j.at("offline_single_player")==true&&j.at("no_anticheat")==true,"Unapproved game profile or offline scope");
    demand(embedded||data_driven||profile->id=="cyberpunk2077-rr-v1","Additional games require the embedded V2 contract");
    // A SHA-256, or -- for a title whose EXE cannot be read (Xbox app / GDK) --
    // the installed OS package's full name (lab_package_identity.hpp).
    demand(j.at("game_sha256").is_string()&&valid_identity_token(j.at("game_sha256").get<std::string>()),"Installation game identity malformed");
    Installation c;
    if(data_driven){demand(j.at("package").is_string()&&j.contains("facts"),"V3 requires package and facts");c.package=j.at("package").get<std::string>();
        c.facts=parse_facts(j.at("facts"),id,c.package,j.at("game_sha256").get<std::string>(),profile);}
    else c.facts=profiles::Facts::from(*profile);
    demand(j.at("game_sha256")==exe.at("sha256")&&j.at("host_sha256")==host.at("sha256"),"Installed executable/host identity changed");
    demand(exe.at("sha256").get<std::string>()==c.facts.executable_sha256,"Game build is not an accepted adapter profile");
    if(j.contains("loader"))c.loader=parse_loader(j.at("loader"));
    const auto game=absolute(exe.at("path").get<std::string>()),dll=absolute(host.at("path").get<std::string>());
    // The game directory comes from the running executable; the loader must then
    // be exactly where this installation says it put itself, under that directory.
    // Append only when there is something to append: path/"" yields a trailing
    // separator that lexically_normal keeps, and the spelling comparison below
    // is exact, so the root strategy would stop matching its own directory.
    const auto loader_dir=c.loader.subdir.empty()?game.parent_path():(game.parent_path()/c.loader.subdir).lexically_normal();
    demand(winpath::same_spelling(game.filename(),wide(c.facts.executable))&&
        winpath::same_spelling(dll.filename(),wide(c.loader.basename))&&winpath::same_spelling(loader_dir,dll.parent_path()),"Loader is not in the admitted game directory");
    c.profile=id;c.game_directory=game.parent_path();c.loader_directory=loader_dir;c.bridge_sha256=j.at("bridge_sha256");
    demand(hash(c.bridge_sha256),"Installation bridge hash malformed");
    if(model_key){demand(j.at("model_sha256").is_string()&&hash(j.at("model_sha256").get<std::string>()),"Installation model pin malformed");
        c.model_sha256=j.at("model_sha256").get<std::string>();}
    // Host contract V4: the data root is the one this receipt
    // records -- written by the manager from its own resolved root -- not a
    // compiled machine path. Only its spelling here; load_installation_file
    // holds the directory itself to the local-disk rules (lab_local_path.hpp).
    c.output_root=absolute(j.at("output_root").get<std::string>());
    demand(c.output_root.has_relative_path(),"Installation output root cannot be a whole disk");
    if(embedded||data_driven){
        demand(j.at("in_game_controls")==true,"V2 requires in-game control ownership");c.in_game_controls=true;
        if(diagnostics){demand(j.at("exception_diagnostics").is_boolean(),"Exception diagnostics must be an explicit boolean");c.exception_diagnostics=j.at("exception_diagnostics");}
        return c;
    }
    c.console_sha256=j.at("console_sha256");demand(hash(c.console_sha256),"Installation console hash malformed");
    c.console=absolute(j.at("console_path").get<std::string>());
    demand(winpath::same_spelling(c.console.filename(),L"overglaze_console.exe")&&winpath::same_spelling(c.console.parent_path().parent_path(),c.output_root),"Installation console escaped managed Lab paths");
    demand(j.at("open_console").is_boolean(),"open_console must be boolean");c.open_console=j.at("open_console");return c;
}
Installation load_installation(HMODULE module,const json& exe,const json& host){
    // An Xbox app title runs through the OS package layout, whose junctions the
    // contract below refuses by design. Validate the real files instead -- and
    // only for exactly that layout (lab_package_identity.hpp); any other
    // reparse point is left in place and refused exactly as before.
    auto real_exe=exe,real_host=host;
    real_exe["path"]=utf8(resolve_package_layout(absolute(exe.at("path").get<std::string>())).wstring());
    real_host["path"]=utf8(resolve_package_layout(absolute(host.at("path").get<std::string>())).wstring());
    return load_installation_file(resolve_package_layout(module_path(module)).parent_path()/identity::kInstallationFile,real_exe,real_host);
}
Installation load_installation_file(const std::filesystem::path& p,const json& exe,const json& host){
    const auto host_path=absolute(host.at("path").get<std::string>()),exe_path=absolute(exe.at("path").get<std::string>());
    demand(p.is_absolute()&&p==p.lexically_normal(),"Config path must be absolute and normalized");
    winpath::require_same_file(p,host_path.parent_path()/identity::kInstallationFile);
    demand(std::filesystem::file_size(p)<=16384,"Installation config exceeds 16 KiB");
    std::ifstream file(p,std::ios::binary);const auto document=json::parse(file);auto c=parse_installation(document,exe,host);
    // V4: the recorded data root must be an existing directory on a local fixed
    // disk with no reparse point anywhere on the way -- the rules the out-of-game
    // programs apply to the root they resolve (lab_root_locator.hpp).
    localpath::require_local_fixed(c.output_root,true,{},"安装记录的数据目录不可用","data-root");
    demand(std::filesystem::is_directory(c.output_root),"Installation output root is not a directory");
    winpath::require_same_file(exe_path,c.game_directory/wide(c.facts.executable));
    winpath::require_same_file(host_path,c.loader_directory/wide(c.loader.basename));
    winpath::require_same_file(absolute(document.at("output_root").get<std::string>()),c.output_root);
    // Hashes supplied by the loaded-image observation remain mandatory. Also
    // confirm the admitted on-disk files still carry those exact identities.
    demand(game_identity_token(exe_path)==exe.at("sha256").get<std::string>()&&sha256(host_path)==host.at("sha256").get<std::string>(),"Installed executable/host changed during validation");
    no_reparse(c.output_root);no_reparse(c.loader_directory/identity::kBridgeFile);
    if(!c.in_game_controls){winpath::require_same_file(c.console.parent_path().parent_path(),c.output_root);
        no_reparse(c.console);demand(sha256(c.console)==c.console_sha256,"Installed console identity changed");}
    demand(sha256(c.loader_directory/identity::kBridgeFile)==c.bridge_sha256,"Installed bridge identity changed");
    if(!c.package.empty()){
        // Hash chain for data-driven facts: the JSON in the game directory is
        // trusted only through the Lab-owned adapter package that produced it.
        // The manifest pins the executable, the exact payload and the config's
        // own SHA-256; every pinned game module is re-hashed here.
        const auto manifest_path=c.output_root.parent_path()/L"app"/L"adapters"/wide(c.package)/L"package.json";
        no_reparse(manifest_path);
        demand(std::filesystem::is_regular_file(manifest_path)&&std::filesystem::file_size(manifest_path)<=64*1024,"Adapter package manifest missing");
        std::ifstream m(manifest_path,std::ios::binary);const auto manifest=json::parse(m);
        demand(manifest.is_object()&&manifest.value("schema","")=="overglaze-adapter-package-v1"&&manifest.value("name","")==c.package&&
            manifest.value("profile","")==c.profile&&manifest.value("executable","")==c.facts.executable&&manifest.contains("pins")&&manifest.contains("payload"),"Adapter package does not describe this installation");
        demand(manifest.at("pins").value(c.facts.executable,"")==exe.at("sha256").get<std::string>()&&
            manifest.at("payload").value(c.loader.basename,"")==host.at("sha256").get<std::string>()&&
            manifest.at("payload").value("overglaze_nvngx.dll","")==c.bridge_sha256&&
            manifest.value("config_sha256","")==sha256(p),"Installed files are not the adapter package's exact payload");
        // A pinned model is the package's own model, through the same chain.
        demand(c.model_sha256.empty()||manifest.at("payload").value("nvngx_dlssnr.dll","")==c.model_sha256,"Pinned model is not the adapter package's model");
        const auto module_dirs=module_directories(c.game_directory); // walked once, not per module
        for(const auto& [name,pin]:c.facts.modules){
            demand(manifest.at("pins").value(name,"")==pin,"Module pin differs from the adapter package");
            // A UE title keeps its DLSS plugin under Engine\Plugins, not beside
            // the EXE. Looking only beside the EXE made a module that was simply
            // elsewhere fail as "path missing or contains a reparse point", which
            // named the wrong thing entirely (Halo: Campaign Evolved).
            const auto module=find_module(module_dirs,wide(name));
            demand(!module.empty(),"Pinned game module is no longer in the game directory");
            no_reparse(module);demand(sha256(module)==pin,"Pinned game module changed");}
    }
    return c;
}
void activate_installation(Installation& c){
    demand(std::filesystem::space(c.output_root).available>=30ULL*1024*1024*1024,"30 GiB disk reserve required");
    auto id=uuid();id.erase(std::remove_if(id.begin(),id.end(),[](char c){return c=='{'||c=='}';}),id.end());
    c.run=c.output_root/("daily-"+id);demand(std::filesystem::create_directory(c.run),"Daily run must be unique");
    json receipt={{"version",1},{"purpose","interactive-use"},{"profile",c.profile},{"pid",GetCurrentProcessId()},
        {"automatic_nr_on",false},{"in_game_controls",c.in_game_controls},{"heavy_sampling",false},{"scene_verified",false},{"raw_texture_files",0},{"bridge_sha256",c.bridge_sha256}};
    std::ofstream file(c.run/L"session.json",std::ios::binary);file<<receipt.dump(2);demand(bool(file),"Cannot save bounded daily receipt");
}
void open_installed_console(const Installation& c){if(!c.open_console)return;
    // Keep the verified binary immutable until CreateProcess has mapped it.
    Handle locked(CreateFileW(c.console.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    demand(locked.valid()&&sha256(c.console)==c.console_sha256,"Console changed after installation validation");
    auto command=L"\""+c.console.wstring()+L"\" --auto-connect";
    STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION process{};
    demand(CreateProcessW(c.console.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,c.console.parent_path().c_str(),&start,&process)!=FALSE,"Cannot open installed console");
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
}
}
