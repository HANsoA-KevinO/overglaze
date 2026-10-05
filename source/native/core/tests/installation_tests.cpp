// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_installation.hpp"
#include "lab_package_identity.hpp"
#include "lab_game_profile.hpp"
#include "lab_control.hpp"
#include "lab_windows_path.hpp"
#include "lab_identity.hpp"
#include <iostream>
#include <fstream>
int wmain(int argc,wchar_t** argv){try{
    if(argc==2||argc==3){const std::filesystem::path p=argv[1];
        if(std::filesystem::file_size(p)>16384)throw std::runtime_error("Config exceeds bound");
        std::ifstream file(p);const auto config=lab::json::parse(file);
        const auto* profile=lab::profiles::game(config.at("profile").get<std::string>());
        const bool data_driven=config.value("version",0)==3;
        if(!profile&&!data_driven)throw std::runtime_error("Unknown installation profile");
        const std::string exe_name=data_driven?config.at("facts").at("executable").get<std::string>():std::string(profile->executable);
        // The loader sits beside this config and is named by the config itself. A
        // late-loading or plugin-hosted installation is not called dxgi.dll, and
        // its game executable is NOT in the config's directory -- the config lives
        // in a subdirectory of the game -- so the caller must pass the EXE path.
        const auto loader=data_driven&&config.contains("loader")?lab::parse_loader(config.at("loader")):lab::Loader{};
        if(!loader.root()&&argc!=3)throw std::runtime_error("A subdirectory installation must be checked with its game executable path");
        const auto game=argc==3?std::filesystem::path(argv[2]):p.parent_path()/lab::wide(exe_name),host=p.parent_path()/lab::wide(loader.basename);
        auto c=lab::load_installation_file(p,{{"path",lab::utf8(game.wstring())},{"sha256",lab::game_identity_token(game)}},
            {{"path",lab::utf8(host.wstring())},{"sha256",lab::sha256(host)}});
        std::cout<<"PASS installed file identities and paths; no module loaded or game started\n";return 0;}
    if(argc!=1)throw std::runtime_error("Usage: lab_installation_tests [absolute installed config [actual executable path]]");
    const std::string h(64,'a'),b(64,'b');
    lab::json exe={{"path","D:\\Steam\\steamapps\\common\\Cyberpunk 2077\\bin\\x64\\Cyberpunk2077.exe"},
        {"sha256","a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991"}};
    lab::json host={{"path","D:\\Steam\\steamapps\\common\\Cyberpunk 2077\\bin\\x64\\dxgi.dll"},{"sha256",h}};
    lab::json j={{"version",1},{"profile","cyberpunk2077-rr-v1"},{"offline_single_player",true},{"no_anticheat",true},
        {"game_sha256",exe["sha256"]},{"host_sha256",h},{"bridge_sha256",b},{"console_sha256",h},
        {"console_path","D:\\Overglaze\\data\\build-test\\overglaze_console.exe"},
        {"output_root","D:\\Overglaze\\data"},{"open_console",true}};
    auto c=lab::parse_installation(j,exe,host);
    if(!c.open_console||c.profile!="cyberpunk2077-rr-v1")throw std::runtime_error("Positive contract");
    auto reject=[&](lab::json x,lab::json e,lab::json d){bool rejected=false;try{lab::parse_installation(x,e,d);}catch(...){rejected=true;}
        if(!rejected)throw std::runtime_error("Invalid installation accepted");};
    for(const auto* k:{"version","profile","offline_single_player","no_anticheat","game_sha256","host_sha256","bridge_sha256","console_sha256","console_path","output_root","open_console"}){
        auto x=j;x.erase(k);reject(x,exe,host);x=j;x[k]=nullptr;reject(x,exe,host);
    }
    for(const auto* p:{"C:\\Temp\\overglaze_console.exe","D:\\Overglaze\\data\\build-test\\..\\overglaze_console.exe","overglaze_console.exe","\\\\server\\share\\overglaze_console.exe"}){
        auto x=j;x["console_path"]=p;reject(x,exe,host);}
    auto x=j;x["extra"]="--run-anything";reject(x,exe,host);
    auto e=exe;e["sha256"]=b;x=j;x["game_sha256"]=b;reject(x,e,host);
    auto d=host;d["path"]="I:\\Other\\dxgi.dll";reject(j,exe,d);
    auto v2=j;v2["version"]=2;v2.erase("console_path");v2.erase("console_sha256");v2.erase("open_console");v2["in_game_controls"]=true;
    const auto embedded=lab::parse_installation(v2,exe,host);
    for(const auto* name:{"cyberpunk2077.exe","CYBERPUNK2077.EXE","cYbErPuNk2077.ExE"}){
        const auto* profile=lab::profiles::executable(name,exe.at("sha256").get<std::string>());
        if(!profile||profile->id!="cyberpunk2077-rr-v1")throw std::runtime_error("Case variant lost runtime/capture profile");
    }
    if(lab::profiles::executable("cyberpunk2077.exe",b)||lab::profiles::executable("cyberpunk2077.exe.bak",exe.at("sha256").get<std::string>()))throw std::runtime_error("Profile identity weakened");
    auto lower_exe=exe;lower_exe["path"]="d:\\steam\\steamapps\\common\\cyberpunk 2077\\bin\\x64\\cyberpunk2077.exe";
    auto upper_host=host;upper_host["path"]="D:\\STEAM\\STEAMAPPS\\COMMON\\CYBERPUNK 2077\\BIN\\X64\\DXGI.DLL";
    auto lower_config=v2;lower_config["output_root"]="d:\\OVERGLAZE\\DATA";
    // V4: the recorded root is kept as written; a case alias names the same directory.
    if(!lab::winpath::same_spelling(lab::parse_installation(lower_config,lower_exe,upper_host).output_root,embedded.output_root))throw std::runtime_error("Case alias of the recorded root not accepted");
    auto lower_console=j;lower_console["console_path"]="d:\\OVERGLAZE\\DATA\\build-test\\OVERGLAZE_CONSOLE.EXE";
    lab::parse_installation(lower_console,lower_exe,upper_host);
    for(const auto* path:{"I:\\Other\\Cyberpunk2077.exe","D:\\Steam\\steamapps\\common\\Cyberpunk 2077\\bin\\x64\\Cyberpunk2077.exe.bak","D:\\Steam\\steamapps\\common\\Cyberpunk 2077\\bin\\x64\\..\\Cyberpunk2077.exe"}){
        auto bad=exe;bad["path"]=path;reject(v2,bad,host);
    }
    if(!embedded.in_game_controls||embedded.open_console||!embedded.console.empty())throw std::runtime_error("V2 must not depend on a viewer executable");
    if(embedded.exception_diagnostics)throw std::runtime_error("Diagnostics must default OFF");
    auto diagnostic=v2;diagnostic["exception_diagnostics"]=true;
    if(!lab::parse_installation(diagnostic,exe,host).exception_diagnostics)throw std::runtime_error("Explicit diagnostic opt-in lost");
    diagnostic["exception_diagnostics"]=false;
    if(lab::parse_installation(diagnostic,exe,host).exception_diagnostics)throw std::runtime_error("Explicit diagnostic OFF lost");
    for(const auto& invalid:{lab::json(nullptr),lab::json(1),lab::json("true"),lab::json::object()}){diagnostic["exception_diagnostics"]=invalid;reject(diagnostic,exe,host);}
    diagnostic=j;diagnostic["exception_diagnostics"]=true;reject(diagnostic,exe,host);
    for(auto it=v2.begin();it!=v2.end();++it){auto bad=v2;bad.erase(it.key());reject(bad,exe,host);bad=v2;bad[it.key()]=nullptr;reject(bad,exe,host);}
    x=v2;x["in_game_controls"]=false;reject(x,exe,host);x=v2;x["open_console"]=false;reject(x,exe,host);
    const auto* re9=lab::profiles::game("re9-rr-v1");
    if(!re9||re9->settings_file==lab::profiles::game("cyberpunk2077-rr-v1")->settings_file)throw std::runtime_error("Game preferences must be separate");
    auto r=v2;r["profile"]=re9->id;r["game_sha256"]=re9->executable_sha256;
    auto re=exe;re["sha256"]=re9->executable_sha256;re["path"]="I:\\Re9\\re9.exe";
    auto rh=host;rh["path"]="I:\\Re9\\dxgi.dll";
    if(lab::parse_installation(r,re,rh).profile!=re9->id)throw std::runtime_error("Second game installation");
    reject(r,exe,host);reject(v2,re,rh);
    auto wrong=r;wrong["offline_single_player"]=false;reject(wrong,re,rh);
    wrong=r;wrong["no_anticheat"]=false;reject(wrong,re,rh);
    auto wrong_exe=re;wrong_exe["path"]="I:\\Re9\\Cyberpunk2077.exe";reject(r,wrong_exe,rh);
    wrong=r;wrong["game_sha256"]=b;wrong_exe=re;wrong_exe["sha256"]=b;reject(wrong,wrong_exe,rh);
    const auto* first_light=lab::profiles::game("007-first-light-rr-v1");
    if(!first_light||first_light->settings_file==re9->settings_file||first_light->settings_file==lab::profiles::games[0].settings_file)throw std::runtime_error("007 isolated preferences");
    auto fl_config=v2;fl_config["profile"]=first_light->id;fl_config["game_sha256"]=first_light->executable_sha256;
    auto fl_exe=exe;fl_exe["sha256"]=first_light->executable_sha256;fl_exe["path"]="I:\\First Light\\Retail\\007FirstLight.exe";
    auto fl_host=host;fl_host["path"]="I:\\First Light\\Retail\\dxgi.dll";
    if(lab::parse_installation(fl_config,fl_exe,fl_host).profile!=first_light->id)throw std::runtime_error("007 experimental installation contract");
    reject(fl_config,exe,host);reject(v2,fl_exe,fl_host);
    for(const auto* field:{"offline_single_player","no_anticheat"}){auto bad=fl_config;bad[field]=false;reject(bad,fl_exe,fl_host);}
    auto changed=fl_config;changed["game_sha256"]=b;auto changed_exe=fl_exe;changed_exe["sha256"]=b;reject(changed,changed_exe,fl_host);
    // Alan Wake 2: viewport 1 AND linear depth are both MEASURED in the running
    // game (two late attaches). The game never tags hardware depth;
    // its inline set carries linear depth (49) instead, the same shape as 007.
    // Still unmeasured, and asserted here so the distinction stays visible:
    // native_evaluate_host_rebind and the exposure default. There is still NO
    // reviewed interposer of any kind; the public path carries it.
    const auto* aw2=lab::profiles::game("alanwake2-rr-v1");
    if(!aw2||aw2->viewport!=1||!aw2->linear_depth||aw2->native_evaluate_host_rebind||aw2->default_exposure_stops!=0.f||
       aw2->settings_file==first_light->settings_file||aw2->settings_file==re9->settings_file||aw2->settings_file==lab::profiles::games[0].settings_file)throw std::runtime_error("Alan Wake 2 default-hypothesis profile");
    if(!lab::profiles::executable("AlanWake2.exe",aw2->executable_sha256)||lab::profiles::executable("AlanWake2.exe",first_light->executable_sha256))throw std::runtime_error("Alan Wake 2 executable identity");
    auto aw_config=v2;aw_config["profile"]=aw2->id;aw_config["game_sha256"]=aw2->executable_sha256;
    auto aw_exe=exe;aw_exe["sha256"]=aw2->executable_sha256;aw_exe["path"]="D:\\Games\\AlanWake2\\AlanWake2.exe";
    auto aw_host=host;aw_host["path"]="D:\\Games\\AlanWake2\\dxgi.dll";
    if(lab::parse_installation(aw_config,aw_exe,aw_host).profile!=aw2->id)throw std::runtime_error("Alan Wake 2 installation contract");
    reject(aw_config,exe,host);reject(v2,aw_exe,aw_host);reject(fl_config,aw_exe,aw_host);
    for(const auto* field:{"offline_single_player","no_anticheat"}){auto bad=aw_config;bad[field]=false;reject(bad,aw_exe,aw_host);}
    // V3: data-driven facts. A compiled row must agree exactly; an unknown
    // profile is accepted by parse (shape only) and bound to its package chain
    // later in load_installation_file. Reserved ids and malformed derivations
    // are refused.
    auto facts_of=[](const lab::profiles::Facts& f,const std::string& title){return lab::json{{"executable",f.executable},{"title",title},{"route",f.route},
        {"viewport",f.viewport},{"linear_depth",f.linear_depth},{"native_evaluate_host_rebind",f.native_evaluate_host_rebind},{"binding_preservation",f.binding_preservation},
        {"default_exposure_stops",f.default_exposure_stops},{"settings_file",f.settings_file},{"capture_origin",f.capture_origin},
        {"modules",{{"sl.interposer.dll","5032e5f5e76260815ac309d58d9bd4bf6da5d55873c216c7141c3f192ed38105"}}}};};
    auto v3=v2;v3["version"]=3;v3["exception_diagnostics"]=true;v3["package"]="alanwake2";v3["profile"]=aw2->id;v3["game_sha256"]=aw2->executable_sha256;
    v3["facts"]=facts_of(lab::profiles::Facts::from(*aw2),"Alan Wake 2");
    {const auto parsed=lab::parse_installation(v3,aw_exe,aw_host);
     if(!parsed.facts.reviewed||parsed.package!="alanwake2"||parsed.facts.executable!="AlanWake2.exe"||parsed.facts.modules.size()!=1||!parsed.exception_diagnostics||parsed.facts.settings_file!=aw2->settings_file)throw std::runtime_error("V3 reviewed facts lost");}
    // Diverging from the reviewed row in EITHER direction is refused. The row
    // holds the measured viewport=1 / linear_depth=true, so the divergent values
    // are the default hypothesis (viewport=0 / linear_depth=false).
    {auto bad=v3;bad["facts"]["viewport"]=0;reject(bad,aw_exe,aw_host);bad=v3;bad["facts"]["linear_depth"]=false;reject(bad,aw_exe,aw_host);
     bad=v3;bad["facts"]["default_exposure_stops"]=5.f;reject(bad,aw_exe,aw_host);bad=v3;bad["facts"]["binding_preservation"]=true;reject(bad,aw_exe,aw_host);}
    {auto bad=v3;bad.erase("package");reject(bad,aw_exe,aw_host);bad=v3;bad.erase("exception_diagnostics");reject(bad,aw_exe,aw_host);
     bad=v3;bad.erase("facts");reject(bad,aw_exe,aw_host);bad=v3;bad["facts"].erase("modules");reject(bad,aw_exe,aw_host);
     bad=v3;bad["facts"]["modules"]=lab::json::object();reject(bad,aw_exe,aw_host);bad=v3;bad["facts"]["modules"]={{"sl.common.dll",std::string(64,'c')}};reject(bad,aw_exe,aw_host);
     bad=v3;bad["facts"]["route"]="ngx";reject(bad,aw_exe,aw_host);bad=v3;bad["facts"]["extra"]=1;reject(bad,aw_exe,aw_host);
     bad=v3;bad["facts"]["settings_file"]="overlay-007-first-light.json";reject(bad,aw_exe,aw_host);}
    // The NGX-direct routes parse so that an observation package can be
    // installed at all. Parsing is NOT admission: the host builds no provider
    // on these routes. The capture origin still has to derive from the route,
    // with ngx-rr taking the rr stage and ngx-sr the sr stage, and a bare
    // "ngx" or a crossed stage is still refused (the cases just above and
    // below this one).
    {for(const auto* route:{"ngx-rr","ngx-sr","sl-sr"}){
        const std::string name(route);const std::string stage=name.substr(name.size()-2);
        // A row-reviewed profile pins sl-rr, so these travel as unreviewed facts.
        auto ok=v3;ok["facts"]["route"]=route;
        ok["profile"]="halo-campaign-evolved-rr-v1";ok["package"]="halo";
        ok["facts"]["settings_file"]="overlay-halo.json";
        ok["facts"]["capture_origin"]="halo-controlled-"+stage+"-stage";
        // Each route pins its own driven module: interposer for Streamline,
        // the NGX model for NGX-direct (a pure-NGX title has no interposer).
        const std::string model=name=="ngx-rr"?"nvngx_dlssd.dll":name=="ngx-sr"?"nvngx_dlss.dll":"sl.interposer.dll";
        ok["facts"]["modules"]=lab::json{{model,std::string(64,'e')}};
        const auto parsed=lab::parse_installation(ok,aw_exe,aw_host);
        if(parsed.facts.route!=route||parsed.facts.reviewed)throw std::runtime_error("NGX-direct route must parse as unreviewed facts");
        auto crossed=ok;crossed["facts"]["capture_origin"]="halo-controlled-"+std::string(stage=="rr"?"sr":"rr")+"-stage";
        reject(crossed,aw_exe,aw_host);}
     // Refusals: an NGX route that pins only the interposer, and a Streamline
     // route that pins no interposer.
     auto rr=v3;rr["profile"]="halo-campaign-evolved-rr-v1";rr["package"]="halo";rr["facts"]["route"]="ngx-rr";
     rr["facts"]["settings_file"]="overlay-halo.json";rr["facts"]["capture_origin"]="halo-controlled-rr-stage";
     rr["facts"]["modules"]=lab::json{{"sl.interposer.dll",std::string(64,'e')}};reject(rr,aw_exe,aw_host);
     auto sr=rr;sr["facts"]["route"]="sl-sr";sr["facts"]["capture_origin"]="halo-controlled-sr-stage";
     sr["facts"]["modules"]=lab::json{{"nvngx_dlss.dll",std::string(64,'e')}};reject(sr,aw_exe,aw_host);}
    // ---- the loader block: where this installation's own files live.
    // Absent means the original layout, and it must stay byte-for-byte the same
    // decision as before the key existed.
    {const auto parsed=lab::parse_installation(v3,aw_exe,aw_host);
     if(!parsed.loader.root()||parsed.loader.basename!="dxgi.dll"||!parsed.loader.subdir.empty())throw std::runtime_error("Absent loader block must mean the root dxgi layout");
     if(parsed.loader_directory!=parsed.game_directory)throw std::runtime_error("Root layout resolves the loader directory to the game directory");}
    // Stating the root strategy explicitly is the same installation.
    {auto rooted=v3;rooted["loader"]={{"strategy","root_dxgi_minimal"},{"basename","dxgi.dll"},{"subdir",""}};
     const auto parsed=lab::parse_installation(rooted,aw_exe,aw_host);
     if(!parsed.loader.root()||parsed.loader_directory!=parsed.game_directory)throw std::runtime_error("Explicit root strategy differs from the default");}
    // Late loading: the loader lives in a subdirectory and is NOT called dxgi.dll,
    // because a late loader named dxgi.dll would be caught by the very import
    // redirection it exists to avoid.
    {auto late=v3;late["loader"]={{"strategy","late_d3d12"},{"basename","overglaze_controller.dll"},{"subdir","overglaze"}};
     auto late_host=aw_host;late_host["path"]="D:\\Games\\AlanWake2\\overglaze\\overglaze_controller.dll";
     const auto parsed=lab::parse_installation(late,aw_exe,late_host);
     if(parsed.loader.root()||parsed.loader.basename!="overglaze_controller.dll")throw std::runtime_error("Late strategy lost");
     if(parsed.loader_directory!=parsed.game_directory/"overglaze")throw std::runtime_error("Late loader directory is the game directory plus the subdirectory");
     // The same config with the loader still in the game root is refused.
     reject(late,aw_exe,aw_host);
     if(!parsed.loader.late_host()||parsed.loader.root_proxy()||parsed.loader.on_insert())throw std::runtime_error("Late strategy predicates");}
    // Root proxy on Insert (RE9): the thin proxy in the game root,
    // the host in the subdirectory, arriving late -- both predicates hold.
    {auto deferred=v3;deferred["loader"]={{"strategy","root_proxy_on_insert"},{"basename","overglaze_controller.dll"},{"subdir","overglaze"}};
     auto deferred_host=aw_host;deferred_host["path"]="D:\\Games\\AlanWake2\\overglaze\\overglaze_controller.dll";
     const auto parsed=lab::parse_installation(deferred,aw_exe,deferred_host);
     if(!parsed.loader.root_proxy()||!parsed.loader.on_insert()||!parsed.loader.late_host()||parsed.loader.proxy_basename()!="dxgi.dll")
         throw std::runtime_error("Root-proxy-on-Insert predicates");
     if(parsed.loader_directory!=parsed.game_directory/"overglaze")throw std::runtime_error("On-Insert loader directory is the game directory plus the subdirectory");
     auto misnamed=deferred;misnamed["loader"]["basename"]="dxgi.dll";reject(misnamed,aw_exe,deferred_host);}
    {auto proxied=v3;proxied["loader"]={{"strategy","root_proxy_d3d12"},{"basename","overglaze_controller.dll"},{"subdir","overglaze"}};
     auto proxied_host=aw_host;proxied_host["path"]="D:\\Games\\AlanWake2\\overglaze\\overglaze_controller.dll";
     const auto parsed=lab::parse_installation(proxied,aw_exe,proxied_host);
     if(!parsed.loader.root_proxy()||parsed.loader.on_insert()||parsed.loader.late_host())throw std::runtime_error("Root-proxy predicates");}
    {auto plugin=v3;plugin["loader"]={{"strategy","reframework_plugin"},{"basename","overglaze_reframework.dll"},{"subdir","reframework/plugins"}};
     auto plugin_host=aw_host;plugin_host["path"]="D:\\Games\\AlanWake2\\reframework\\plugins\\overglaze_reframework.dll";
     const auto parsed=lab::parse_installation(plugin,aw_exe,plugin_host);
     if(parsed.loader_directory!=parsed.game_directory/"reframework"/"plugins")throw std::runtime_error("Plugin loader directory lost");}
    // Every way the block could name the wrong file or escape the game directory.
    {auto late=v3;late["loader"]={{"strategy","late_d3d12"},{"basename","overglaze_controller.dll"},{"subdir","overglaze"}};
     auto late_host=aw_host;late_host["path"]="D:\\Games\\AlanWake2\\overglaze\\overglaze_controller.dll";
     for(const auto* escape:{"..","overglaze\\..\\..","..\\overglaze","C:\\Windows\\System32","\\\\server\\share","a\\b\\c\\d",".","dls slab","overglaze.","dls:slab"}){
        auto bad=late;bad["loader"]["subdir"]=escape;reject(bad,aw_exe,late_host);}
     // A late loader must not be spelled like the proxy, and a root loader must not be spelled like anything else.
     {auto bad=late;bad["loader"]["basename"]="dxgi.dll";reject(bad,aw_exe,late_host);}
     {auto bad=late;bad["loader"]["basename"]="d3d12.dll";reject(bad,aw_exe,late_host);}
     {auto bad=late;bad["loader"]["subdir"]="";reject(bad,aw_exe,late_host);}
     {auto bad=v3;bad["loader"]={{"strategy","root_dxgi_minimal"},{"basename","overglaze_controller.dll"},{"subdir",""}};reject(bad,aw_exe,aw_host);}
     {auto bad=v3;bad["loader"]={{"strategy","root_dxgi_minimal"},{"basename","dxgi.dll"},{"subdir","overglaze"}};reject(bad,aw_exe,aw_host);}
     {auto bad=late;bad["loader"]["strategy"]="anything_goes";reject(bad,aw_exe,late_host);}
     // The block is exactly three keys, and only the V3 contract may carry it.
     {auto bad=late;bad["loader"].erase("subdir");reject(bad,aw_exe,late_host);}
     {auto bad=late;bad["loader"]["extra"]=1;reject(bad,aw_exe,late_host);}
     {auto bad=late;bad["loader"]="overglaze";reject(bad,aw_exe,late_host);}
     {auto bad=v2;bad["loader"]={{"strategy","late_d3d12"},{"basename","overglaze_controller.dll"},{"subdir","overglaze"}};reject(bad,exe,host);}}

    // 007 facts under the 007 row still bind binding_preservation=true and viewport 1.
    {auto fl=v3;fl["package"]="007-first-light";fl["profile"]=first_light->id;fl["game_sha256"]=first_light->executable_sha256;
     fl["facts"]=facts_of(lab::profiles::Facts::from(*first_light),"007 First Light");
     const auto parsed=lab::parse_installation(fl,fl_exe,fl_host);if(!parsed.facts.binding_preservation||parsed.facts.viewport!=1||!parsed.facts.linear_depth||parsed.facts.default_exposure_stops!=5.f)throw std::runtime_error("007 facts from row");}
    // Unknown profile: accepted on shape, not reviewed, package-derived names enforced.
    lab::profiles::Facts sample;sample.executable="Sample.exe";sample.route="sl-rr";sample.settings_file="overlay-sample.json";sample.capture_origin="sample-controlled-rr-stage";
    auto s3=v3;s3["package"]="sample";s3["profile"]="sample-rr-v1";s3["game_sha256"]=b;s3["facts"]=facts_of(sample,"Sample Game");
    auto s_exe=exe;s_exe["sha256"]=b;s_exe["path"]="I:\\Games\\Sample\\Sample.exe";auto s_host=host;s_host["path"]="I:\\Games\\Sample\\dxgi.dll";
    {const auto parsed=lab::parse_installation(s3,s_exe,s_host);if(parsed.facts.reviewed||parsed.profile!="sample-rr-v1"||parsed.package!="sample"||parsed.facts.viewport!=0)throw std::runtime_error("V3 unknown profile facts");}
    {auto bad=s3;bad["profile"]="synthetic-standalone-fixture";reject(bad,s_exe,s_host);bad=s3;bad["profile"]="Sample RR";reject(bad,s_exe,s_host);
     bad=s3;bad["facts"]["capture_origin"]="sample-controlled-sr-stage";reject(bad,s_exe,s_host);bad=s3;bad["facts"]["executable"]="..\\Sample.exe";reject(bad,s_exe,s_host);
     bad=s3;bad["facts"]["viewport"]=16;reject(bad,s_exe,s_host);bad=s3;bad["facts"]["default_exposure_stops"]=11;reject(bad,s_exe,s_host);
     bad=s3;bad["version"]=2;reject(bad,s_exe,s_host); // an unknown profile never rides the V2 contract
     auto other=s_exe;other["path"]="I:\\Games\\Sample\\Other.exe";reject(s3,other,s_host);}
    {lab::Controller status;bool refused=false;try{status.publish_nr_runtime({{"profile","sample-rr-v1"},{"state","waiting-frame"}});}catch(const std::logic_error&){refused=true;}
     if(!refused)throw std::runtime_error("Unaccepted V3 profile published");
     status.accept_game_profile("sample-rr-v1");status.publish_nr_runtime({{"profile","sample-rr-v1"},{"state","waiting-frame"}});
     if(status.status()["nr_runtime"]["profile"]!="sample-rr-v1")throw std::runtime_error("Accepted V3 profile not published");
     refused=false;try{status.accept_game_profile("another-rr-v1");}catch(const std::logic_error&){refused=true;}if(!refused)throw std::runtime_error("Second profile acceptance not refused");}
    for(const auto* id:{"cyberpunk-rr-experimental-v1","cyberpunk2077-rr-v1","re9-rr-v1","007-first-light-rr-v1","alanwake2-rr-v1","synthetic-standalone-fixture"}){
        lab::Controller status;status.publish_nr_runtime({{"profile",id},{"state","waiting-frame"}});
        if(status.status()["nr_runtime"]["profile"]!=id)throw std::runtime_error("Profile not preserved across controller publication");
        bool refused=false;try{status.publish_nr_runtime({{"profile","unknown"},{"state","ready"}});}catch(const std::logic_error&){refused=true;}
        if(!refused||status.status()["nr_runtime"]["profile"]!=id)throw std::runtime_error("Unknown runtime profile mutated last good status");
    }
    // ---- host contract V4: the receipt's data root, not a compiled path.
    {auto v4=v2;v4["output_root"]="D:\\Elsewhere\\Overglaze\\data";
     if(lab::parse_installation(v4,exe,host).output_root!=std::filesystem::path(L"D:\\Elsewhere\\Overglaze\\data"))throw std::runtime_error("V4: the recorded data root is the root");
     // A UNC spelling parses; load_installation_file refuses it with the disk rules.
     for(const auto* bad:{"relative\\data","D:\\","D:\\x\\..\\data"}){auto y=v2;y["output_root"]=bad;reject(y,exe,host);}}
    // The silent stand-down decision and the disk rules, on real files in a
    // temporary directory (never the Lab's own data root).
    {std::wstring tmp(MAX_PATH+1,L'\0');tmp.resize(GetTempPathW(DWORD(tmp.size()),tmp.data()));
     const auto base=std::filesystem::path(tmp).lexically_normal()/lab::wide("overglaze-installation-test-"+lab::uuid());
     std::filesystem::create_directories(base/L"lab"/L"data");std::filesystem::create_directories(base/L"lab"/L"app");std::filesystem::create_directories(base/L"game");
     const auto config=base/L"game"/lab::identity::kInstallationFile;
     auto write=[&](const lab::json& j){std::ofstream out(config,std::ios::binary|std::ios::trunc);out<<j.dump(2);};
     auto data=(base/L"lab"/L"data").lexically_normal();
     auto v4=v2;v4["output_root"]=lab::utf8(data.wstring());write(v4);
     if(lab::recorded_output_root(config)!=data)throw std::runtime_error("V4: recorded root not read back");
     if(lab::installation_root_missing(config))throw std::runtime_error("V4: an existing root reported missing");
     auto v3=v4;v3["package"]="sample";write(v3);
     if(lab::installation_root_missing(config))throw std::runtime_error("V4: an existing program folder reported missing");
     std::filesystem::remove(base/L"lab"/L"app");
     if(!lab::installation_root_missing(config))throw std::runtime_error("V4: a deleted program folder must stand the host down silently");
     write(v4);if(lab::installation_root_missing(config))throw std::runtime_error("V4: a receipt without a package does not need the program folder");
     std::filesystem::remove(data);
     if(!lab::installation_root_missing(config))throw std::runtime_error("V4: a deleted data root must stand the host down silently");
     {std::ofstream out(config,std::ios::binary|std::ios::trunc);out<<"{broken";}
     if(lab::installation_root_missing(config)||lab::recorded_output_root(config))throw std::runtime_error("V4: an unreadable receipt is reported by the full contract, not silenced");
     std::filesystem::remove_all(base);}
    std::cout<<"PASS identity-bound offline installation contract, V4 recorded data root and silent stand-down, malformed/escape negatives; no game loaded\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
