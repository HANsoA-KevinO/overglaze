// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Self-update (lab_self_update.hpp) without the network: version order, the
// signature with ephemeral CNG keys (never the release key), release selection
// from canned GitHub JSON, manifest validation, the install/portable decision,
// download folders, check + download + verify end to end against a loopback
// HTTP server through the loopback test endpoint, and the offer cache.
//
//   lab_self_update_tests [--viewer <overglaze_viewer.exe>]
//   lab_self_update_tests --verify <manifest> <signature> <public blob>   (for test_sign_release.py)
#include "lab_self_update.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
namespace fs=std::filesystem;
namespace u=lab::update;
using lab::json;
namespace {
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>std::string refusal(F f){try{f();}catch(const std::exception& e){return e.what();}return {};}
template<class F>void refuses(F f,const std::string& contains,const std::string& why){
    const auto what=refusal(f);need(!what.empty(),why+": no refusal");need(what.find(contains)!=std::string::npos,why+": "+what);}
std::string read(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void write(const fs::path& p,const std::string& s){fs::create_directories(p.parent_path());std::ofstream out(p,std::ios::binary);out<<s;need(bool(out),"fixture written");}
u::Version v(const char* text){const auto parsed=u::parse_version(text);need(parsed.has_value(),std::string("parse ")+text);return *parsed;}

std::string base64(const std::vector<unsigned char>& b){
    static const char* a="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string s;
    for(std::size_t i=0;i<b.size();i+=3){const std::uint32_t n=(std::uint32_t(b[i])<<16)|(i+1<b.size()?std::uint32_t(b[i+1])<<8:0)|(i+2<b.size()?b[i+2]:0);
        s+=a[(n>>18)&63];s+=a[(n>>12)&63];s+=i+1<b.size()?a[(n>>6)&63]:'=';s+=i+2<b.size()?a[n&63]:'=';}
    return s;}
std::vector<unsigned char> digest(std::string_view m){
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE h=nullptr;std::vector<unsigned char> d(32);
    need(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0&&BCryptCreateHash(alg,&h,nullptr,0,nullptr,0,0)==0,"sha init");
    need(BCryptHashData(h,reinterpret_cast<PUCHAR>(const_cast<char*>(m.data())),ULONG(m.size()),0)==0&&BCryptFinishHash(h,d.data(),32,0)==0,"sha");
    BCryptDestroyHash(h);BCryptCloseAlgorithmProvider(alg,0);return d;}
// An ephemeral P-256 key that exists only in this process.
struct Key {
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_KEY_HANDLE key=nullptr;std::vector<unsigned char> pub;
    Key(){need(BCryptOpenAlgorithmProvider(&alg,BCRYPT_ECDSA_P256_ALGORITHM,nullptr,0)==0,"ecdsa provider");
        need(BCryptGenerateKeyPair(alg,&key,256,0)==0&&BCryptFinalizeKeyPair(key,0)==0,"ephemeral key");
        ULONG n=0;need(BCryptExportKey(key,nullptr,BCRYPT_ECCPUBLIC_BLOB,nullptr,0,&n,0)==0,"export size");pub.resize(n);
        need(BCryptExportKey(key,nullptr,BCRYPT_ECCPUBLIC_BLOB,pub.data(),n,&n,0)==0&&n==72,"export public blob");}
    ~Key(){if(key)BCryptDestroyKey(key);if(alg)BCryptCloseAlgorithmProvider(alg,0);}
    std::vector<unsigned char> raw(std::string_view m)const{auto d=digest(m);ULONG n=0;
        need(BCryptSignHash(key,nullptr,d.data(),32,nullptr,0,&n,0)==0&&n==64,"signature size");std::vector<unsigned char> s(n);
        need(BCryptSignHash(key,nullptr,d.data(),32,s.data(),n,&n,0)==0,"sign");return s;}
    std::string sign(std::string_view m)const{return base64(raw(m))+"\n";}
};

json manifest_json(const std::string& version,const std::string& setup_bytes){
    return {{"schema","overglaze-update-v1"},{"version",version},{"channel","preview"},{"published","2026-10-20"},
        {"source_commit",std::string(40,'a')},{"notes_zh","修复若干问题。\n第二行"},
        {"setup",{{"name","Overglaze-Setup-"+version+"-win64.exe"},{"size",setup_bytes.size()},{"sha256",u::sha256_hex(setup_bytes)}}},
        {"portable",{{"name","Overglaze-"+version+"-win64.zip"},{"size",1234},{"sha256",std::string(64,'b')}}}};}

void versions(){
    need(u::compare(v("0.2.0-preview.10"),v("0.2.0-preview.9"))>0,"preview.10 > preview.9 (numeric)");
    need(u::compare(v("0.2.0"),v("0.2.0-preview.10"))>0,"a stable release outranks its previews");
    need(u::compare(v("0.2.0-preview.6"),v("0.2.0-preview.6"))==0,"equal");
    need(u::compare(v("0.10.0"),v("0.9.9"))>0&&u::compare(v("1.0.0"),v("0.99.99"))>0,"numeric core");
    // The SemVer 2.0 example chain.
    const char* chain[]{"1.0.0-alpha","1.0.0-alpha.1","1.0.0-alpha.beta","1.0.0-beta","1.0.0-beta.2","1.0.0-beta.11","1.0.0-rc.1","1.0.0"};
    for(std::size_t i=0;i+1<std::size(chain);++i)need(u::compare(v(chain[i]),v(chain[i+1]))<0&&u::compare(v(chain[i+1]),v(chain[i]))>0,std::string("SemVer order at ")+chain[i]);
    need(v("0.2.0-preview.6").text()=="0.2.0-preview.6","text round trip");
    need(u::parse_tag("v0.2.0-preview.6").has_value()&&u::parse_tag("0.2.0").has_value(),"tags with and without v");
    for(const char* garbage:{"","1.2","1.2.3.4","v1.2.3","01.2.3","1.02.3","1.2.3-","1.2.3-a..b","1.2.3-01","1.2.3+build","1.2.3-a+b",
            "1.2.3-é","a.b.c","1..3","-1.2.3"," 1.2.3","1.2.3 ","nightly","9999999999999999.0.0"})
        need(!u::parse_version(garbage).has_value(),std::string("garbage refused: '")+garbage+"'");
    need(!u::parse_tag("vv1.2.3").has_value()&&!u::parse_tag("V1.2.3").has_value(),"one lowercase v only");
}

void signatures(){
    const Key key,other;
    const std::string message=manifest_json("0.2.0-preview.7","setup").dump(2);
    const auto signature=key.sign(message);std::string why;
    need(u::verify(message,signature,key.pub,&why)&&why.empty(),"ephemeral signature verifies");
    need(u::verify(message,signature.substr(0,signature.size()-1),key.pub),"no trailing newline accepted");
    need(u::verify(message,base64(key.raw(message))+"\r\n",key.pub),"CRLF accepted");
    for(std::size_t i:{std::size_t(0),message.size()/2,message.size()-1}){auto changed=message;changed[i]^=0x01;
        need(!u::verify(changed,signature,key.pub,&why)&&why=="签名与发布密钥不匹配","one flipped manifest byte fails");}
    need(!u::verify(message+" ",signature,key.pub),"an appended byte fails");
    need(!u::verify(message,signature,other.pub,&why)&&why=="签名与发布密钥不匹配","the wrong key fails");
    {auto raw=key.raw(message);raw[10]^=0x80;need(!u::verify(message,base64(raw),key.pub),"a flipped signature byte fails");}
    const auto good=base64(key.raw(message));
    std::vector<std::string> malformed{"","\n",good.substr(0,87),good+"A",good.substr(0,87)+"*",good+"\n"+good+"\n",
        good.substr(0,40)+"="+good.substr(41),base64(std::vector<unsigned char>(63,7)),base64(std::vector<unsigned char>(66,7)),
        base64(std::vector<unsigned char>(65,7)),std::string(88,'='),good+"\n\n"," "+good.substr(1)};
    for(const auto& s:malformed)need(!u::verify(message,s,key.pub,&why)&&why=="签名文件格式无效","malformed signature refused: "+s.substr(0,12));
    need(!u::decode_signature(base64(std::vector<unsigned char>(66,7))).has_value(),"88 characters of 66 bytes is not a signature");
    // 64 bytes end in "==": the character before them carries 2 bits, its low 4 must be zero.
    {auto s=good;need(s.size()==88&&s.substr(86)=="==","64 bytes end in two padding characters");
     const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
     s[85]=alphabet[alphabet.find(s[85])^1];need(!u::decode_signature(s).has_value(),"non-canonical padding bits refused");}
    need(!u::verify(message,signature,std::vector<unsigned char>(key.pub.begin(),key.pub.end()-1),&why)&&why=="公钥格式无效","a short key is refused");
    {auto bad=key.pub;bad[0]='X';need(!u::verify(message,signature,bad,&why)&&why=="公钥格式无效","a key with the wrong magic is refused");}
    // The compiled-in release key: imports, published fingerprint, and the
    // only key official() ever returns. Nothing is signed with it here.
    need(u::sha256_hex(std::string_view(reinterpret_cast<const char*>(u::kReleaseKey.data()),u::kReleaseKey.size()))==u::kReleaseKeySha256,"release key fingerprint");
    need(!u::verify(message,signature,u::kReleaseKey,&why)&&why=="签名与发布密钥不匹配","the release key imports and rejects a test signature");
    const auto official=u::official("0.2.0-preview.6");
    need(std::equal(official.release_key.begin(),official.release_key.end(),u::kReleaseKey.begin(),u::kReleaseKey.end()),"official endpoint carries the release key");
    need(official.releases_api==u::kReleasesApi&&!official.loopback_test&&official.user_agent=="Overglaze/0.2.0-preview.6","official endpoint is fixed");
}

void manifests(){
    const std::string setup="synthetic setup bytes";
    const auto good=manifest_json("0.2.0-preview.7",setup);
    const auto m=u::parse_manifest(good.dump());
    need(m.version=="0.2.0-preview.7"&&m.channel=="preview"&&m.setup.size==setup.size()&&m.setup.sha256==u::sha256_hex(setup)&&m.notes_zh=="修复若干问题。\n第二行","manifest fields");
    for(const char* key:{"schema","version","channel","published","source_commit","notes_zh","setup","portable"}){
        auto j=good;j.erase(key);refuses([&]{u::parse_manifest(j.dump());},key,std::string("missing ")+key);}
    for(const char* key:{"name","size","sha256"}){auto j=good;j["setup"].erase(key);refuses([&]{u::parse_manifest(j.dump());},"更新清单",std::string("missing setup.")+key);}
    auto variant=[&](const std::function<void(json&)>& change,const char* why){auto j=good;change(j);refuses([&]{u::parse_manifest(j.dump());},"更新清单",why);};
    variant([](json& j){j["schema"]="overglaze-update-v2";},"unknown schema");
    variant([](json& j){j["version"]="0.2";},"bad version");
    variant([](json& j){j["version"]=7;},"version type");
    variant([](json& j){j["channel"]="nightly";},"bad channel");
    variant([](json& j){j["published"]="20261020";},"bad date");
    variant([](json& j){j["source_commit"]="abc";},"short commit");
    variant([](json& j){j["source_commit"]=std::string(40,'A');},"uppercase commit");
    variant([](json& j){j["setup"]["name"]="..\\Overglaze.exe";},"path in name");
    variant([](json& j){j["setup"]["name"]="Overglaze-Setup.zip";},"setup must be an exe");
    variant([](json& j){j["portable"]["name"]="Overglaze.exe";},"portable must be a zip");
    variant([](json& j){j["setup"]["name"]=".hidden.exe";},"hidden name");
    variant([](json& j){j["setup"]["size"]=0;},"zero size");
    variant([](json& j){j["setup"]["size"]=-5;},"negative size");
    variant([](json& j){j["setup"]["size"]=(2ULL<<30);},"size above bound");
    variant([](json& j){j["setup"]["sha256"]=std::string(63,'a');},"short hash");
    variant([](json& j){j["setup"]["sha256"]=std::string(64,'A');},"uppercase hash");
    variant([](json& j){j["setup"]=json::array();},"setup type");
    refuses([]{u::parse_manifest("[1,2]");},"更新清单","not an object");
    refuses([]{u::parse_manifest("{broken");},"更新清单","not JSON");
    refuses([]{u::parse_manifest(std::string(70*1024,' '));},"更新清单","oversized");
    {auto j=good;j["future_field"]="ignored";need(u::parse_manifest(j.dump()).version=="0.2.0-preview.7","an extra field is ignored");}
}

void files(const fs::path& base){
    const auto file=base/L"verify"/L"Overglaze-Setup.exe";const std::string bytes="downloaded bytes";write(file,bytes);
    const u::FileRef ok{"Overglaze-Setup.exe",bytes.size(),u::sha256_hex(bytes)};
    u::verify_file(file,ok);++checks;
    refuses([&]{u::verify_file(file,{ok.name,ok.size+1,ok.sha256});},"大小","size mismatch of a downloaded file");
    refuses([&]{u::verify_file(file,{ok.name,ok.size,u::sha256_hex("other bytes!!!!!")});},"SHA-256","hash mismatch of a downloaded file");
    refuses([&]{u::verify_file(base/L"verify"/L"missing.exe",ok);},"无法读取","missing download");
}

std::string release(const std::string& tag,bool draft,const std::vector<std::string>& assets,const std::string& base_url="https://github.com/HANsoA-KevinO/overglaze/releases/download/"){
    json a=json::array();for(const auto& name:assets)a.push_back({{"name",name},{"size",100},{"browser_download_url",base_url+tag+"/"+name}});
    return json{{"tag_name",tag},{"draft",draft},{"prerelease",tag.find('-')!=std::string::npos},{"html_url","https://github.com/HANsoA-KevinO/overglaze/releases/tag/"+tag},{"assets",a}}.dump();}
const std::vector<std::string> kSigned{"Overglaze-Setup-x-win64.exe","overglaze-update.json","overglaze-update.json.sig"};
void selection(){
    const auto e=u::official("0.2.0-preview.6");const auto running=v("0.2.0-preview.6");
    auto pick=[&](const std::string& list){return u::select_release(json::parse(list),running,e);};
    {const auto s=pick("["+release("v0.3.0",true,kSigned)+","+release("v0.2.0-preview.7",false,kSigned)+"]");
     need(s.kind==u::Selection::Kind::signed_update&&s.release.tag=="v0.2.0-preview.7","a newer draft is ignored");
     need(s.release.page=="https://github.com/HANsoA-KevinO/overglaze/releases/tag/v0.2.0-preview.7"&&s.release.assets.size()==3,"page and assets kept");}
    {const auto s=pick("["+release("v0.2.0-preview.9",false,kSigned)+","+release("v0.2.0-preview.10",false,kSigned)+"]");
     need(s.release.tag=="v0.2.0-preview.10","preview.10 chosen over preview.9");}
    {const auto s=pick("["+release("v0.2.0-preview.10",false,kSigned)+","+release("v0.2.0",false,kSigned)+"]");need(s.release.tag=="v0.2.0","stable outranks previews");}
    need(pick("["+release("v0.2.0-preview.6",false,kSigned)+","+release("v0.2.0-preview.5",false,kSigned)+"]").kind==u::Selection::Kind::none,"equal and older: nothing newer");
    need(pick("[]").kind==u::Selection::Kind::none&&pick("{}").kind==u::Selection::Kind::none,"empty list or wrong shape");
    {const auto s=pick("["+release("v0.2.0-preview.7",false,{"Overglaze-Setup-x-win64.exe"})+"]");
     need(s.kind==u::Selection::Kind::manual_only&&s.release.tag=="v0.2.0-preview.7","without the manifest assets: manual only");}
    need(pick("["+release("v0.2.0-preview.7",false,{"Overglaze-Setup-x-win64.exe","overglaze-update.json"})+"]").kind==u::Selection::Kind::manual_only,"manifest without signature: manual only");
    need(pick("["+release("v0.2.0-preview.7",false,{"overglaze-update.json.sig"})+"]").kind==u::Selection::Kind::manual_only,"signature without manifest: manual only");
    need(pick("["+release("v0.2.0-preview.7",false,kSigned,"https://example.com/elsewhere/")+"]").kind==u::Selection::Kind::manual_only,"asset URLs outside the repository are dropped");
    {const auto s=pick("["+release("v0.3.0",false,{"Overglaze-Setup-x-win64.exe"})+","+release("v0.2.1",false,kSigned)+"]");
     need(s.kind==u::Selection::Kind::manual_only&&s.release.tag=="v0.3.0","the highest version wins, signed or not");}
    {auto j=json::parse("["+release("v0.2.0-preview.7",false,kSigned)+"]");j[0]["html_url"]="https://example.com/phish";
     need(u::select_release(j,running,e).release.page==u::kReleasesPage,"a foreign page URL falls back to the releases page");}
    {auto j=json::parse("["+release("v0.2.0-preview.7",false,kSigned)+"]");j[0].erase("draft");
     need(u::select_release(j,running,e).kind==u::Selection::Kind::none,"an entry without a draft flag is not trusted");}
    need(pick("[1,\"x\",null,"+release("nightly",false,kSigned)+","+release("v1.2",false,kSigned)+","+release("v01.2.3",false,kSigned)+"]").kind==u::Selection::Kind::none,"garbage entries and tags skipped");
}

void settings(const fs::path& base){
    u::Settings s;need(s.check_on_start&&s.last_check==0,"defaults: check on start, never checked");
    const std::int64_t now=1'800'000'000;
    need(u::automatic_check_due(s,now),"never checked: due");
    s.last_check=now-3600;need(!u::automatic_check_due(s,now),"checked an hour ago: not due");
    s.last_check=now-25*3600;need(u::automatic_check_due(s,now),"checked 25 h ago: due");
    s.last_check=now+3600;need(u::automatic_check_due(s,now),"a check in the future (clock change): due");
    s.last_check=0;s.check_on_start=false;need(!u::automatic_check_due(s,now),"setting off: never automatic");
    s.check_on_start=true;s.last_check=now;s.last_result="已是最新版本";s.pending_from="0.2.0-preview.6";s.pending_to="0.2.0-preview.7";
    const auto back=u::Settings::parse(s.document());
    need(back.check_on_start&&back.last_check==now&&back.last_result==s.last_result&&back.pending_from==s.pending_from&&back.pending_to==s.pending_to,"settings round trip");
    need(u::after_update(s,v("0.2.0-preview.7"))==u::AfterUpdate::updated,"running the pending version: updated");
    need(u::after_update(s,v("0.2.0-preview.8"))==u::AfterUpdate::updated,"running a later version: updated");
    need(u::after_update(s,v("0.2.0-preview.6"))==u::AfterUpdate::failed,"still the old version: failed");
    need(u::after_update(s,v("0.2.0-preview.5"))==u::AfterUpdate::none,"another version: nothing to say");
    need(u::after_update(u::Settings{},v("0.2.0-preview.7"))==u::AfterUpdate::none,"nothing pending");
    need(u::Settings::parse(json{{"kind","overglaze-update-settings"},{"check_on_start","yes"}}).check_on_start,"an invalid field keeps its default");
    need(u::Settings::parse(json::array()).check_on_start,"garbage: defaults");
    const auto data=base/L"settings-data";fs::create_directories(data);
    {u::SettingsStore store(data);store.update([](u::Settings& x){x.check_on_start=false;x.last_check=42;});need(store.error().empty(),"store saved: "+store.error());}
    {u::SettingsStore store(data);const auto x=store.get();need(!x.check_on_start&&x.last_check==42,"store reloads what it saved");}
    need(fs::is_regular_file(data/L"settings"/L"update-v1.json"),"saved with the other settings");
    {u::SettingsStore memory;memory.update([](u::Settings& x){x.last_check=7;});need(memory.get().last_check==7,"memory-only store");}
}

void installation(const fs::path& base){
    const auto root=base/L"installed 中文"/L"Overglaze";const auto data=root/L"data";
    write(root/L"app"/L"overglaze_viewer.exe","x");write(root/L"app"/L"unins000.exe","x");
    write(data/L"settings"/L"application-install.json",json{{"schema","overglaze-application-install-v1"},{"product","overglaze"},{"root",lab::utf8(root.wstring())},{"version","0.2.0-preview.6"}}.dump());
    lab::root::Location at;at.root=root;at.data=data;at.source=lab::root::Source::app_layout;
    const auto registered=[&]{return std::optional<std::wstring>(root.wstring()+L"\\");};
    need(u::detect(at,registered).by_setup,"marker, uninstaller and uninstall entry: installed by Setup");
    need(!u::detect(at,[]{return std::optional<std::wstring>();}).by_setup,"no uninstall entry: portable");
    // Why there is no location, with the Win32 error.
    need(u::detect(at,[]{return u::UninstallLookup{std::nullopt,ERROR_FILE_NOT_FOUND,true};}).why=="Windows 卸载登记里没有这个程序（错误 2）","the entry is missing: error 2");
    need(u::detect(at,[]{return u::UninstallLookup{std::nullopt,ERROR_FILE_NOT_FOUND,false};}).why=="Windows 卸载登记里没有安装位置（错误 2）","the entry has no InstallLocation: error 2");
    need(u::detect(at,[]{return u::UninstallLookup{std::nullopt,ERROR_UNSUPPORTED_TYPE};}).why=="Windows 卸载登记里的安装位置类型不对（错误 1630）","InstallLocation is not a string");
    need(u::detect(at,[]{return u::UninstallLookup{std::nullopt,ERROR_ACCESS_DENIED};}).why=="无法读取 Windows 卸载登记（错误 5）","any other error");
    need(u::detect(at,[]{return u::UninstallLookup{std::wstring()};}).why=="Windows 卸载登记里的安装位置为空","an empty InstallLocation");
    need(u::detect(at,[]{return std::optional<std::wstring>();}).why=="Windows 卸载登记里没有这个程序","a reader with nothing to say");
    need(!u::detect(at,[&]{return std::optional<std::wstring>((base/L"elsewhere").wstring());}).by_setup,"uninstall entry for another folder: portable");
    {auto build=at;build.source=lab::root::Source::build_layout;need(!u::detect(build,registered).by_setup,"a build directory: portable");}
    {auto over=at;over.source=lab::root::Source::override_file;need(!u::detect(over,registered).by_setup,"an override file: portable");}
    fs::remove(root/L"app"/L"unins000.exe");need(!u::detect(at,registered).by_setup,"no uninstaller: portable");write(root/L"app"/L"unins000.exe","x");
    write(data/L"settings"/L"application-install.json",json{{"schema","overglaze-application-install-v1"},{"product","overglaze"},{"root",lab::utf8(base.wstring())}}.dump());
    need(!u::detect(at,registered).by_setup,"a marker for another root: portable");
    write(data/L"settings"/L"application-install.json","{broken");need(!u::detect(at,registered).by_setup,"a broken marker: portable");
    fs::remove(data/L"settings"/L"application-install.json");
    const auto portable=u::detect(at,registered);need(!portable.by_setup&&!portable.why.empty(),"no marker: portable, with the reason");
    need(u::install_refusal(portable,100).find("便携版")!=std::string::npos,"a portable copy is never installed");
    need(u::setup_command_line(L"D:\\Overglaze 中文\\data\\updates\\0.2.0-preview.7\\Overglaze-Setup-0.2.0-preview.7-win64.exe",L"D:\\Overglaze 中文",4242)==
        L"\"D:\\Overglaze 中文\\data\\updates\\0.2.0-preview.7\\Overglaze-Setup-0.2.0-preview.7-win64.exe\" /SILENT /SUPPRESSMSGBOXES /NORESTART /DIR=\"D:\\Overglaze 中文\" /WAITPID=4242 /RELAUNCH",
        "the Setup command line");
}

void marker(const fs::path& folder,const std::string& version,const std::string& setup){
    write(folder/L".overglaze-download.json",json{{"kind","overglaze-update-download"},{"version",version},{"setup",setup}}.dump());}
void cleanup(const fs::path& base){
    const auto data=base/L"cleanup-data";const auto updates=u::updates_dir(data);
    const auto old=updates/L"0.2.0-preview.5";marker(old,"0.2.0-preview.5","Overglaze-Setup-0.2.0-preview.5-win64.exe");
    write(old/L"Overglaze-Setup-0.2.0-preview.5-win64.exe","old");write(old/L"Overglaze-Setup-0.2.0-preview.5-win64.exe.partial","part");
    const auto foreign=updates/L"0.2.0-preview.4";write(foreign/L"Overglaze-Setup-0.2.0-preview.4-win64.exe","not ours");
    const auto shared=updates/L"0.2.0-preview.3";marker(shared,"0.2.0-preview.3","Overglaze-Setup-0.2.0-preview.3-win64.exe");
    write(shared/L"Overglaze-Setup-0.2.0-preview.3-win64.exe","old");write(shared/L"notes.txt","the user's");
    const auto keep=updates/L"0.2.0-preview.7";marker(keep,"0.2.0-preview.7","Overglaze-Setup-0.2.0-preview.7-win64.exe");write(keep/L"Overglaze-Setup-0.2.0-preview.7-win64.exe","new");
    const auto lying=updates/L"0.2.0-preview.2";marker(lying,"0.2.0-preview.1","Overglaze-Setup-0.2.0-preview.2-win64.exe");write(lying/L"Overglaze-Setup-0.2.0-preview.2-win64.exe","x");
    write(updates/L"readme.txt","a file beside the folders");
    need(u::cleanup_downloads(data,"0.2.0-preview.7")==1,"one folder removed");
    need(!fs::exists(old),"our older download removed entirely");
    need(read(foreign/L"Overglaze-Setup-0.2.0-preview.4-win64.exe")=="not ours","a folder without our marker is untouched");
    need(!fs::exists(shared/L"Overglaze-Setup-0.2.0-preview.3-win64.exe")&&read(shared/L"notes.txt")=="the user's"&&fs::exists(shared/L".overglaze-download.json"),
        "our file removed, someone else's kept, and the folder with it");
    need(read(keep/L"Overglaze-Setup-0.2.0-preview.7-win64.exe")=="new","the version being installed is kept");
    need(fs::exists(lying/L"Overglaze-Setup-0.2.0-preview.2-win64.exe"),"a marker naming another version is not ours");
    need(fs::exists(updates/L"readme.txt"),"files beside the folders are untouched");
    need(u::cleanup_downloads(data,{})==1&&!fs::exists(keep),"without a version to keep: every folder of ours");
    need(u::cleanup_downloads(base/L"no-data",{})==0,"no updates folder: nothing to do");
    // At start-up: the folders of versions already installed (or older) go, newer ones stay.
    const auto installed=updates/L"0.2.0-preview.6";marker(installed,"0.2.0-preview.6","Overglaze-Setup-0.2.0-preview.6-win64.exe");write(installed/L"Overglaze-Setup-0.2.0-preview.6-win64.exe","x");
    const auto older=updates/L"0.1.9";marker(older,"0.1.9","Overglaze-Setup-0.1.9-win64.exe");
    const auto newer=updates/L"0.2.0-preview.8";marker(newer,"0.2.0-preview.8","Overglaze-Setup-0.2.0-preview.8-win64.exe");write(newer/L"Overglaze-Setup-0.2.0-preview.8-win64.exe","y");
    need(u::cleanup_installed(data,v("0.2.0-preview.6"))==2&&!fs::exists(installed)&&!fs::exists(older),"installed and older downloads removed at start-up");
    need(read(newer/L"Overglaze-Setup-0.2.0-preview.8-win64.exe")=="y","a newer download is kept at start-up");
    need(read(foreign/L"Overglaze-Setup-0.2.0-preview.4-win64.exe")=="not ours","still nothing of anyone else's");
}

// ---- loopback HTTP server: canned responses, every request recorded.
struct Route {int status=200;std::string body,location;};
class Server {
    SOCKET listener_=INVALID_SOCKET;std::thread thread_;std::mutex lock_;std::map<std::string,Route> routes_;std::vector<std::string> requests_;
public:
    unsigned short port=0;
    Server(){WSADATA wsa{};need(WSAStartup(MAKEWORD(2,2),&wsa)==0,"winsock");
        listener_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);need(listener_!=INVALID_SOCKET,"socket");
        sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;
        need(bind(listener_,reinterpret_cast<sockaddr*>(&a),sizeof a)==0&&listen(listener_,16)==0,"bind loopback");
        int n=sizeof a;need(getsockname(listener_,reinterpret_cast<sockaddr*>(&a),&n)==0,"port");port=ntohs(a.sin_port);
        thread_=std::thread([this]{serve();});}
    ~Server(){closesocket(listener_);if(thread_.joinable())thread_.join();WSACleanup();}
    std::string url(const std::string& path)const{return "http://127.0.0.1:"+std::to_string(port)+path;}
    void route(const std::string& path,Route r){std::lock_guard g(lock_);routes_[path]=std::move(r);}
    std::vector<std::string> requests(){std::lock_guard g(lock_);return requests_;}
    std::size_t count(const std::string& path){std::size_t n=0;for(const auto& r:requests())if(r.rfind("GET "+path+" ",0)==0)++n;return n;}
private:
    void serve(){
        for(;;){SOCKET client=accept(listener_,nullptr,nullptr);if(client==INVALID_SOCKET)return;
            std::string request;char buffer[4096];
            while(request.find("\r\n\r\n")==std::string::npos&&request.size()<65536){const int n=recv(client,buffer,sizeof buffer,0);if(n<=0)break;request.append(buffer,n);}
            const auto space=request.find(' '),second=request.find(' ',space+1);
            const auto path=space==std::string::npos||second==std::string::npos?std::string():request.substr(space+1,second-space-1);
            Route r;{std::lock_guard g(lock_);requests_.push_back(request);const auto it=routes_.find(path);if(it!=routes_.end())r=it->second;else r.status=404;}
            std::string head;
            if(r.status==302)head="HTTP/1.1 302 Found\r\nLocation: "+r.location+"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            else head="HTTP/1.1 "+std::to_string(r.status)+(r.status==200?" OK":" Error")+"\r\nContent-Type: application/octet-stream\r\nContent-Length: "+std::to_string(r.body.size())+"\r\nConnection: close\r\n\r\n";
            const auto out=head+(r.status==302?std::string():r.body);
            for(std::size_t sent=0;sent<out.size();){const int n=send(client,out.data()+sent,int(std::min<std::size_t>(out.size()-sent,1<<16)),0);if(n<=0)break;sent+=std::size_t(n);}
            shutdown(client,SD_SEND);closesocket(client);}
    }
};

void end_to_end(const fs::path& base){
    Server server;const Key key,other;const std::string tag="v0.2.0-preview.7";
    std::string setup(3*1024*1024+17,'\0');for(std::size_t i=0;i<setup.size();++i)setup[i]=char((i*131+7)&0xFF);
    const auto manifest=manifest_json("0.2.0-preview.7",setup).dump(2);const auto signature=key.sign(manifest);
    const std::string prefix="/download/"+tag+"/";const auto setup_name="Overglaze-Setup-0.2.0-preview.7-win64.exe";
    auto asset=[&](const std::string& name,std::size_t size){return json{{"name",name},{"size",size},{"browser_download_url",server.url(prefix+name)}};};
    auto releases=[&](bool with_signature){json assets=json::array({asset(setup_name,setup.size()),asset("overglaze-update.json",manifest.size())});
        if(with_signature)assets.push_back(asset("overglaze-update.json.sig",signature.size()));
        return json::array({{{"tag_name","v9.9.9"},{"draft",true},{"html_url",server.url("/releases/tag/v9.9.9")},{"assets",assets}},
            {{"tag_name",tag},{"draft",false},{"prerelease",true},{"html_url",server.url("/releases/tag/"+tag)},{"assets",assets}},
            {{"tag_name","v0.2.0-preview.5"},{"draft",false},{"html_url",server.url("/releases/tag/v0.2.0-preview.5")},{"assets",json::array()}}}).dump();};
    server.route("/api/releases?per_page=20",{200,releases(true)});
    server.route(prefix+"overglaze-update.json",{200,manifest});
    server.route(prefix+"overglaze-update.json.sig",{200,signature});
    server.route(prefix+setup_name,{302,"",server.url("/objects/setup-blob")}); // GitHub redirects asset downloads
    server.route("/objects/setup-blob",{200,setup});
    u::Endpoint e;e.releases_api=server.url("/api/releases?per_page=20");e.asset_prefix=server.url("/download/");e.page_prefix=server.url("/releases/");
    e.releases_page=server.url("/releases");e.user_agent="Overglaze/0.2.0-preview.6";e.release_key=key.pub;e.loopback_test=true;
    const auto running=v("0.2.0-preview.6");

    auto offer=u::check(e,running);
    need(offer.state==u::Offer::State::available&&offer.version=="0.2.0-preview.7"&&offer.page==server.url("/releases/tag/"+tag),"check: signed update available");
    need(offer.manifest.notes_zh=="修复若干问题。\n第二行"&&offer.setup.url==server.url(prefix+setup_name),"check: verified notes and the Setup asset");
    {const auto first=server.requests().at(0);
     need(first.rfind("GET /api/releases?per_page=20 ",0)==0,"the release list is requested first");
     need(first.find("User-Agent: Overglaze/0.2.0-preview.6\r\n")!=std::string::npos&&first.find("Accept: application/vnd.github+json\r\n")!=std::string::npos,"User-Agent and Accept as documented");
     for(const auto& r:server.requests())need(r.find("Cookie:")==std::string::npos&&r.find("Authorization:")==std::string::npos&&r.find("Referer:")==std::string::npos,"no cookie, credentials or referer sent");}
    need(server.count("/objects/setup-blob")==0,"a check never downloads the Setup");

    const auto data=base/L"e2e-data";fs::create_directories(data);
    const auto stale=u::updates_dir(data)/L"0.2.0-preview.5";marker(stale,"0.2.0-preview.5","Overglaze-Setup-0.2.0-preview.5-win64.exe");write(stale/L"Overglaze-Setup-0.2.0-preview.5-win64.exe","old");
    u::Progress progress;
    const auto file=u::download_setup(e,offer,data,nullptr,&progress);
    need(file==u::updates_dir(data)/L"0.2.0-preview.7"/setup_name&&read(file)==setup,"download: the Setup through the redirect, into updates\\<version>");
    need(progress.received==setup.size()&&progress.total==setup.size(),"download progress");
    need(!fs::exists(fs::path(file.wstring()+L".partial"))&&fs::exists(file.parent_path()/L".overglaze-download.json"),"no partial file left; folder marked as ours");
    need(!fs::exists(stale),"older downloads of ours removed");
    u::verify_file(file,offer.manifest.setup);++checks;
    const auto downloads=server.count("/objects/setup-blob");
    need(u::download_setup(e,offer,data)==file&&server.count("/objects/setup-blob")==downloads,"a verified download is reused");

    // Tampered Setup on the server: same size, one byte changed.
    fs::remove(file);auto tampered=setup;tampered[tampered.size()/2]^=0x40;server.route("/objects/setup-blob",{200,tampered});
    refuses([&]{u::download_setup(e,offer,data);},"SHA-256","a tampered Setup is refused");
    need(!fs::exists(file)&&!fs::exists(fs::path(file.wstring()+L".partial")),"nothing of a refused download is kept");
    server.route("/objects/setup-blob",{200,setup+"extra"});
    refuses([&]{u::download_setup(e,offer,data);},"超出上限","a Setup larger than the manifest is refused");
    server.route("/objects/setup-blob",{200,setup.substr(1)});
    refuses([&]{u::download_setup(e,offer,data);},"不完整","a short Setup is refused");
    server.route("/objects/setup-blob",{200,setup});
    {u::Cancel cancel{true};refuses([&]{u::download_setup(e,offer,data,&cancel);},"已取消","a cancelled download stops");
     need(!fs::exists(fs::path(file.wstring()+L".partial")),"a cancelled download leaves nothing");}
    need(u::download_setup(e,offer,data)==file,"downloads again once the server is right");

    // The manifest is not trusted before its signature verifies.
    {auto changed=manifest;changed[changed.find("修复")]^=0x01;server.route(prefix+"overglaze-update.json",{200,changed});
     refuses([&]{u::check(e,running);},"签名校验失败","one changed manifest byte fails the check");
     server.route(prefix+"overglaze-update.json",{200,manifest});}
    {auto wrong=e;wrong.release_key=other.pub;refuses([&]{u::check(wrong,running);},"签名校验失败","a manifest signed by another key fails");}
    server.route(prefix+"overglaze-update.json.sig",{200,"not base64"});
    refuses([&]{u::check(e,running);},"签名文件格式无效","a malformed signature file fails");
    // A correctly signed manifest whose version is not the tag's.
    {const auto mismatched=manifest_json("0.2.0-preview.8",setup).dump(2);
     server.route(prefix+"overglaze-update.json",{200,mismatched});server.route(prefix+"overglaze-update.json.sig",{200,key.sign(mismatched)});
     refuses([&]{u::check(e,running);},"不一致","manifest version must equal the release tag");}
    // A signed manifest naming a Setup the release does not carry.
    {auto j=manifest_json("0.2.0-preview.7",setup);j["setup"]["name"]="Overglaze-Setup-other.exe";const auto renamed=j.dump(2);
     server.route(prefix+"overglaze-update.json",{200,renamed});server.route(prefix+"overglaze-update.json.sig",{200,key.sign(renamed)});
     refuses([&]{u::check(e,running);},"没有签名清单所列的安装程序","the manifest's Setup must be a release asset");}
    {auto j=manifest_json("0.2.0-preview.7",setup+"x");const auto resized=j.dump(2);
     server.route(prefix+"overglaze-update.json",{200,resized});server.route(prefix+"overglaze-update.json.sig",{200,key.sign(resized)});
     refuses([&]{u::check(e,running);},"大小","the release asset size must equal the manifest's");}
    server.route(prefix+"overglaze-update.json",{200,manifest});server.route(prefix+"overglaze-update.json.sig",{200,signature});
    // Without the signature asset: manual only, and the manifest is not even fetched.
    {server.route("/api/releases?per_page=20",{200,releases(false)});const auto before=server.count(prefix+"overglaze-update.json");
     const auto manual=u::check(e,running);
     need(manual.state==u::Offer::State::manual_only&&manual.version=="0.2.0-preview.7"&&server.count(prefix+"overglaze-update.json")==before,"manual only without the signature asset");
     refuses([&]{u::download_setup(e,manual,data);},"没有可安装的签名更新","a manual-only release is never downloaded");
     server.route("/api/releases?per_page=20",{200,releases(true)});}
    need(u::check(e,v("0.2.0-preview.7")).state==u::Offer::State::up_to_date,"running the newest: up to date");
    server.route("/api/releases?per_page=20",{403,"rate limited"});
    refuses([&]{u::check(e,running);},"HTTP 403","a refusal by GitHub is reported");
    server.route("/api/releases?per_page=20",{200,"<html>"});
    refuses([&]{u::check(e,running);},"无法解析","a release list that is not JSON is reported");
    // Plain HTTP is for this loopback test endpoint only.
    {auto production=e;production.loopback_test=false;refuses([&]{u::check(production,running);},"只允许 HTTPS","plain HTTP refused without the test hook");
     auto elsewhere=e;elsewhere.releases_api="http://localhost:"+std::to_string(server.port)+"/api/releases?per_page=20";
     refuses([&]{u::check(elsewhere,running);},"只允许 HTTPS","the test hook allows 127.0.0.1 only");}
    {auto official=u::official("0.2.0-preview.6");official.releases_api=server.url("/api/releases?per_page=20");
     refuses([&]{u::check(official,running);},"只允许 HTTPS","the official endpoint never speaks plain HTTP");}
}

// The offer cache: written by a check, shown at start without the network,
// re-verified each time, and installed through the same download and checks.
void offer_cache(const fs::path& base){
    Server server;const Key key,other;
    std::string setup(256*1024+3,'\0');for(std::size_t i=0;i<setup.size();++i)setup[i]=char((i*37+11)&0xFF);
    std::map<std::string,std::pair<std::string,std::string>> published; // version -> manifest, signature
    // One signed release on the loopback server; returns its release-list entry.
    auto publish=[&](const std::string& version){
        const auto tag="v"+version;const auto prefix="/download/"+tag+"/";const auto setup_name="Overglaze-Setup-"+version+"-win64.exe";
        const auto manifest=manifest_json(version,setup).dump(2);const auto signature=key.sign(manifest);published[version]={manifest,signature};
        server.route(prefix+"overglaze-update.json",{200,manifest});server.route(prefix+"overglaze-update.json.sig",{200,signature});server.route(prefix+setup_name,{200,setup});
        auto asset=[&](const std::string& name,std::size_t size){return json{{"name",name},{"size",size},{"browser_download_url",server.url(prefix+name)}};};
        return json{{"tag_name",tag},{"draft",false},{"html_url",server.url("/releases/tag/"+tag)},
            {"assets",json::array({asset(setup_name,setup.size()),asset("overglaze-update.json",manifest.size()),asset("overglaze-update.json.sig",signature.size())})}};};
    const std::string version="0.2.0-preview.9";const auto setup_path="/download/v"+version+"/Overglaze-Setup-"+version+"-win64.exe";
    server.route("/api/releases?per_page=20",{200,json::array({publish(version)}).dump()});
    u::Endpoint e;e.releases_api=server.url("/api/releases?per_page=20");e.asset_prefix=server.url("/download/");e.page_prefix=server.url("/releases/");
    e.releases_page=server.url("/releases");e.user_agent="Overglaze/0.2.0-preview.8";e.release_key=key.pub;e.loopback_test=true;
    const auto running=v("0.2.0-preview.8");
    const auto data=base/L"cache-data";fs::create_directories(data);const auto file=u::offer_cache_file(data);
    need(file==data/L"settings"/L"update-offer-v1.json","the cache sits with the update settings");
    {std::string why="x";need(!u::cached_offer(data,e,running,&why)&&why.empty(),"no cache: nothing shown, nothing logged");}

    // A check keeps what the cache needs, and the cache holds it byte for byte.
    const auto fresh=u::check(e,running);
    need(fresh.state==u::Offer::State::available&&!fresh.from_cache&&fresh.tag=="v"+version&&
        fresh.manifest_bytes==published[version].first&&fresh.signature==published[version].second,"a check carries the signed bytes and the tag");
    u::record_offer(data,fresh);
    need(fs::is_regular_file(file)&&fs::file_size(file)<=u::kOfferCacheLimit,"a signed offer is saved, bounded");
    {const auto j=json::parse(read(file));
     need(j["kind"]=="overglaze-update-offer"&&j["version"]==1&&j["tag"]=="v"+version&&j["page"]==fresh.page&&
         j["manifest"]==published[version].first&&j["signature"]==published[version].second&&
         j["setup"]["name"]==fresh.setup.name&&j["setup"]["url"]==server.url(setup_path)&&j["setup"]["size"]==setup.size(),"the exact manifest and signature, tag, page and Setup address");}
    {bool temp=false;for(const auto& f:fs::directory_iterator(file.parent_path()))if(f.path().filename().wstring().find(L".tmp-")!=std::wstring::npos)temp=true;
     need(!temp,"written atomically, no temporary file left");}

    // At start, without the network: the same offer, from the cache.
    const auto requests=server.requests().size();
    std::string why;const auto cached=u::cached_offer(data,e,running,&why);
    need(cached.has_value()&&why.empty()&&server.requests().size()==requests,"the cached offer is shown at start without a request");
    need(cached->state==u::Offer::State::available&&cached->from_cache&&cached->version==version&&cached->page==fresh.page&&cached->tag==fresh.tag&&
        cached->setup.url==fresh.setup.url&&cached->setup.size==fresh.setup.size&&cached->setup.name==fresh.setup.name&&
        cached->manifest.setup.sha256==fresh.manifest.setup.sha256&&cached->manifest.setup.size==fresh.manifest.setup.size&&cached->manifest.notes_zh==fresh.manifest.notes_zh,
        "the cached offer equals the checked one");
    need(fs::is_regular_file(file),"a valid cache stays");

    // 稍后 hides it for this session only; the next start shows it again.
    {u::OfferNotice n;need(!n.shown(),"an empty notice shows nothing");
     n.start(u::cached_offer(data,e,running));need(n.shown()&&n.shown()->from_cache&&n.shown()->version==version,"shown at start");
     n.later();need(!n.shown(),"稍后 hides it");
     n.checked(fresh,false);need(!n.shown(),"an automatic check finding the same version keeps it hidden");
     n.checked(fresh,true);need(n.shown()&&!n.shown()->from_cache,"a manual check shows it again, the fresh offer");}
    need(fs::is_regular_file(file),"稍后 never touches the cache");
    {u::OfferNotice n;n.start(u::cached_offer(data,e,running));need(n.shown()!=nullptr,"稍后, then a restart: shown again");
     n.later();auto newer=fresh;newer.version="0.2.0-preview.10";n.checked(newer,false);
     need(n.shown()&&n.shown()->version=="0.2.0-preview.10","an automatic check finding another version shows it");
     n.checked(u::Offer{},true);need(!n.shown(),"nothing newer: no notice");
     n.start(std::nullopt);need(!n.shown(),"no cache: no notice");}

    // 更新 from the cache: the same download and the same verification as a fresh offer.
    {const auto target=u::download_setup(e,*cached,data);
     need(read(target)==setup&&target==u::updates_dir(data)/lab::wide(version)/lab::wide(fresh.setup.name),"the cached offer downloads the Setup into updates\\<version>");
     u::verify_file(target,cached->manifest.setup);++checks;fs::remove(target);
     auto fetch_failed=[&](const char* contains,const char* what){bool typed=false;std::string text;
         try{u::download_setup(e,*cached,data);}catch(const u::FetchFailed& x){typed=true;text=x.what();}catch(const std::exception& x){text=x.what();}
         need(typed&&text.find(contains)!=std::string::npos,std::string(what)+": "+text);
         need(!fs::exists(target)&&!fs::exists(fs::path(target.wstring()+L".partial")),std::string(what)+": nothing kept");};
     auto tampered=setup;tampered[tampered.size()/3]^=0x20;server.route(setup_path,{200,tampered});
     fetch_failed("SHA-256","a tampered Setup is refused for a cached offer, as for a fresh one");
     server.route(setup_path,{200,setup.substr(7)});fetch_failed("不完整","a short Setup is refused for a cached offer");
     server.route(setup_path,{404,"gone"});fetch_failed("HTTP 404","a cached address that is gone asks for a fresh check");
     server.route(setup_path,{200,setup});
     {bool typed=false,local=false;try{u::download_setup(e,*cached,base/L"no-such-data");}catch(const u::FetchFailed&){typed=true;}catch(const u::Error&){local=true;}
      need(local&&!typed,"a local failure is not a failed address");}
     need(read(u::download_setup(e,*cached,data))==setup,"downloads once the address serves the signed Setup again");}

    // A tampered or broken cache is refused and removed.
    auto rejected=[&](const std::function<void(json&)>& change,const std::string& contains,const std::string& what,const u::Endpoint& endpoint){
        u::record_offer(data,fresh);auto j=json::parse(read(file));change(j);write(file,j.dump(2));
        std::string reason;need(!u::cached_offer(data,endpoint,running,&reason),what+": refused");
        need(reason.find(contains)!=std::string::npos,what+": "+reason);need(!fs::exists(file),what+": removed");};
    auto tamper=[&](const std::function<void(json&)>& change,const std::string& contains,const std::string& what){rejected(change,contains,what,e);};
    tamper([](json& j){auto m=j["manifest"].get<std::string>();const auto at=m.find("\"preview\"");m.replace(at,9,"\"stable\"");j["manifest"]=m;},"签名与发布密钥不匹配","a changed manifest byte");
    tamper([&](json& j){j["signature"]=other.sign(j["manifest"].get<std::string>());},"签名与发布密钥不匹配","a signature by another key");
    tamper([](json& j){j["signature"]="not base64";},"签名文件格式无效","a malformed signature");
    tamper([&](json& j){j["setup"]["url"]="https://example.com/Overglaze-Setup.exe";},"下载地址无效","a Setup address outside the releases");
    tamper([&](json& j){j["page"]="https://example.com/phish";},"发布页地址无效","a page outside the releases");
    tamper([](json& j){j["tag"]="v0.2.0-preview.10";},"不一致","a tag that is not the signed version");
    tamper([](json& j){j["setup"]["size"]=1;},"大小","a Setup size that is not the signed one");
    tamper([](json& j){j["setup"]["name"]="Overglaze-Setup-other.exe";},"没有签名清单所列的安装程序","a Setup name that is not the signed one");
    tamper([](json& j){j.erase("manifest");},"缺少字段 manifest","a missing manifest");
    tamper([](json& j){j["kind"]="something-else";},"缓存格式未知","another kind of file");
    {auto wrong=e;wrong.release_key=other.pub;rejected([](json&){},"签名与发布密钥不匹配","verified with the endpoint's key, not trusted as saved",wrong);}
    for(const auto& bytes:{std::string("{broken"),std::string("[]"),std::string(std::size_t(u::kOfferCacheLimit)+1,' ')}){
        write(file,bytes);std::string reason;need(!u::cached_offer(data,e,running,&reason)&&!reason.empty()&&!fs::exists(file),"a broken cache file is removed");}

    // A cache for the running version or an older one is removed.
    for(const char* now:{"0.2.0-preview.9","0.2.0-preview.10","0.2.0"}){
        u::record_offer(data,fresh);std::string reason;
        need(!u::cached_offer(data,e,v(now),&reason)&&reason.find("不比当前版本")!=std::string::npos&&!fs::exists(file),std::string("running ")+now+": cache removed: "+reason);}

    // A newer check result replaces the cache; nothing newer, or nothing signed, clears it.
    u::record_offer(data,fresh);
    server.route("/api/releases?per_page=20",{200,json::array({publish(version),publish("0.2.0-preview.10")}).dump()});
    u::record_offer(data,u::check(e,running));
    {const auto j=json::parse(read(file));need(j["tag"]=="v0.2.0-preview.10"&&j["manifest"]==published["0.2.0-preview.10"].first,"a newer check result replaces the cache");
     const auto again=u::cached_offer(data,e,running);need(again&&again->version=="0.2.0-preview.10","the replacement verifies at start");}
    {const auto up_to_date=u::check(e,v("0.2.0-preview.10"));need(up_to_date.state==u::Offer::State::up_to_date,"nothing newer");
     u::record_offer(data,up_to_date);need(!fs::exists(file),"a check finding nothing newer clears the cache");}
    {u::record_offer(data,fresh);auto unsigned_release=publish("0.2.0-preview.11");
     unsigned_release["assets"]=json::array({unsigned_release["assets"][0]});
     server.route("/api/releases?per_page=20",{200,json::array({publish(version),unsigned_release}).dump()});
     const auto manual=u::check(e,running);need(manual.state==u::Offer::State::manual_only,"newest is manual only");
     u::record_offer(data,manual);need(!fs::exists(file),"a manual-only result (nothing signed to keep) clears the cache");}
    u::record_offer({},fresh);need(!u::cached_offer({},e,running).has_value()&&!u::clear_offer({}),"without a data root: nothing kept, nothing read");
    need(!u::clear_offer(data),"nothing to clear");
}

int verify_mode(wchar_t** argv){
    const auto manifest=read(argv[2]),signature=read(argv[3]),blob=read(argv[4]);std::string why;
    const bool ok=u::verify(manifest,signature,std::vector<unsigned char>(blob.begin(),blob.end()),&why);
    std::cout<<(ok?"verified":"refused: "+why)<<'\n';return ok?0:1;}
fs::path temp_base(){std::wstring value(32768,L'\0');const auto n=GetEnvironmentVariableW(L"OVERGLAZE_TEST_TEMP",value.data(),DWORD(value.size()));
    value.resize(n<value.size()?n:0);return value.empty()?fs::temp_directory_path():fs::path(value);}
}
int wmain(int argc,wchar_t** argv){
    if(argc==5&&std::wstring(argv[1])==L"--verify")return verify_mode(argv);
    fs::path base;
    try{
        fs::path viewer;for(int i=1;i<argc;++i){const std::wstring a=argv[i];if(a==L"--viewer"&&i+1<argc)viewer=argv[++i];else throw std::runtime_error("unknown argument");}
        fs::create_directories(temp_base());base=temp_base()/lab::wide("overglaze-self-update-test-"+lab::uuid());need(fs::create_directory(base),"unique fixture directory");
        versions();signatures();manifests();files(base);selection();settings(base);installation(base);cleanup(base);end_to_end(base);offer_cache(base);
        if(!viewer.empty()){const auto version=u::product_version(viewer);need(version&&u::parse_version(*version),"the built viewer carries a SemVer ProductVersion");}
        need(base.filename().native().rfind(L"overglaze-self-update-test-",0)==0,"cleanup stays in the owned fixture");
        fs::remove_all(base);
        std::cout<<"PASS self-update: "<<checks<<" checks; network=loopback only; release key used for signing=no\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\nfixture="<<lab::utf8(base.wstring())<<'\n';return 1;}
}
