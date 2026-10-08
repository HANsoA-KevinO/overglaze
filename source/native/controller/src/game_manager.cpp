// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_game_manager.hpp"
#include "lab_game_presentation.hpp"
#include "lab_module_search.hpp"
#include "lab_package_identity.hpp"
#include "lab_windows_path.hpp"
#include "lab_identity.hpp"
#include "lab_model_versions.hpp"
#include <tlhelp32.h>
#include <fstream>
#include <memory>
#include <set>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <bcrypt.h>

namespace lab::games {
namespace fs=std::filesystem;
namespace {
// Does a recorded strategy put the thin dxgi.dll in the game directory?
bool proxied(const std::string& strategy){Loader l;l.strategy=strategy;return l.root_proxy();}
using lab::module_directories;
using lab::find_module;
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void need(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
std::string text(const fs::path& p){return utf8(p.wstring());}
std::string lower_ascii(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c=char(c+32);return s;}
bool plain_name(const std::string& s){if(s.empty()||s.size()>64)return false;if(!((s[0]>='a'&&s[0]<='z')||(s[0]>='0'&&s[0]<='9')))return false;
    for(char c:s)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.'||c=='_'))return false;return true;}
fs::path local(fs::path p){
    need(p.is_absolute()&&p==p.lexically_normal()&&p.native().size()<30000,"请选择规范的本地绝对路径（不含 ..）");
    const auto s=p.native();need(s.size()>3&&s[1]==L':'&&(s[2]==L'\\'||s[2]==L'/')&&s.find(L':',2)==std::wstring::npos&&s.find(L'\0')==std::wstring::npos,"不接受网络路径、设备路径或替代数据流");
    for(const auto& part:p.relative_path()){const auto n=part.native();need(!n.empty()&&n.back()!=L'.'&&n.back()!=L' ',"路径末尾不能含空格或句点");}
    winpath::require_no_reparse(p);return fs::canonical(p);
}
void directory(const fs::path& p){winpath::require_no_reparse(p.parent_path());if(!fs::exists(p)){std::error_code ec;
    const bool made=fs::create_directory(p,ec);need(made||(!ec&&fs::is_directory(p)),"无法创建管理目录");}
    winpath::require_no_reparse(p);need(fs::is_directory(p),"管理路径不是目录");}
struct Pins {
    std::vector<std::unique_ptr<Handle>> handles;
    void parents(fs::path p){for(;;){winpath::require_no_reparse(p);auto h=std::make_unique<Handle>(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));need(h->valid(),"目录被占用或权限不足");handles.push_back(std::move(h));auto next=p.parent_path();if(next==p)break;p=next;}}
    HANDLE file(const fs::path& p,bool erase=false){winpath::require_no_reparse(p);auto h=std::make_unique<Handle>(CreateFileW(p.c_str(),GENERIC_READ|(erase?DELETE:0),FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));need(h->valid(),"文件正在使用、被修改或权限不足；请先退出游戏");
        BY_HANDLE_FILE_INFORMATION info{};need(GetFileInformationByHandle(h->value,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&info.nNumberOfLinks==1,"拒绝链接或非普通文件");auto raw=h->value;handles.push_back(std::move(h));return raw;}
};
std::string handle_digest(HANDLE f){LARGE_INTEGER size{},zero{};need(GetFileSizeEx(f,&size)&&size.QuadPart<=2LL*1024*1024*1024,"文件超过 2 GiB 检查上限");need(SetFilePointerEx(f,zero,nullptr,FILE_BEGIN),"无法读取文件");
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    struct Cleanup{BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Cleanup(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}} cleanup{alg,hash};
    need(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0&&BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0,"SHA-256 初始化失败");
    std::vector<unsigned char> b(1<<20);DWORD n=0;for(;;){need(ReadFile(f,b.data(),DWORD(b.size()),&n,nullptr),"读取文件失败");if(!n)break;need(BCryptHashData(hash,b.data(),n,0)>=0,"SHA-256 读取失败");}
    std::array<unsigned char,32> result{};need(BCryptFinishHash(hash,result.data(),DWORD(result.size()),0)>=0,"SHA-256 失败");std::string out;for(auto v:result){out+="0123456789abcdef"[v>>4];out+="0123456789abcdef"[v&15];}return out;}
std::string digest(const fs::path& p){Pins pin;return handle_digest(pin.file(p));}
json read(const fs::path& p){Pins pin;pin.file(p);need(fs::file_size(p)<=128*1024,"管理记录超过大小限制");std::ifstream in(p,std::ios::binary);return json::parse(in);}
void save(const fs::path& p,const json& doc){Pins pin;pin.parents(p.parent_path());if(fs::exists(p))winpath::require_no_reparse(p);const auto bytes=doc.dump(2)+"\n";need(bytes.size()<=128*1024,"管理记录超过大小限制");const auto tmp=p.parent_path()/wide(".manager-"+uuid()+".tmp");
    Handle out(CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));need(out.valid(),"无法保存管理记录");DWORD wrote=0;const bool ok=WriteFile(out.value,bytes.data(),DWORD(bytes.size()),&wrote,nullptr)&&wrote==bytes.size()&&FlushFileBuffers(out.value);CloseHandle(out.value);out.value=INVALID_HANDLE_VALUE;
    if(!ok||!MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(tmp.c_str());throw std::runtime_error("管理记录未能原子保存；未继续操作");}}
struct Writer {Handle h;Pins pins;explicit Writer(const fs::path& store):h(){directory(store);pins.parents(store);const auto path=store/L"writer.lock";if(fs::exists(path))winpath::require_no_reparse(path);h.value=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);need(h.valid(),"另一控制台正在管理安装，请稍后重试");}};
bool id_ok(const std::string& s){return s.size()==38&&s.front()=='{'&&s.back()=='}'&&s.find_first_not_of("{}-0123456789ABCDEFabcdef")==std::string::npos;}
Entry entry(const json& j){Entry e{j.at("id"),j.at("title"),fs::path(wide(j.at("exe").get<std::string>()))};need(id_ok(e.id)&&e.title.size()<=256&&e.exe.is_absolute(),"无效游戏登记项");return e;}
json registry(const fs::path& store){const auto f=store/L"games.json";if(!fs::exists(f))return {{"schema","overglaze-game-library-v1"},{"entries",json::array()}};auto j=read(f);
    // A library written before the rename is read once under its old
    // schema and saved under the new one by the next write; entries are unchanged.
    if(j.is_object()&&j.value("schema","")=="dlsslab-game-library-v1")j["schema"]="overglaze-game-library-v1";
    need(j.at("schema")=="overglaze-game-library-v1"&&j.at("entries").is_array()&&j.at("entries").size()<=128,"游戏清单损坏；不自动覆盖");std::set<std::string> ids;for(auto& v:j.at("entries"))need(ids.insert(entry(v).id).second,"重复登记 ID");return j;}
bool running(const fs::path& exe){Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));need(snapshot.valid(),"无法检查游戏进程，暂不允许修改");PROCESSENTRY32W p{sizeof(p)};need(Process32FirstW(snapshot.value,&p)!=FALSE,"无法枚举游戏进程");
    do{if(!winpath::same_spelling(p.szExeFile,exe.filename()))continue;Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,p.th32ProcessID));if(!process.valid())return true;std::wstring path(32768,0);DWORD n=DWORD(path.size());if(!QueryFullProcessImageNameW(process.value,0,path.data(),&n))return true;path.resize(n);if(winpath::same_spelling(path,exe))return true;}while(Process32NextW(snapshot.value,&p));need(GetLastError()==ERROR_NO_MORE_FILES,"游戏进程检查不完整");return false;}
// The files this product places in a game under its current names (lab_identity.hpp).
bool current_name(const std::string& n){return n=="dxgi.dll"||n=="overglaze_controller.dll"||n=="overglaze_reframework.dll"||
    n=="overglaze.install.json"||n=="overglaze_nvngx.dll"||n=="nvngx_dlssnr.dll";}
// The names of a pre-rename (DLSS Lab) installation. Recognised, verified and
// removed by hash; never written.
bool legacy_name(const std::string& n){return n=="dlsslab_controller.dll"||n=="dlsslab_reframework.dll"||n=="dlsslab.install.json"||n=="dlsslab_nvngx.dll";}
// Anything the product ever placed: what a transaction, a staging directory or a
// recovery copy may name.
bool owned_name(const std::string& n){return current_name(n)||legacy_name(n);}
// A transaction written by the pre-rename manager, or one that names its files.
bool legacy_transaction(const json& j){
    if(j.value("schema","")=="dlsslab-install-transaction-v1"||j.value("loader_subdirectory","")=="dlsslab")return true;
    if(j.contains("files")&&j.at("files").is_object())for(auto it=j.at("files").begin();it!=j.at("files").end();++it)if(legacy_name(it.key()))return true;
    return false;}
// The order a removal goes in: the proxy first (once it is gone the game loads
// nothing of ours), then the loader, then everything else the record names --
// config, bridge, model -- under whichever names it was installed.
std::vector<std::string> removal_order(const json& files,const std::string& proxy,const std::string& loader){
    std::vector<std::string> out;auto add=[&](const std::string& n){if(!n.empty()&&files.contains(n)&&std::find(out.begin(),out.end(),n)==out.end())out.push_back(n);};
    add(proxy);add(loader);
    for(const auto* n:{"overglaze.install.json","dlsslab.install.json","overglaze_nvngx.dll","dlsslab_nvngx.dll","nvngx_dlssnr.dll"})add(n);
    for(auto it=files.begin();it!=files.end();++it)add(it.key()); // anything else it names, last
    return out;}
// Where a package's own files live inside a game. Empty subdirectory means the
// game directory itself (the original root layout).
fs::path lab_directory(const fs::path& game_directory,const Policy& p){
    return p.loader.subdir.empty()?game_directory:(game_directory/p.loader.subdir).lexically_normal();
}
bool hash_ok(const std::string& s){return s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos;}
json receipt(const fs::path& file,const Entry& e){auto j=read(file);need((j.at("schema")=="overglaze-install-transaction-v1"||j.at("schema")=="dlsslab-install-transaction-v1")&&j.at("id")==e.id&&winpath::same_spelling(fs::path(wide(j.at("exe").get<std::string>())),e.exe),"安装记录与目标路径不匹配");
    // Five, not four: the root-proxy strategy records the thin dxgi.dll in the
    // game directory in addition to the host, bridge, model and config.
    need(j.at("files").is_object()&&j.at("files").size()<=5,"无效安装文件清单");for(auto it=j.at("files").begin();it!=j.at("files").end();++it)need(owned_name(it.key())&&hash_ok(it.value().get<std::string>()),"安装清单包含非 Lab 文件");return j;}
// The verdicts a package can be made for. Written once: the gate that reached
// make-package but not refresh-package has happened three times, and a new
// route is exactly when a second copy would drift. Anti-tamper and anti-cheat
// never reach the verdict (they are risks), so every DLSS route admits.
bool admitting_verdict(const std::string& v){return v=="sl-rr-ready"||v=="sl-sr-ready"||v=="ngx-rr-ready"||v=="ngx-sr-ready";}
// What an anti-cheat file's name points to. A product is named only when its
// own files carry that name; a file matched by the generic "anticheat" marker
// is shown by its file name, with no product claimed for it.
std::string anticheat_product(const std::string& marker){
    const auto file=fs::path(wide(marker)).filename();const auto name=lower_ascii(text(file));
    auto has=[&](const char* part){return name.find(part)!=std::string::npos;};
    if(has("easyanticheat"))return "Easy Anti-Cheat";
    if(has("battleye")||has("beservice")||has("beclient"))return "BattlEye";
    if(has("equ8"))return "EQU8";
    if(has("vgk.sys")||has("vanguard"))return "Vanguard";
    if(has("anticheatexpert"))return "Anti-Cheat Expert";
    const auto shown=text(file);return shown.size()<=64?shown:std::string("anticheat");}
std::vector<std::string> anticheat_products(const std::vector<std::string>& markers){std::vector<std::string> out;
    for(const auto& m:markers){const auto n=anticheat_product(m);if(std::find(out.begin(),out.end(),n)==out.end()&&out.size()<8)out.push_back(n);}
    return out;}
std::vector<std::string> risk_list(bool anti_tamper,bool anticheat){std::vector<std::string> out;
    if(anti_tamper)out.push_back("anti-tamper");if(anticheat)out.push_back("anticheat");return out;}
// What a package records in its notes when the checks found a risk.
std::string risk_note(const Preflight& pre){std::string found;
    if(pre.denuvo_suspected)found="Denuvo 反篡改";
    if(!pre.anticheat.empty()){if(!found.empty())found+="、";found+="反作弊相关文件（";
        for(std::size_t i=0;i<pre.anticheat.size();++i){if(i)found+="、";found+=pre.anticheat[i];}found+="）";}
    return "检测到风险："+found+"。安装、更新和重新适配时由用户确认风险；Overglaze 不隐藏自己，"
        "不绕过、修补、欺骗、调试或转储任何保护，也不因此放宽任何身份或状态检查。";}
// The scope a config states: V1-V3 the user's offline / no-anti-cheat
// declaration, V4 the user's risk acknowledgement with the facts the checks found.
bool scope_declared(const json& c){
    if(c.value("version",0)==4)return c.at("risk").is_object()&&c.at("risk").at("acknowledged")==true;
    return c.at("offline_single_player")==true&&c.at("no_anticheat")==true;}
std::vector<std::string> conflicts(const fs::path& dir){std::vector<std::string> found;for(const auto* n:{"d3d12.dll","d3d11.dll","version.dll","winmm.dll","dinput8.dll","ReShade.ini","renodx-dlss5.addon64","overglaze.addon64","overglaze_preview.addon64","dlsslab.addon64","dlsslab_preview.addon64"})if(fs::exists(dir/wide(n)))found.push_back(n);return found;}
void copy_handle(HANDLE from,const fs::path& to,const std::string& hash){need(!fs::exists(to),"目标已存在，拒绝覆盖");Pins pin;pin.parents(to.parent_path());need(handle_digest(from)==hash,"源文件身份改变");
    Handle out(CreateFileW(to.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));need(out.valid(),"复制失败：目标占用或权限不足");
    try{LARGE_INTEGER zero{};need(SetFilePointerEx(from,zero,nullptr,FILE_BEGIN),"复制源无法读取");std::vector<unsigned char> buffer(1<<20);DWORD n=0,wrote=0;
        for(;;){need(ReadFile(from,buffer.data(),DWORD(buffer.size()),&n,nullptr),"复制读取失败");if(!n)break;need(WriteFile(out.value,buffer.data(),n,&wrote,nullptr)&&wrote==n,"复制写入失败");}
        need(FlushFileBuffers(out.value)&&handle_digest(out.value)==hash,"复制后 SHA-256 不一致");
    }catch(...){FILE_DISPOSITION_INFO d{TRUE};SetFileInformationByHandle(out.value,FileDispositionInfo,&d,sizeof(d));throw;}}
void copy_new(const fs::path& from,const fs::path& to,const std::string& hash){Pins pin;copy_handle(pin.file(from),to,hash);}
void erase_handle(HANDLE h){FILE_DISPOSITION_INFO d{TRUE};need(SetFileInformationByHandle(h,FileDispositionInfo,&d,sizeof(d))!=FALSE,"文件仍在使用；安装记录保留，可在退出后重试卸载");}
std::string now_utc(){SYSTEMTIME t{};GetSystemTime(&t);char b[32];std::snprintf(b,sizeof(b),"%04u-%02u-%02uT%02u:%02u:%02uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);return b;}

// ---------------------------------------------------------------- coded refusals
// A refusal names itself with a code from the table (lab_game_reasons.hpp) and
// its parameters. what() keeps the message the refusal always had, so every
// caller that shows e.what() is unchanged; the status carries the code.
struct Refusal : std::runtime_error {std::string code;json params;
    Refusal(std::string c,const std::string& message,json p=json::object()):std::runtime_error(message),code(std::move(c)),params(std::move(p)){}};
// Runs f; an exception that is not already a Refusal becomes one under `code`,
// with the original message kept as the "message" parameter.
template<class F> auto within(const char* code,F&& f)->decltype(f()){
    try{return f();}catch(const Refusal&){throw;}catch(const std::exception& e){throw Refusal(code,e.what(),json{{"message",e.what()}});}}
void record(Status& s,const char* name,Outcome o,json value=nullptr,std::string reason={}){s.checks.push_back({name,o,std::move(value),std::move(reason)});}
// A check that refuses: recorded either way, and a failure throws the Refusal.
void gate(Status& s,const char* name,bool ok,const char* code,const std::string& message,json params=json::object(),json value=nullptr){
    if(ok){record(s,name,Outcome::pass,std::move(value));return;}
    record(s,name,Outcome::fail,std::move(value),code);throw Refusal(code,message,std::move(params));}
// How the player starts a game installed with this strategy (the success
// message and the "how to use" text follow the load mode). A root-layout
// install (the original layout) has no subdirectory. A late-loading Steam game
// is started through the launch option; any other store through watch.
Reason launch_reason(const std::string& strategy,const std::string& subdir,const fs::path& root,const std::string& store){
    if(subdir.empty())return {"launch-root-layout"};
    if(strategy=="root_proxy_on_insert")return {"launch-root-proxy-on-insert",{{"subdir",subdir}}};
    if(proxied(strategy))return {"launch-root-proxy",{{"subdir",subdir}}};
    if(store=="steam")return {"launch-late",{{"subdir",subdir},{"option",steam_launch_option(root)}}};
    return {"launch-late-watch",{{"subdir",subdir},{"command",watch_command(root)}}};}

// ---------------------------------------------------------------- progress
// Each list is the transaction's real steps, in the order the code runs them.
const std::vector<std::string> kInstallStages{"check","pin-game","space","receipts","stage-files","record-transaction","activate","verify","commit","cleanup-staging"};
const std::vector<std::string> kUninstallStages{"check","pin-files","space","recovery-copy","record-transaction","remove-files","commit","retention"};
const std::vector<std::string> kUpdateStages{"check","refresh-check","uninstall","refresh-package","install"};
const std::vector<std::string> kRepinStages{"check","preflight","uninstall","retire-package","make-package","install"};
const std::vector<std::string> kNoStages{};
// Reports one operation's stages. A callback that throws is ignored: progress
// is a report and must never change what the transaction does.
class Steps {
public:
    Steps(const Progress& cb,std::string operation,std::string parent):cb_(cb),op_(std::move(operation)),parent_(std::move(parent)),all_(operation_stages(op_)){}
    void enter(const char* stage){close();current_=stage;emit(current_,"start");}
    void skip(const char* stage,json params=json::object()){close();emit(stage,"skipped",std::move(params));}
    void done(json params=json::object()){if(!current_.empty()){emit(current_,"done",std::move(params));current_.clear();}}
    void fail(const std::string& message){if(!current_.empty()){emit(current_,"failed",json{{"message",message}});failed_=current_;current_.clear();}}
    // The stage running now, or the one that failed.
    std::string where()const{return current_.empty()?failed_:current_;}
private:
    void close(){done();}
    void emit(const std::string& stage,const char* status,json params=json::object()){if(!cb_)return;
        ProgressEvent ev;ev.operation=op_;ev.stage=stage;ev.status=status;ev.parent=parent_;ev.count=unsigned(all_.size());ev.params=std::move(params);
        for(std::size_t i=0;i<all_.size();++i)if(all_[i]==stage)ev.index=unsigned(i+1);
        try{cb_(ev);}catch(...){}}
    const Progress& cb_;std::string op_,parent_,current_,failed_;const std::vector<std::string>& all_;
};

// ---------------------------------------------------------------- second receipt
// The second install receipt, in the Manage-LabGame.ps1 schema, lives beside
// the manager's own transaction. The research tools' install script
// (Manage-LabGame.ps1) keeps its own per-package install receipt inside the
// program root (legacy_receipt_file below); the manager only READS it, so the
// cross-tool guard (no install over a receipt that is not "uninstalled") holds
// in both places. The one write that remains there is transitional: a receipt
// this manager or the script left saying "installed" for this very game
// directory is set to "uninstalled" when the game is uninstalled, so it never
// lies and never blocks the next install. The manager never creates that
// directory.
fs::path own_receipt_file(const fs::path& box){return box/L"install-receipt.json";}
fs::path legacy_receipt_file(const fs::path& root,const std::string& package){return root/L"docs"/L"maintenance"/wide(package+"-install.json");}
void mark_receipt_uninstalled(const fs::path& file,const fs::path& game_directory,const fs::path& recovery){
    if(!fs::exists(file))return;
    try{auto r=read(file);if(r.value("state","")=="installed"&&winpath::same_spelling(fs::path(wide(r.value("game_directory",""))),game_directory)){
        r["state"]="uninstalled";r["uninstalled_by"]="overglaze-games native controller";r["uninstalled_at"]=now_utc();r["recovery"]=text(recovery);save(file,r);}}catch(...){}}

// ---------------------------------------------------------------- storage retention
// The two kinds of directory the manager makes in a game's box: package-{GUID}
// (an install's staging copy) and uninstall-{GUID} (a recovery copy).
bool made_here(const fs::path& box,const fs::path& dir,const std::wstring& prefix){
    const auto name=dir.filename().wstring();
    return winpath::same_spelling(dir.parent_path(),box)&&name.size()==prefix.size()+38&&name.compare(0,prefix.size(),prefix)==0&&id_ok(utf8(name.substr(prefix.size())));}
// Deletes one directory the manager made: only ordinary single-link files with a
// name Lab owns and, when `expected` lists them, the recorded hash; then the empty
// directory. Every entry is checked (and held by handle) before anything is
// deleted: a foreign file, a subdirectory, a link or changed bytes and the whole
// directory stays. Never recursive.
void remove_made_directory(const fs::path& box,const fs::path& dir,const std::wstring& prefix,const json& expected){
    need(made_here(box,dir,prefix),"保留规则只删除管理器自己建立的目录");winpath::require_no_reparse(dir);
    Pins pins;std::vector<HANDLE> files;
    for(const auto& item:fs::directory_iterator(dir)){const auto name=text(item.path().filename());
        need(owned_name(name),"目录里有不属于 Lab 的文件，整个保留: "+name);
        need(!expected.is_object()||expected.contains(name),"目录里有记录之外的文件，整个保留: "+name);
        const auto h=pins.file(item.path(),true);
        if(expected.is_object())need(handle_digest(h)==expected.at(name).get<std::string>(),"副本内容已改变，整个保留: "+name);
        files.push_back(h);}
    for(const auto h:files)erase_handle(h);
    pins.handles.clear();
    // A scanner may still hold a deleted file open for a moment, which keeps
    // the directory non-empty until it lets go.
    for(unsigned attempt=0;;++attempt){if(RemoveDirectoryW(dir.c_str()))return;
        need(attempt<20&&GetLastError()==ERROR_DIR_NOT_EMPTY,"目录未能删除（可能仍被占用）");Sleep(50);}}
// The ledger of recovery copies made under the rule, oldest first. Anything not
// in it predates the rule and is kept until the owner decides what to do with it.
constexpr const char* kLedgerSchema="overglaze-manager-retention-v1";
fs::path ledger_file(const fs::path& box){return box/L"retention.json";}
json load_ledger(const fs::path& box){const auto f=ledger_file(box);
    if(!fs::exists(f))return {{"schema",kLedgerSchema},{"keep_recovery_copies",kRecoveryCopiesKept},{"recovery_copies",json::array()},{"pruned",0}};
    auto j=read(f);if(j.is_object()&&j.value("schema","")=="dlsslab-manager-retention-v1")j["schema"]=kLedgerSchema; // pre-rename ledger
    need(j.value("schema","")==kLedgerSchema&&j.contains("recovery_copies")&&j.at("recovery_copies").is_array()&&j.at("recovery_copies").size()<=64,"保留记录损坏；不按它删除任何副本");return j;}
// Adds a successful uninstall's recovery copy and deletes ledger copies beyond
// the newest kRecoveryCopiesKept. Never throws: the uninstall has already
// succeeded, and a copy that cannot be removed stays and is retried next time.
void retain_recovery(const fs::path& box,const fs::path& recovery,const json& copied){
    json ledger;try{ledger=load_ledger(box);}catch(...){return;}
    auto& copies=ledger["recovery_copies"];copies.push_back({{"directory",text(recovery.filename())},{"created_at",now_utc()},{"files",copied}});
    ledger.erase("last_error");
    while(copies.size()>kRecoveryCopiesKept){const auto oldest=copies.front();
        try{const auto dir=box/wide(oldest.at("directory").get<std::string>());
            std::error_code ec;if(fs::exists(dir,ec))remove_made_directory(box,dir,L"uninstall-",oldest.value("files",json()));
            copies.erase(copies.begin());ledger["pruned"]=ledger.value("pruned",0)+1;}
        catch(const std::exception& e){ledger["last_error"]=e.what();break;}}
    try{save(ledger_file(box),ledger);}catch(...){}}
std::uint64_t file_bytes(HANDLE h){LARGE_INTEGER size{};need(GetFileSizeEx(h,&size)!=FALSE,"无法读取文件大小");return static_cast<std::uint64_t>(size.QuadPart);}
// ---------------------------------------------------------------- the model is kept once
// The NR model (~158 MiB) is the same file for every game. If every staging
// directory and every recovery copy carried its own copy, a few updates per
// game would add up to gigabytes. So an install copies it straight from
// app\models (hash re-checked by handle) without staging it, and a recovery copy
// records its hash and keeps ONE shared copy per distinct model here.
constexpr const char* kModelName="nvngx_dlssnr.dll";
bool hex_sha256(const std::string& h){if(h.size()!=64)return false;for(const char c:h)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;}
fs::path shared_models(const fs::path& store){return store/L"_models";}
fs::path shared_model_file(const fs::path& store,const std::string& sha){need(hex_sha256(sha),"模型身份格式不对");return shared_models(store)/wide(sha)/L"nvngx_dlssnr.dll";}
// Makes sure the shared copy for `sha` exists and is intact; copies it from the
// open, identity-checked handle only when it is missing. Never overwrites.
void keep_shared_model(const fs::path& store,HANDLE from,const std::string& sha){
    const auto file=shared_model_file(store,sha);
    if(fs::exists(file)){need(digest(file)==sha,"共享模型副本已改变；不覆盖，先检查");return;}
    directory(shared_models(store));directory(file.parent_path());copy_handle(from,file,sha);}

// ---------------------------------------------------------------- preflight helpers
struct PeInfo{bool valid=false,x64=false;std::vector<std::string> sections;std::string error;};
PeInfo pe_info(HANDLE h){PeInfo r;std::vector<unsigned char> b(16384);DWORD n=0;LARGE_INTEGER zero{};
    if(!SetFilePointerEx(h,zero,nullptr,FILE_BEGIN)||!ReadFile(h,b.data(),DWORD(b.size()),&n,nullptr)){r.error="read-failed";return r;}
    auto u16=[&](size_t o){return o+2<=n?unsigned(b[o]|(b[o+1]<<8)):0xFFFFu;};auto u32=[&](size_t o){return o+4<=n?unsigned(b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(unsigned(b[o+3])<<24)):0xFFFFFFFFu;};
    if(n<64||b[0]!='M'||b[1]!='Z'){r.error="not-mz";return r;}const auto pe=u32(60);if(pe==0xFFFFFFFFu||size_t(pe)+24>n||b[pe]!='P'||b[pe+1]!='E'||b[pe+2]||b[pe+3]){r.error="no-pe-signature";return r;}
    const auto machine=u16(pe+4),count=u16(pe+6),opt=u16(pe+20),magic=u16(pe+24);
    r.x64=machine==0x8664&&magic==0x20b;if(count==0||count>96){r.error="section-count";return r;}
    const size_t table=size_t(pe)+24+opt;if(table+size_t(count)*40>n){r.error="section-table-beyond-16k";return r;}
    for(unsigned i=0;i<count;++i){std::string name(reinterpret_cast<const char*>(&b[table+size_t(i)*40]),8);name.erase(std::find(name.begin(),name.end(),'\0'),name.end());r.sections.push_back(name);}
    r.valid=true;return r;}
std::string store_of(const fs::path& dir){
    auto has=[](const fs::path& d,const wchar_t* n){std::error_code ec;return fs::exists(d/n,ec);};
    for(const auto& d:{dir,dir.parent_path()}){if(d.empty())continue;
        if(has(d,L".egstore"))return "epic";if(has(d,L"MicrosoftGame.config"))return "gdk";if(has(d,L"steam_api64.dll")||has(d,L"steam_api.dll"))return "steam";}
    return lower_ascii(text(dir)).find("\\steamapps\\")!=std::string::npos?"steam":"unknown";}
// The marker scan's bounds, named so the "cannot check" report can state them.
constexpr unsigned kMarkerScanEntries=4096;constexpr unsigned kMarkerScanSeconds=5;
void scan_markers(const fs::path& dir,std::vector<std::string>& out,bool& complete,unsigned& scanned){
    static constexpr const char* markers[]{"easyanticheat","battleye","beservice","beclient","equ8","anticheat","vgk.sys","vanguard"};
    const auto start=GetTickCount64();unsigned count=0;std::error_code ec;
    for(fs::recursive_directory_iterator it(dir,fs::directory_options::skip_permission_denied,ec),end;it!=end;it.increment(ec)){if(ec){complete=false;break;}
        if(++count>kMarkerScanEntries||GetTickCount64()-start>kMarkerScanSeconds*1000ULL){complete=false;break;}if(it.depth()>=3)it.disable_recursion_pending();
        const auto name=lower_ascii(text(it->path().filename()));for(auto m:markers)if(name.find(m)!=std::string::npos){out.push_back(text(fs::relative(it->path(),dir,ec)));break;}}
    if(ec)complete=false;
    scanned=std::min(count,kMarkerScanEntries);}
std::string derive_package_name(const fs::path& exe){std::string s=lower_ascii(text(exe.stem())),out;for(char c:s){if((c>='a'&&c<='z')||(c>='0'&&c<='9'))out+=c;else if(!out.empty()&&out.back()!='-')out+='-';}
    while(!out.empty()&&out.back()=='-')out.pop_back();if(out.empty())out="game";if(out.size()>48)out.resize(48);if(!((out[0]>='a'&&out[0]<='z')||(out[0]>='0'&&out[0]<='9')))out="g"+out;return out;}
std::string package_from_settings(std::string_view settings){std::string s(settings);const std::string pre="overlay-",post=".json";
    if(s.size()>pre.size()+post.size()&&s.compare(0,pre.size(),pre)==0&&s.compare(s.size()-post.size(),post.size(),post)==0)return s.substr(pre.size(),s.size()-pre.size()-post.size());return {};}
json facts_json(const profiles::Facts& f){json modules=json::object();for(const auto& [k,v]:f.modules)modules[k]=v;
    return {{"executable",f.executable},{"title",f.title},{"route",f.route},{"viewport",f.viewport},{"linear_depth",f.linear_depth},
        {"native_evaluate_host_rebind",f.native_evaluate_host_rebind},{"binding_preservation",f.binding_preservation},
        {"default_exposure_stops",f.default_exposure_stops},{"settings_file",f.settings_file},{"capture_origin",f.capture_origin},{"modules",modules}};}
profiles::Facts facts_from_json(const json& f,const std::string& id,const std::string& package,const std::string& exe_hash){
    need(f.is_object()&&f.contains("executable")&&f.contains("modules"),"适配包 facts 不完整");profiles::Facts x;x.id=id;x.package=package;x.executable_sha256=exe_hash;
    x.executable=f.at("executable");x.title=f.value("title","");x.route=f.value("route","sl-rr");x.viewport=f.value("viewport",0u);x.linear_depth=f.value("linear_depth",false);
    x.native_evaluate_host_rebind=f.value("native_evaluate_host_rebind",false);x.binding_preservation=f.value("binding_preservation",false);
    x.default_exposure_stops=f.value("default_exposure_stops",0.f);x.settings_file=f.value("settings_file","overlay-"+package+".json");
    x.capture_origin=f.value("capture_origin",package+"-controlled-rr-stage");for(auto it=f.at("modules").begin();it!=f.at("modules").end();++it)x.modules[it.key()]=it.value().get<std::string>();return x;}

// A folder that holds a DLSS route's modules: the game's own EXE sits next to them.
bool holds_dlss(const fs::path& dir){std::error_code ec;
    for(const auto* name:{L"sl.interposer.dll",L"nvngx_dlssd.dll",L"nvngx_dlss.dll"})if(fs::is_regular_file(dir/name,ec))return true;
    return false;}
bool ends_with(const std::string& s,std::string_view tail){return s.size()>=tail.size()&&s.compare(s.size()-tail.size(),tail.size(),tail)==0;}
// Unreal Engine games carry a small bootstrap EXE at their root that starts
// <Project>\Binaries\Win64 (or WinGDK)\<Project>-Win64-Shipping.exe. The
// bootstrap has no DLSS beside it; the shipping EXE is the game. Only the
// root's direct subfolders are looked at, and only when an Engine folder marks
// the layout.
std::vector<fs::path> unreal_games(const fs::path& root){std::vector<fs::path> out;std::error_code ec;
    if(!fs::is_directory(root/L"Engine",ec))return out;unsigned seen=0;
    for(fs::directory_iterator it(root,fs::directory_options::skip_permission_denied,ec),end;it!=end&&!ec&&++seen<=64;it.increment(ec)){
        if(!it->is_directory(ec))continue;
        for(const auto* platform:{L"Win64",L"WinGDK"}){const auto bin=it->path()/L"Binaries"/platform;std::error_code bec;if(!fs::is_directory(bin,bec))continue;
            unsigned files=0;for(fs::directory_iterator f(bin,fs::directory_options::skip_permission_denied,bec),fe;f!=fe&&!bec&&++files<=256;f.increment(bec))
                if(f->is_regular_file(bec)&&ends_with(lower_ascii(text(f->path().filename())),"-shipping.exe"))out.push_back(f->path());}}
    std::stable_sort(out.begin(),out.end(),[](const fs::path& a,const fs::path& b){return holds_dlss(a.parent_path())>holds_dlss(b.parent_path());});
    return out;}
// Most likely game EXE first: one beside DLSS modules, then an Unreal shipping
// build, then anything else, then installers, crash reporters and helpers.
int candidate_rank(const fs::path& exe){
    if(holds_dlss(exe.parent_path()))return 0;const auto name=lower_ascii(text(exe.filename()));if(ends_with(name,"-shipping.exe"))return 1;
    for(const auto* helper:{"crashreport","cefsubprocess","redist","dxsetup","unins","setup"})if(name.find(helper)!=std::string::npos)return 3;
    return 2;}
// Folders of game data, never of the game's EXE. Skipping them keeps a large
// game (thousands of packed asset files) inside the scan bound.
bool data_folder(const fs::path& dir){const auto name=lower_ascii(text(dir.filename()));
    for(const auto* data:{"content","paks","movies","saved","logs","localization","shadercache","crashes"})if(name==data)return true;
    return false;}
}

Discovery discover(const fs::path& input){Discovery out;const auto path=local(input);if(fs::is_regular_file(path)){need(winpath::same_spelling(path.extension(),L".exe"),"请选择游戏目录或 EXE");
        // An Unreal bootstrap at the game root: offer the game itself first, keep
        // the chosen EXE as the last choice.
        if(!holds_dlss(path.parent_path())){auto games=unreal_games(path.parent_path());if(!games.empty()){out.executables=std::move(games);out.executables.push_back(path);
            out.notice="所选 EXE 是虚幻引擎的启动器，旁边没有 DLSS 模块。游戏本体已排在最前。";return out;}}
        out.executables.push_back(path);return out;}need(fs::is_directory(path)&&path!=path.root_path(),"请选择具体游戏目录，不扫描整块磁盘");
    const auto start=GetTickCount64();unsigned count=0;std::error_code ec;for(fs::recursive_directory_iterator it(path,fs::directory_options::none,ec),end;it!=end;it.increment(ec)){if(ec){out.complete=false;break;}
        if(++count>8192||GetTickCount64()-start>15000||out.executables.size()>=128){out.complete=false;break;}const auto attr=GetFileAttributesW(it->path().c_str());if(attr==INVALID_FILE_ATTRIBUTES){out.complete=false;continue;}
        if(attr&FILE_ATTRIBUTE_REPARSE_POINT){it.disable_recursion_pending();out.complete=false;continue;}if((attr&FILE_ATTRIBUTE_DIRECTORY)&&data_folder(it->path())){it.disable_recursion_pending();continue;}if(it.depth()>=7&&it->is_directory()){it.disable_recursion_pending();out.complete=false;}
        if(it->is_regular_file()&&winpath::same_spelling(it->path().extension(),L".exe"))out.executables.push_back(it->path());}if(ec)out.complete=false;
    std::sort(out.executables.begin(),out.executables.end(),[](const fs::path& a,const fs::path& b){const int ra=candidate_rank(a),rb=candidate_rank(b);return ra!=rb?ra<rb:a<b;});
    if(!out.complete)out.notice=out.executables.empty()?"目录检查未完整完成，没有找到 EXE。请直接选择游戏 EXE 再检查。":"目录较大，只检查了一部分；旁边有 DLSS 模块的 EXE 排在最前。";
    return out;}
// ---------------------------------------------------------------- packages
std::vector<Policy> load_packages(const fs::path& lab_root,std::vector<std::string>* notes){
    std::vector<Policy> out;const auto adapters=lab_root/L"app"/L"adapters";std::error_code ec;if(!fs::is_directory(adapters,ec))return out;
    for(const auto& dir:fs::directory_iterator(adapters,ec)){if(!dir.is_directory())continue;const auto file=dir.path()/L"package.json";const auto name=text(dir.path().filename());
        try{if(!fs::is_regular_file(file))continue;auto m=read(file);
            // A package written before the rename (DLSS Lab) is still loaded, so its
            // installed game can be recognised and migrated: its payload names are
            // the old ones and its loader is described under the NEW names, which is
            // what a refresh rewrites it to. It is never installed as it is.
            const bool legacy=m.value("schema","")=="dlsslab-adapter-package-v1";
            need((legacy||m.value("schema","")=="overglaze-adapter-package-v1")&&m.value("name","")==name&&plain_name(name),"清单 schema/名称不匹配");
            Policy p;p.name=name;p.directory=dir.path();p.profile=m.at("profile");p.executable=m.at("executable");p.title=m.value("title",name);p.route=m.value("route","sl-rr");
            p.track=m.value("track","research");p.legacy=legacy;
            if(legacy){const auto old=m.contains("loader")?m.at("loader"):json{{"strategy","root_dxgi_minimal"},{"basename","dxgi.dll"},{"subdir",""}};
                p.legacy_basename=old.value("basename","dxgi.dll");p.legacy_subdir=wide(old.value("subdir",""));
                json now=old;if(p.legacy_basename=="dlsslab_controller.dll"){now["basename"]="overglaze_controller.dll";now["subdir"]="overglaze";}
                else if(p.legacy_basename=="dlsslab_reframework.dll"){now["basename"]="overglaze_reframework.dll";now["subdir"]=old.value("subdir","");}
                if(m.contains("loader"))p.loader=parse_loader(now);}
            else if(m.contains("loader"))p.loader=parse_loader(m.at("loader"));
            p.consent=m.value("consent","");p.checker_sha256=m.value("checker_sha256","");p.config_sha256=m.value("config_sha256","");
            if(m.contains("game_root")&&m.at("game_root").is_string()&&!m.at("game_root").get<std::string>().empty())p.game_root=fs::path(wide(m.at("game_root").get<std::string>()));
            for(auto it=m.at("pins").begin();it!=m.at("pins").end();++it){
                // The EXE pin may be an OS package token (Xbox app titles); every
                // module pin is still a SHA-256 of a file we can read.
                const bool exe_pin=m.contains("executable")&&it.key()==m.at("executable").get<std::string>();
                need(exe_pin?valid_identity_token(it.value().get<std::string>()):hash_ok(it.value().get<std::string>()),"pin hash 无效");p.pins[it.key()]=it.value();}
            for(auto it=m.at("payload").begin();it!=m.at("payload").end();++it){need((legacy?owned_name(it.key()):current_name(it.key()))&&hash_ok(it.value().get<std::string>()),"payload 条目无效");p.payload[it.key()]=it.value();}
            // The loader is keyed by its OWN basename, so a late package carries
            // overglaze_controller.dll where a root package carries dxgi.dll.
            const std::string loader_key=legacy?p.legacy_basename:p.loader.basename,bridge_key=legacy?"dlsslab_nvngx.dll":"overglaze_nvngx.dll";
            need(p.pins.contains(p.executable)&&p.payload.contains(loader_key)&&p.payload.contains(bridge_key)&&p.payload.contains("nvngx_dlssnr.dll"),"清单缺少 EXE pin 或载荷");
            p.accepted_hosts.push_back(p.payload.at(loader_key));p.accepted_bridges.push_back(p.payload.at(bridge_key));
            if(m.contains("adopt_from")&&m.at("adopt_from").is_object()){const auto& a=m.at("adopt_from");
                if(a.contains("dxgi.dll")&&hash_ok(a.at("dxgi.dll").get<std::string>()))p.accepted_hosts.push_back(a.at("dxgi.dll"));
                if(a.contains(bridge_key)&&hash_ok(a.at(bridge_key).get<std::string>()))p.accepted_bridges.push_back(a.at(bridge_key));}
            const auto config_file=dir.path()/(legacy?identity::legacy::kInstallationFile:identity::kInstallationFile);
            if(p.config_sha256.empty()&&fs::is_regular_file(config_file))p.config_sha256=digest(config_file);
            // The risks its preflight found. A manifest written while Denuvo and
            // anti-cheat were refusals says so through its verdict instead.
            if(m.contains("preflight")&&m.at("preflight").is_object()){const auto& pf=m.at("preflight");const auto verdict=pf.value("verdict","");
                std::vector<std::string> markers;if(pf.contains("anticheat_markers")&&pf.at("anticheat_markers").is_array())for(const auto& x:pf.at("anticheat_markers"))if(x.is_string())markers.push_back(x.get<std::string>());
                p.risks=risk_list(pf.value("denuvo_suspected",false)||verdict=="denuvo-blocked",!markers.empty()||verdict=="anticheat-blocked");
                p.anticheat=anticheat_products(markers);}
            if(m.contains("facts")&&m.at("facts").is_object())p.facts=facts_from_json(m.at("facts"),p.profile,name,p.pins.at(p.executable));
            else if(const auto* row=profiles::game(p.profile)){p.facts=profiles::Facts::from(*row);p.facts.package=name;p.facts.title=p.title;
                for(const auto* n:{"sl.interposer.dll","sl.common.dll","sl.dlss_d.dll"})if(p.pins.contains(n))p.facts.modules[n]=p.pins.at(n);}
            else throw std::runtime_error("清单无 facts 且 profile 不在编译期表内");
            out.push_back(std::move(p));
        }catch(const std::exception& e){if(notes)notes->push_back(name+": "+e.what());}}
    std::sort(out.begin(),out.end(),[](const Policy& a,const Policy& b){return a.name<b.name;});return out;}
json policy_json(const Policy& p){json pins=json::object(),payload=json::object();for(auto& [k,v]:p.pins)pins[k]=v;for(auto& [k,v]:p.payload)payload[k]=v;
    return {{"name",p.name},{"title",p.title},{"profile",p.profile},{"route",p.route},{"track",p.track},{"legacy",p.legacy},{"loader",{{"strategy",p.loader.strategy},{"basename",p.loader.basename},{"subdir",text(p.loader.subdir)}}},{"executable",p.executable},{"game_root",text(p.game_root)},{"directory",text(p.directory)},
        {"pins",pins},{"payload",payload},{"config_sha256",p.config_sha256},{"checker_sha256",p.checker_sha256},{"facts",facts_json(p.facts)},{"reviewed_row",p.facts.reviewed},{"risks",p.risks},{"anticheat",p.anticheat}};}

// ---------------------------------------------------------------- preflight
json Preflight::to_json()const{json c=json::array();for(const auto& x:checks)c.push_back(check_json(x));
    return {{"schema","overglaze-game-preflight-v3"},{"executable",executable},{"executable_sha256",executable_sha256},{"executable_readable",executable_readable},{"package_identity",package_identity},
    {"store",store},{"pe_valid",pe_valid},{"pe_x64",pe_x64},{"sections",sections},{"denuvo_suspected",denuvo_suspected},{"modules",modules},{"modules_signed",modules_signed},
    {"anticheat_markers",anticheat_markers},{"anticheat_names",anticheat},{"risks",risks()},{"loader_conflicts",loader_conflicts},{"route",route},{"verdict",verdict},{"scan_complete",scan_complete},{"checks",c},{"notes",notes},
    {"scope","read-only: nothing executed, loaded or written; module presence is navigation, not compatibility; absence of markers proves nothing; a check that could not run is ok:null, never ok:true"}};}
std::vector<std::string> Preflight::risks()const{return risk_list(denuvo_suspected,!anticheat_markers.empty());}
std::string store_kind(const fs::path& dir,bool has_package_identity){return has_package_identity?std::string("gdk"):store_of(dir);}
std::string steam_launch_option(const fs::path& root){return "\""+text((root/L"app"/L"overglaze_launch.exe").lexically_normal())+"\" %command%";}
std::string watch_command(const fs::path& root){return "\""+text((root/L"app"/L"overglaze_games.exe").lexically_normal())+"\" watch";}
Preflight preflight(const fs::path& input){
    Preflight r;const auto exe=local(input);need(fs::is_regular_file(exe)&&winpath::same_spelling(exe.extension(),L".exe"),"请选择实际游戏 EXE");const auto dir=exe.parent_path();
    r.executable=text(exe.filename());
    // The directory markers alone; the package identity below may still decide
    // the store (an installed OS package means Xbox app / GDK).
    const auto marker_store=store_of(dir);
    std::string pe_error;
    {Handle h(CreateFileW(exe.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
     if(!h.valid()){r.executable_readable=false;r.notes.push_back(GetLastError()==ERROR_ACCESS_DENIED?"EXE 读取被拒（受商店许可保护的包，如 Xbox app/GDK）":"EXE 无法打开");}
     else{r.executable_readable=true;const auto pe=pe_info(h.value);r.pe_valid=pe.valid;r.pe_x64=pe.x64;r.sections=pe.sections;pe_error=pe.error;if(!pe.valid)r.notes.push_back("PE 头无法解析: "+pe.error);
        for(const auto& s:r.sections)if(s==".xtext"||s==".xcode"||s==".xtls"||s==".sxdata")r.denuvo_suspected=true;
        if(r.denuvo_suspected)r.notes.push_back("节表含 Denuvo 虚拟化段（.xtext/.xcode/.xtls/.sxdata）：作为风险提示，安装时由用户确认；不绕过、不修补反篡改");
        r.executable_sha256=handle_digest(h.value);}}
    // The OS package, when Windows has one installed around this EXE, is the
    // identity for readable and unreadable EXEs alike -- the same rule the
    // checker and the in-game host apply, so all three compute one value.
    {std::string why;
     if(const auto pkg=installed_package_covering(exe,&why)){
        r.package_identity={{"full_name",text(pkg->full_name)},{"family",text(pkg->family_name)},{"version",pkg->version},{"install_path",text(pkg->install_path)}};
        r.executable_sha256="package:"+text(pkg->full_name);
        r.notes.push_back("身份取自 Windows 已安装的包 "+text(pkg->full_name)+"（版本 "+pkg->version+"），EXE 不参与");
        if(!r.executable_readable)r.notes.push_back("EXE 不可读：无法检查 Denuvo 节表与 PE 头，按包身份继续");
     }else if(marker_store=="gdk")r.notes.push_back("未能从系统确认包身份："+why);}
    r.store=store_kind(dir,!r.package_identity.is_null());
    bool interposer=false,dlss=false,dlss_d=false,ngx=false,ngx_d=false;
    json unreadable=json::array(),unsigned_modules=json::array();
    const auto module_dirs=module_directories(dir); // walked once for every module name
    for(const auto* n:{"sl.interposer.dll","sl.common.dll","sl.dlss.dll","sl.dlss_d.dll","sl.dlss_g.dll","sl.reflex.dll","sl.pcl.dll","nvngx_dlss.dll","nvngx_dlssd.dll","nvngx_dlssg.dll"}){
        fs::path p;std::error_code ec;bool found=false;
        for(const auto& candidate:module_dirs){p=candidate/wide(n);if(fs::is_regular_file(p,ec)){found=true;break;}}
        if(!found)continue;
        try{winpath::require_no_reparse(p);auto id=file_identity(p);id.erase("path");
            // Where it was found matters: beside the executable, or in the Unreal
            // plugin tree. The caller cannot tell those apart from the name alone.
            if(!winpath::same_spelling(p.parent_path(),dir))id["directory"]=text(fs::relative(p.parent_path(),dir,ec));
            r.modules[n]=id;
            const std::string name(n);if(name.rfind("sl.",0)==0&&id.value("signature","")!="ValidCachedTrust"){r.modules_signed=false;unsigned_modules.push_back(name);r.notes.push_back(name+" 未通过 Authenticode 校验；宿主在挂接时会拒绝未签名的 Streamline 模块");}
            if(name=="sl.interposer.dll")interposer=true;if(name=="sl.dlss.dll")dlss=true;if(name=="sl.dlss_d.dll")dlss_d=true;
            if(name.rfind("nvngx_dlss",0)==0)ngx=true;if(name=="nvngx_dlssd.dll")ngx_d=true;
        }catch(const std::exception& e){r.notes.push_back(std::string(n)+": "+e.what());r.scan_complete=false;unreadable.push_back(n);}}
    bool markers_complete=true;unsigned scanned=0;
    scan_markers(dir,r.anticheat_markers,markers_complete,scanned);if(!markers_complete)r.scan_complete=false;r.loader_conflicts=conflicts(dir);
    r.anticheat=anticheat_products(r.anticheat_markers);
    if(!r.anticheat.empty())r.notes.push_back("发现反作弊相关文件（按文件名判断）：作为风险提示，安装时由用户确认；Overglaze 不隐藏自己，不绕过反作弊");
    // A game may ship Streamline for frame generation and still call DLSS and Ray
    // Reconstruction straight through NGX -- the Unreal DLSS plugin does exactly
    // that (Halo: Campaign Evolved ships an interposer and still calls RR through
    // NGX). What decides the route is which surface carries DLSS, not whether
    // Streamline is present at all.
    r.route=interposer&&dlss_d?"sl-rr":interposer&&dlss?"sl-sr":ngx_d?"ngx-rr":ngx?"ngx-sr":"none";
    if(r.route=="none"){const auto games=unreal_games(dir);if(!games.empty()){std::error_code rec;
        r.notes.push_back("这是虚幻引擎的启动器，游戏本体是 "+text(fs::relative(games.front(),dir,rec))+"。请把它从列表移除，改为添加游戏本体。");}}
    // The verdict ladder holds only what is functional: an identity that cannot
    // be checked, another loader in the folder, the DLSS route. Anti-tamper and
    // anti-cheat are risks (risks(), and the checks below name what was found),
    // shown to the user and accepted at install -- not rungs that refuse.
    // "Cannot check" stays visible in the checks instead of passing silently;
    // it does not by itself turn a supported game into a refused one -- the
    // Xbox app titles that work today (Hellblade 2, A Plague Tale) are exactly
    // the unreadable-EXE case.
    if(!r.executable_readable&&r.package_identity.is_null())r.verdict="unsupported-store";
    else if(!r.loader_conflicts.empty())r.verdict="loader-conflict";
    else if(r.route=="sl-rr"){r.verdict="sl-rr-ready";
        r.notes.push_back("控制器宿主：游戏关闭光线重建、改用 DLSS 超分时，NR 自动改接在超分输出之后");}
    else if(r.route=="sl-sr"){r.verdict="sl-sr-ready";
        r.notes.push_back("只有 DLSS 超分（无 sl.dlss_d.dll）：控制器宿主把 NR 插在超分输出之后（纯超分游戏未实机验证）");}
    else if(r.route=="ngx-rr"){r.verdict="ngx-rr-ready";r.notes.push_back("DLSS 与光线重构走 NGX 直连（UE DLSS 插件），Streamline 只管帧生成：准入经 NGX 观察面的 Evaluate 钩子（依据光环实测；首次实机未验证）");}
    else if(r.route=="ngx-sr"){r.verdict="ngx-sr-ready";r.notes.push_back("NGX 直连、只有超分模型（无 nvngx_dlssd.dll）：NR 插在 DLSS SR 输出之后（纯 SR 游戏未实机验证）");}
    else{r.verdict="no-dlss";r.notes.push_back("目录内没有 Streamline 或 NGX DLSS 模块；NR 需要 RR/SR 输入");}
    if(!r.pe_x64&&r.executable_readable)r.notes.push_back("EXE 不是 x64 PE 映像");
    // ---- the same facts as three-state checks
    auto add=[&](const char* name,Outcome o,json value=nullptr,const char* reason=""){r.checks.push_back({name,o,std::move(value),reason});};
    if(!r.executable_sha256.empty())add("identity",Outcome::pass,r.executable_sha256);
    else add("identity",Outcome::unknown,nullptr,"exe-unreadable-no-package");
    if(!r.executable_readable)add("pe",Outcome::unknown,nullptr,"exe-unreadable");
    else if(!r.pe_valid)add("pe",Outcome::fail,pe_error,"pe-invalid");
    else if(!r.pe_x64)add("pe",Outcome::fail,nullptr,"not-x64");
    else add("pe",Outcome::pass,"x64");
    if(!r.executable_readable)add("denuvo",Outcome::unknown,nullptr,"exe-unreadable");
    else if(!r.pe_valid)add("denuvo",Outcome::unknown,pe_error,"pe-invalid");
    else if(r.denuvo_suspected){json found=json::array();for(const auto& s:r.sections)if(s==".xtext"||s==".xcode"||s==".xtls"||s==".sxdata")found.push_back(s);add("denuvo",Outcome::fail,found,"denuvo-sections");}
    else add("denuvo",Outcome::pass);
    if(!r.anticheat_markers.empty())add("anticheat",Outcome::fail,r.anticheat,"markers-found");
    else if(!markers_complete)add("anticheat",Outcome::unknown,json{{"entries",scanned},{"limit",kMarkerScanEntries},{"seconds",kMarkerScanSeconds}},"scan-truncated");
    else add("anticheat",Outcome::pass,json{{"entries",scanned}});
    if(!r.loader_conflicts.empty())add("loader-conflicts",Outcome::fail,r.loader_conflicts,"conflicts-found");
    else add("loader-conflicts",Outcome::pass);
    if(!unreadable.empty())add("modules",Outcome::unknown,unreadable,"module-unreadable");
    else add("modules",Outcome::pass,r.modules.size());
    bool sl_unreadable=false;for(const auto& n:unreadable)if(n.get<std::string>().rfind("sl.",0)==0)sl_unreadable=true;
    if(!unsigned_modules.empty())add("module-signatures",Outcome::fail,unsigned_modules,"unsigned-modules");
    else if(sl_unreadable)add("module-signatures",Outcome::unknown,unreadable,"module-unreadable");
    else add("module-signatures",Outcome::pass);
    if(r.route=="none")add("route",Outcome::fail,r.route,"no-dlss-modules");
    else add("route",Outcome::pass,r.route);
    return r;}
// ---------------------------------------------------------------- manager
Manager::Manager(fs::path root,std::optional<std::vector<Policy>> policies,std::string model_sha256,bool run_checker):root_(local(root)),store_(root_/L"data"/L"settings"/L"plugin-manager"),model_sha256_(std::move(model_sha256)),run_checker_(run_checker){
    directory(root_/L"data");directory(root_/L"data"/L"settings");directory(store_);
    policies_=policies?std::move(*policies):load_packages(root_,&package_notes_);}
std::vector<Entry> Manager::list()const{std::vector<Entry> out;auto j=registry(store_);for(auto& v:j["entries"])out.push_back(entry(v));return out;}
Entry Manager::find(const std::string& id)const{for(auto& e:list())if(e.id==id)return e;throw std::runtime_error("游戏未登记或已被其他窗口移除");}
const Policy* Manager::match(const fs::path& exe)const{const Policy* rooted=nullptr;std::vector<const Policy*> named;
    for(const auto& p:policies_){if(!winpath::same_spelling(exe.filename(),wide(p.executable)))continue;
        if(!p.game_root.empty()){if(winpath::same_spelling(p.game_root,exe.parent_path())){need(!rooted,"多个适配包指向同一游戏目录");rooted=&p;}}else named.push_back(&p);}
    if(rooted)return rooted;if(named.size()==1)return named.front();need(named.empty(),"多个适配包命中同一 EXE 名而无目录绑定");return nullptr;}
Entry Manager::add(const fs::path& exe){const auto path=local(exe);need(fs::is_regular_file(path)&&winpath::same_spelling(path.extension(),L".exe"),"请选择实际游戏 EXE");Writer lock(store_);auto j=registry(store_);for(auto& v:j["entries"]){auto e=entry(v);if(winpath::same_spelling(e.exe,path))return e;}need(j["entries"].size()<128,"游戏清单已达 128 项上限");
    const auto* p=match(path);Entry e{uuid(),p?p->title:text(path.stem()),path};j["entries"].push_back({{"id",e.id},{"title",e.title},{"exe",text(e.exe)}});save(store_/L"games.json",j);return e;}
Preflight Manager::preflight(const Entry& e)const{return games::preflight(e.exe);}
std::map<std::string,std::string> Manager::published_host(const std::string& track)const{std::map<std::string,std::string> out;
    // Two tracks publish two different hosts. Comparing a package against the
    // wrong one would mark it stale and a refresh would then write the other
    // track's host into the game, which is exactly what must never happen.
    need(track=="controller"||track=="research","适配包的 track 只能是 controller 或 research");
    const auto dir=track=="controller"?root_/L"app"/L"plugin":root_/L"app"/L"research"/L"host";
    const std::string shown=track=="controller"?"app/plugin/":"app/research/host/";
    for(const auto* n:{"dxgi.dll","overglaze_nvngx.dll"}){const auto p=dir/wide(n);need(fs::is_regular_file(p),"已发布宿主缺失: "+shown+n);out[n]=digest(p);}
    // The late loader is published beside the proxy; a package that uses it
    // compares against this entry instead of dxgi.dll.
    for(const auto* n:{"overglaze_controller.dll"}){const auto p=dir/wide(n);if(fs::is_regular_file(p))out[n]=digest(p);}
    return out;}
Status Manager::inspect(const Entry& e)const{auto s=inspect_impl(e,true);
    // The risks, named as facts after whatever the state says: never a refusal.
    for(const auto& r:s.risks)s.reasons.push_back(r=="anti-tamper"?Reason{"risk-anti-tamper"}:Reason{"risk-anticheat",{{"names",s.anticheat}}});
    // How a late-loading game is started, built from this program's own root.
    if(s.load_mode=="late_d3d12"){s.launch_via=s.store=="steam"?"steam":"watch";s.launch_command=s.launch_via=="steam"?steam_launch_option(root_):watch_command(root_);}
    return s;}
namespace {
// Lab-owned files of this package already in its game (by name only; the
// transaction or the existing-install check decides whose they are).
bool lab_files_present(const Policy& p){if(p.game_root.empty())return false;const auto lab_dir=lab_directory(p.game_root,p);std::error_code ec;
    if(fs::exists(lab_dir/identity::kInstallationFile,ec)||fs::exists(lab_dir/identity::kBridgeFile,ec)||
        (p.loader.basename!="dxgi.dll"&&fs::exists(lab_dir/wide(p.loader.basename),ec)))return true;
    // A pre-rename installation of the same game, in its own place and names.
    for(const auto& sub:{fs::path(identity::legacy::kPayloadSubdirectory),fs::path()}){const auto d=sub.empty()?p.game_root:p.game_root/sub;
        if(fs::exists(d/identity::legacy::kInstallationFile,ec)||fs::exists(d/identity::legacy::kBridgeFile,ec)||fs::exists(d/identity::legacy::kControllerFile,ec))return true;}
    return false;}
// The parts of a package payload that differ from the published host of its
// track. Throws when the published host cannot be read.
std::vector<std::string> behind_published(const Policy& p,const std::map<std::string,std::string>& published){std::vector<std::string> parts;
    if(p.legacy)return {"host","bridge"}; // pre-rename names: every part is behind
    if(p.payload.at(p.loader.basename)!=published.at(p.loader.basename))parts.push_back("host");
    if(p.payload.at("overglaze_nvngx.dll")!=published.at("overglaze_nvngx.dll"))parts.push_back("bridge");
    // The root proxy is payload like the rest and is re-copied by a refresh,
    // so a newer published proxy is an update too.
    const auto proxy=p.loader.proxy_basename();
    if(!proxy.empty()&&p.payload.contains(proxy)&&published.contains(proxy)&&p.payload.at(proxy)!=published.at(proxy))parts.push_back("proxy");
    return parts;}
bool conflict_code(const std::string& c){return c=="loader-conflict"||c=="foreign-loader"||c=="model-mismatch"||c=="package-payload-mismatch"||c=="local-model-mismatch"||c=="package-config-mismatch"||c=="published-host-missing"||
    c=="model-missing"||c=="model-unknown-version";}
}
Reason Manager::refresh_refusal(const Policy& p,const Preflight& pre)const{
    // Anti-tamper and anti-cheat are not part of this: they are risks the
    // user acknowledges at install, never a refusal here.
    if(!admitting_verdict(pre.verdict))return {"update-refresh-refused",{{"verdict",pre.verdict}}};
    if(pre.executable_sha256!=p.pins.at(p.executable))return {"update-refresh-refused",{{"verdict","identity"}}};
    for(const auto& [n,pin]:p.facts.modules)if(!pre.modules.contains(n)||pre.modules.at(n).value("sha256","")!=pin)return {"update-refresh-refused",{{"verdict","module "+n}}};
    return {};}
Reason Manager::repin_refusal(const Policy& p,const fs::path& exe,bool allow_unsigned,Preflight* pre)const{
    // Only the controller track is re-adapted here. A research package and a
    // package whose profile is a compiled review row (2077, 007, RE9, AW2:
    // the in-game host demands the row's own EXE hash) need the research tools
    // or a reviewed row change, never an automatic repin.
    if(p.track!="controller")return {"repin-research-track"};
    if(profiles::game(p.profile)||p.facts.reviewed)return {"repin-compiled-row",{{"profile",p.profile}}};
    if(p.game_root.empty())return {"repin-no-game-root"};
    *pre=games::preflight(exe);
    if(!admitting_verdict(pre->verdict))return {"repin-preflight-failed",{{"verdict",pre->verdict}}};
    if(!pre->modules_signed&&!allow_unsigned)return {"repin-unsigned-modules"};
    if(pre->route!=p.route)return {"repin-route-changed",{{"old",p.route},{"new",pre->route}}};
    return {};}
Status Manager::inspect_impl(const Entry& e,bool full)const{Status s;s.entry=e;try{
    const auto exe=within("path-invalid",[&]{return local(e.exe);}),dir=exe.parent_path();
    s.running=within("process-check-failed",[&]{return running(exe);});
    const Policy* p=within("multiple-packages",[&]{return match(exe);});
    if(p){s.package=p->name;s.route=p->route;s.track=p->track;s.load_mode=p->loader.strategy;s.risks=p->risks;s.anticheat=p->anticheat;
        s.store=store_kind(dir,is_package_token(p->pins.at(p->executable)));}
    // A game whose EXE or modules changed: offer the re-adaptation when this
    // package qualifies and its fresh preflight passes.
    auto changed=[&](Reason why){s.state="changed";s.install_state="game-changed";s.health=s.installed?"game-changed":"not-applicable";
        s.reasons.push_back(std::move(why));if(s.can_uninstall)s.reasons.push_back({"uninstall-still-possible"});
        if(!full||!p)return;
        Preflight pre;const auto refusal=repin_refusal(*p,exe,false,&pre);
        if(!pre.executable.empty()){s.preflight=pre.to_json();for(const auto& c:pre.checks)s.checks.push_back(c);
            s.compatibility=admitting_verdict(pre.verdict)||pre.verdict=="loader-conflict"?"supported":"unsupported";
            // The changed game's own risks, as found now.
            s.risks=pre.risks();s.anticheat=pre.anticheat;}
        if(refusal.code.empty()){s.can_repin=!s.running;s.reasons.push_back({"repin-available"});}
        else{s.refusals["repin"]=refusal;s.reasons.push_back(refusal);}};
    const auto tx=store_/wide(e.id)/L"transaction.json";
    if(fs::exists(tx)){auto j=within("transaction-invalid",[&]{return receipt(tx,e);});const auto state=j.at("state").get<std::string>();if(state!="removed"){
        gate(s,"transaction",state=="installing"||state=="installed"||state=="removing","transaction-invalid","安装事务状态无效",json{{"message","安装事务状态无效"}},state);
        // Where this transaction actually put its files. Absent means the game
        // root (the original root layout).
        const auto sub=j.value("loader_subdirectory","");
        const auto tx_dir=sub.empty()?dir:(dir/wide(sub)).lexically_normal();
        const auto tx_loader=wide(j.value("loader_basename","dxgi.dll"));
        const std::string strategy=j.value("strategy","");
        s.load_mode=strategy.empty()?std::string("root_dxgi_minimal"):strategy;
        // The root-proxy strategy puts exactly one recorded file outside the
        // payload directory: the thin dxgi.dll the game itself loads.
        const std::string tx_proxy=proxied(strategy)?"dxgi.dll":std::string();
        const auto tx_located=[&](const std::string& n){return !tx_proxy.empty()&&n==tx_proxy?dir/wide(n):tx_dir/wide(n);};
        bool present=false,missing=false;json files_seen=json::object();
        for(auto it=j["files"].begin();it!=j["files"].end();++it){auto f=tx_located(it.key());
            if(fs::exists(f)){const auto h=within("file-unreadable",[&]{return digest(f);});
                if(h!=it.value().get<std::string>()){record(s,"files",Outcome::fail,it.key(),"installed-files-modified");throw Refusal("installed-files-modified","已登记插件被外部修改，拒绝卸载或覆盖",json{{"file",it.key()}});}
                files_seen[it.key()]=h;present=true;}
            else missing=true;}
        record(s,"files",missing?Outcome::fail:Outcome::pass,files_seen.size(),missing?"incomplete":"");
        s.installed=present;s.can_uninstall=!s.running;s.state=state=="installed"&&!missing?"installed":"incomplete";
        s.host_hash=fs::exists(tx_dir/tx_loader)?within("file-unreadable",[&]{return digest(tx_dir/tx_loader);}):"";
        if(!p){record(s,"health.manifest",Outcome::fail,nullptr,"installed-package-missing");changed({"installed-package-missing"});return s;}
        if(within("file-unreadable",[&]{return game_identity_token(exe);})!=p->pins.at(p->executable)){
            record(s,"game-pins",Outcome::fail,p->executable,"game-changed");changed({"game-changed",{{"module",p->executable}}});return s;}
        s.compatibility="supported";
        if(s.state=="incomplete"){s.install_state="incomplete";s.health="unknown";s.reasons.push_back({"incomplete"});return s;}
        // Installed by the pre-rename build (DLSS Lab). A V4 host never reads these
        // files, so this game has no working plugin until it is migrated: remove
        // them by their recorded hashes, rewrite the package under the new names
        // and install it -- update() with the refresh, as one operation.
        if(legacy_transaction(j)){
            record(s,"identity",Outcome::fail,identity::legacy::kTechnicalId,"legacy-install");
            s.health="needs-update";s.install_state="legacy";s.update_available=true;s.update.package_behind_published=true;
            s.reasons.push_back({"legacy-install"});
            if(full){const auto why=refresh_refusal(*p,games::preflight(exe));if(!why.code.empty())s.refusals["update"]=why;
                else{const auto m=model_status();if(!m.known){s.refusals["update"]=m.present?Reason{"model-unknown-version",{{"hash",m.sha256}}}:Reason{"model-missing",{{"path",text(m.path)}}};}}}
            s.can_update=!s.running&&!s.refusals.contains("update");
            return s;}
        // ---- health: what the in-game host checks at start-up
        // against the CURRENT package manifest -- every pinned module, then the
        // host, bridge and config hashes. A module that changed is a game update;
        // a payload the manifest no longer names is "需要更新才能使用".
        {const auto module_dirs=module_directories(dir);
         for(const auto& [name,pin]:p->pins){if(name==p->executable)continue;
            bool matched=false;std::error_code ec;
            for(const auto& candidate:module_dirs){const auto f=candidate/wide(name);if(!fs::is_regular_file(f,ec))continue;
                matched=within("file-unreadable",[&]{return digest(f);})==pin;break;} // first place it exists is the one that must match
            if(!matched){record(s,"game-pins",Outcome::fail,name,"game-changed");changed({"game-changed",{{"module",name}}});return s;}}
         record(s,"game-pins",Outcome::pass,p->pins.size());}
        const auto installed_hash=[&](const std::string& n){const auto f=tx_located(n);return fs::exists(f)?within("file-unreadable",[&]{return digest(f);}):std::string();};
        record(s,"health.manifest",Outcome::pass,p->name);
        s.update.host=p->payload.contains(p->loader.basename)&&s.host_hash!=p->payload.at(p->loader.basename);
        s.update.bridge=p->payload.contains("overglaze_nvngx.dll")&&installed_hash("overglaze_nvngx.dll")!=p->payload.at("overglaze_nvngx.dll");
        s.update.config=!p->config_sha256.empty()&&installed_hash("overglaze.install.json")!=p->config_sha256;
        record(s,"health.host",s.update.host?Outcome::fail:Outcome::pass,s.host_hash,s.update.host?"needs-update":"");
        record(s,"health.bridge",s.update.bridge?Outcome::fail:Outcome::pass,nullptr,s.update.bridge?"needs-update":"");
        if(p->config_sha256.empty())record(s,"health.config",Outcome::unknown,nullptr,"package-config-mismatch");
        else record(s,"health.config",s.update.config?Outcome::fail:Outcome::pass,nullptr,s.update.config?"needs-update":"");
        // The proxy is not part of the host's chain: a different one still starts.
        if(!tx_proxy.empty()&&p->payload.contains(tx_proxy)){s.update.proxy=installed_hash(tx_proxy)!=p->payload.at(tx_proxy);
            record(s,"update.proxy",s.update.proxy?Outcome::fail:Outcome::pass,nullptr,s.update.proxy?"update-available":"");}
        const bool broken=s.update.host||s.update.bridge||s.update.config;
        s.health=broken?"needs-update":"ok";
        // ---- the package itself behind the published host of its track
        std::vector<std::string> newer;
        if(full){try{newer=behind_published(*p,published_host(p->track));s.update.package_behind_published=!newer.empty();
                record(s,"published",newer.empty()?Outcome::pass:Outcome::fail,newer,newer.empty()?"":"package-behind-published");}
            catch(const std::exception& ex){record(s,"published",Outcome::unknown,ex.what(),"published-unreadable");}}
        s.update_available=broken||s.update.proxy||s.update.package_behind_published;
        s.install_state=broken?"needs-update":s.update_available?"update-available":"installed";
        json parts=json::array();if(s.update.host)parts.push_back("host");if(s.update.bridge)parts.push_back("bridge");if(s.update.config)parts.push_back("config");if(s.update.proxy)parts.push_back("proxy");
        if(broken)s.reasons.push_back({"needs-update",{{"parts",parts}}});
        else if(s.update_available){for(const auto& n:newer)if(std::find(parts.begin(),parts.end(),n)==parts.end())parts.push_back(n);s.reasons.push_back({"update-available",{{"parts",parts}}});}
        else s.reasons.push_back({"installed"});
        if(s.update.package_behind_published)s.reasons.push_back({"package-behind-published",{{"published",p->track=="controller"?"app/plugin":"app/research/host"}}});
        s.reasons.push_back(launch_reason(strategy,sub,root_,s.store));
        // An update that has to refresh the package re-runs the refresh gates
        // first; say now whether they would pass, rather than after an uninstall.
        if(full&&s.update.package_behind_published){const auto why=refresh_refusal(*p,games::preflight(exe));if(!why.code.empty())s.refusals["update"]=why;}
        s.can_update=s.update_available&&!s.running&&!s.refusals.contains("update");
        return s;}}
    if(!p){const auto pre=games::preflight(exe);s.preflight=pre.to_json();s.route=pre.route;s.checks=pre.checks;s.store=pre.store;
        // Denuvo and anti-cheat are risks (s.risks), never a state of their own:
        // such a game gets the ordinary states, and the install dialog names them.
        s.risks=pre.risks();s.anticheat=pre.anticheat;
        s.state=admitting_verdict(pre.verdict)?"needs-package":pre.verdict;s.can_make_package=s.state=="needs-package"&&!s.running;
        s.compatibility=s.state=="needs-package"||s.state=="loader-conflict"?"supported":"unsupported";
        if(s.state=="needs-package"){s.reasons.push_back({"package-missing",{{"route",pre.route},{"store",pre.store}}});
            if(!pre.modules_signed)s.reasons.push_back({"modules-unsigned"});
            // A package carries the user's model hash, so it cannot be made without
            // a reviewed model in place; say which, before the button is pressed.
            const auto m=model_status();
            if(!m.known){const Reason why=m.present?Reason{"model-unknown-version",{{"hash",m.sha256}}}:Reason{"model-missing",{{"path",text(m.path)}}};
                record(s,"local-model",Outcome::fail,m.present?json(m.sha256):json(nullptr),why.code);
                s.can_make_package=false;s.refusals["make-package"]=why;s.reasons.push_back(why);}}
        else if(s.state=="loader-conflict")s.reasons.push_back({"loader-conflict",{{"files",pre.loader_conflicts}}});
        else s.reasons.push_back({s.state}); // unsupported-store | no-dlss
        json unknown=json::array();for(const auto& c:pre.checks)if(c.outcome==Outcome::unknown)unknown.push_back(c.name);
        if(!unknown.empty())s.reasons.push_back({"preflight-incomplete",{{"count",unknown.size()},{"check",unknown}}});
        return s;}
    // A pinned module is looked for wherever the game actually keeps it, which is
    // beside the executable for most games and inside the plugin tree for an
    // Unreal one (Halo). Searching only beside the executable would report such
    // a game's unchanged modules as "changed". The identity requirement is unchanged:
    // the file must exist somewhere we recognise AND hash to the pin.
    const auto module_dirs=module_directories(dir);
    for(const auto& [name,pin]:p->pins){
        // An OS-package EXE pin is compared through the package identity: the
        // EXE of an Xbox app title cannot be opened, and trying to was the last
        // place this loop still assumed every pin names a readable file.
        if(name==p->executable&&is_package_token(pin)){
            if(within("file-unreadable",[&]{return game_identity_token(exe);})!=pin){record(s,"game-pins",Outcome::fail,name,"game-package-changed");changed({"game-package-changed",{{"package",pin.substr(8)}}});return s;}
            continue;}
        bool matched=false;std::error_code ec;
        for(const auto& candidate:module_dirs){
            const auto f=candidate/wide(name);
            if(!fs::is_regular_file(f,ec))continue;
            if(within("file-unreadable",[&]{return digest(f);})==pin)matched=true;
            break; // first place it exists is the one that must match
        }
        if(!matched){record(s,"game-pins",Outcome::fail,name,"game-changed");changed({"game-changed",{{"module",name}}});return s;}}
    record(s,"game-pins",Outcome::pass,p->pins.size());s.compatibility="supported";
    // Lab files with no transaction of ours: under the current names, or a
    // pre-rename installation in its own place (DLSS Lab, research tools).
    struct Found {fs::path dir;std::wstring loader,config,bridge;bool legacy=false;};
    const Found current{lab_directory(dir,*p),wide(p->loader.basename),identity::kInstallationFile,identity::kBridgeFile,false};
    const Found old{p->legacy?(p->legacy_subdir.empty()?dir:(dir/p->legacy_subdir).lexically_normal()):(dir/identity::legacy::kPayloadSubdirectory),
        wide(p->legacy?p->legacy_basename:std::string("dlsslab_controller.dll")),identity::legacy::kInstallationFile,identity::legacy::kBridgeFile,true};
    const auto present=[](const Found& f){return fs::exists(f.dir/f.loader)||fs::exists(f.dir/f.config)||fs::exists(f.dir/f.bridge);};
    // The root proxy dxgi.dll is shared by both identities; only a config or a
    // bridge says which one is there.
    const auto legacy_present=[&]{return fs::exists(old.dir/old.config)||fs::exists(old.dir/old.bridge)||(old.loader!=L"dxgi.dll"&&fs::exists(old.dir/old.loader));};
    const auto lab_dir=current.dir;const auto loader_name=current.loader;
    if(legacy_present()||present(current)){
        const auto& f=legacy_present()?old:current;
        const auto h=within("file-unreadable",[&]{return digest(f.dir/f.loader);});
        gate(s,"existing-host",std::find(p->accepted_hosts.begin(),p->accepted_hosts.end(),h)!=p->accepted_hosts.end(),"foreign-loader","dxgi.dll 已被其他插件占用或 Lab 版本未知，拒绝覆盖",json{{"file",utf8(f.loader)}},h);
        const auto bridge=within("file-unreadable",[&]{return digest(f.dir/f.bridge);});
        gate(s,"existing-bridge",std::find(p->accepted_bridges.begin(),p->accepted_bridges.end(),bridge)!=p->accepted_bridges.end(),"foreign-bridge","已有 NR 桥接文件身份不同",json::object(),bridge);
        auto c=within("existing-config-unsupported",[&]{return read(f.dir/f.config);});const auto v=c.value("version",0);
        gate(s,"existing-config",(v==2&&c.size()==10)||(v==3&&(c.size()==12||c.size()==13))||(v==4&&(c.size()==11||c.size()==12)),"existing-config-unsupported","既有安装配置不是受支持的 V2-V4 契约",json::object(),v);
        const bool consistent=within("existing-config-mismatch",[&]{return c.at("profile")==p->profile&&c.at("game_sha256")==p->pins.at(p->executable)&&c.at("host_sha256")==h&&c.at("bridge_sha256")==bridge&&scope_declared(c)&&c.at("in_game_controls")==true;});
        gate(s,"existing-config",consistent,"existing-config-mismatch","既有安装配置与适配包不一致");
        const auto output_root=within("existing-config-mismatch",[&]{return c.at("output_root").get<std::string>();});
        gate(s,"existing-output-root",winpath::same_spelling(fs::path(wide(output_root)),root_/L"data"),"installed-by-other-copy","既有安装输出目录与当前 Lab 不匹配",json{{"output_root",output_root}},output_root);
        s.installed=true;s.can_uninstall=!s.running;s.host_hash=h;s.state="existing";
        s.load_mode=c.contains("loader")&&c.at("loader").is_object()?c.at("loader").value("strategy","root_dxgi_minimal"):std::string("root_dxgi_minimal");
        // A research install -- one the research tools made, or one that lags
        // its package -- is shown read-only; the research tools own it.
        const bool up_to_date=!f.legacy&&!p->legacy&&h==p->payload.at(p->loader.basename);
        if(f.legacy)s.reasons.push_back({"legacy-install"});
        if(!up_to_date||p->track=="research"){s.install_state="research-managed";s.reasons.push_back({"existing-research"});}
        else{s.install_state="external";s.reasons.push_back({"existing-external"});}
        return s;}
    const auto collision=conflicts(dir);
    gate(s,"loader-conflicts",collision.empty(),"loader-conflict","检测到其他加载器 / ReShade，需先处理兼容关系；不覆盖",json{{"files",collision}},collision);
    if(p->legacy){
        // The package itself is still the pre-rename one: a refresh rewrites it
        // under the new names from the published host; nothing is installed yet.
        record(s,"published",Outcome::fail,nullptr,"legacy-package");
        s.state="package-stale";s.can_refresh_package=true;s.reasons.push_back({"legacy-package"});
        return s;}
    if(fs::exists(dir/L"nvngx_dlssnr.dll"))gate(s,"model",within("file-unreadable",[&]{return digest(dir/L"nvngx_dlssnr.dll");})==p->payload.at("nvngx_dlssnr.dll"),"model-mismatch","已有 NR 模型身份不同，不覆盖");
    if(full){
        bool payload_ok=true;for(const auto& n:{p->loader.basename,std::string("overglaze_nvngx.dll")})if(within("file-unreadable",[&]{return digest(p->directory/wide(n));})!=p->payload.at(n))payload_ok=false;
        gate(s,"package-payload",payload_ok,"package-payload-mismatch","适配包载荷与清单不一致");
        {const auto m=model_status();
         gate(s,"local-model",m.present,"model-missing","找不到 NR 模型 nvngx_dlssnr.dll：本项目不包含、也不分发它，请自行准备并放进 "+text(m.path.parent_path()),json{{"path",text(m.path)}});
         gate(s,"local-model",m.known,"model-unknown-version","这份 nvngx_dlssnr.dll 不是已审阅的版本，不使用",json{{"hash",m.sha256}},m.sha256);
         gate(s,"local-model",m.sha256==p->payload.at("nvngx_dlssnr.dll"),"local-model-mismatch","本地模型与适配包清单不一致",json::object(),m.sha256);}
        gate(s,"package-config",!p->config_sha256.empty()&&within("file-unreadable",[&]{return digest(p->directory/L"overglaze.install.json");})==p->config_sha256,"package-config-mismatch","适配包配置与清单 config_sha256 不一致");
        const auto published=within("published-host-missing",[&]{return published_host(p->track);});
        const auto newer=behind_published(*p,published);
        if(!newer.empty()){record(s,"published",Outcome::fail,newer,"package-stale");
            s.state="package-stale";s.can_refresh_package=true;
            s.reasons.push_back({"package-stale",{{"published",p->track=="controller"?"app/plugin":"app/research/host"}}});
            return s;}
        record(s,"published",Outcome::pass);}
    s.state="available";s.can_install=!s.running;
    s.reasons.push_back({"package-ready",{{"package",p->title},{"route",p->route}}});
    s.reasons.push_back(launch_reason(p->loader.strategy,text(p->loader.subdir),root_,s.store));
}catch(const Refusal& r){s.state="blocked";s.reasons.clear();s.reasons.push_back({r.code,r.params});
    s.can_install=s.can_uninstall=s.can_make_package=s.can_update=s.can_repin=false;s.health=s.installed?"unknown":"not-applicable";
    s.install_state=r.code=="installed-files-modified"?"modified":r.code=="installed-by-other-copy"?"other-copy":!s.installed&&conflict_code(r.code)?"not-installed":"unknown";}
catch(const std::exception& ex){s.state="blocked";s.reasons.clear();s.reasons.push_back({"internal-error",{{"message",ex.what()}}});
    s.can_install=s.can_uninstall=s.can_make_package=s.can_update=s.can_repin=false;s.health=s.installed?"unknown":"not-applicable";s.install_state="unknown";}
return s;}
std::string describe(const Status& s){return render_all(s.reasons);}
json status_json(const Status& s){json reasons=json::array(),checks=json::array(),refusals=json::object();
    for(const auto& r:s.reasons)reasons.push_back(reason_json(r));for(const auto& c:s.checks)checks.push_back(check_json(c));for(const auto& [k,v]:s.refusals)refusals[k]=reason_json(v);
    return {{"schema","overglaze-game-status-v2"},{"id",s.entry.id},{"title",s.entry.title},{"exe",text(s.entry.exe)},{"state",s.state},{"detail",describe(s)},{"package",s.package},{"route",s.route},
    {"track",s.track},{"installed",s.installed},{"can_install",s.can_install},{"can_uninstall",s.can_uninstall},{"can_make_package",s.can_make_package},{"can_refresh_package",s.can_refresh_package},
    {"running",s.running},{"update_available",s.update_available},{"host_sha256",s.host_hash},{"preflight",s.preflight},
    {"axes",{{"compatibility",s.compatibility},{"install",s.install_state},{"running",s.running?"running":"not-running"}}},
    {"load_mode",s.load_mode.empty()?json(nullptr):json(s.load_mode)},
    {"update",{{"available",s.update_available},{"host",s.update.host},{"bridge",s.update.bridge},{"config",s.update.config},{"proxy",s.update.proxy},{"package_behind_published",s.update.package_behind_published}}},
    {"health",s.health},{"can_update",s.can_update},{"can_repin",s.can_repin},
    {"risks",s.risks},{"anticheat",s.anticheat},{"store",s.store},
    {"launch",s.launch_via.empty()?json(nullptr):json{{"via",s.launch_via},{"command",s.launch_command}}},
    {"reasons",reasons},{"checks",checks},{"refusals",refusals},{"presentation",presentation_json(present(s))}};}
Policy Manager::write_package(const fs::path& exe,const std::string& name,const profiles::Facts& facts_in,const std::string& title,const Preflight& pre,json notes,bool replace,const std::string& consent,const std::string& track,const Loader& loader){
    need(plain_name(name),"适配包名必须是小写字母/数字开头的简单标识符");const auto pkg=root_/L"app"/L"adapters"/wide(name);
    need(replace||!fs::exists(pkg),"适配包目录已存在；不覆盖（刷新载荷请用 refresh-package）");
    need(track=="controller"||track=="research","适配包的 track 只能是 controller 或 research");
    // Before anything is written: a research package's V3 config can only say
    // no_anticheat:true (see the config below).
    need(track=="controller"||pre.anticheat_markers.empty(),"研究轨配置（V3）只能写 no_anticheat:true，与检测到的反作弊文件不符；请改用控制器加载方式（--root-proxy 或 --late）");
    // The host comes from this package's OWN track. Taking it from a fixed
    // directory is how a research game would end up carrying the controller host.
    const auto plugin=track=="controller"?root_/L"app"/L"plugin":root_/L"app"/L"research"/L"host";
    const auto tools=root_/L"app"/L"tools",models=models_dir();
    for(const auto& src:{plugin/L"overglaze_nvngx.dll",tools/L"overglaze_install_check.exe"})need(fs::is_regular_file(src),"已发布的宿主/工具缺失: "+text(src));
    {const auto m=model_status();
     if(!m.present)throw Refusal("model-missing","找不到 NR 模型 nvngx_dlssnr.dll：本项目不包含、也不分发它，请自行准备并放进 "+text(models),json{{"path",text(m.path)}});
     if(!m.known)throw Refusal("model-unknown-version","这份 nvngx_dlssnr.dll 不是已审阅的版本，不使用",json{{"hash",m.sha256}});}
    // The loader this package will carry, taken from the published track under
    // its own name. A late package ships overglaze_controller.dll, not dxgi.dll.
    const auto loader_source=plugin/wide(loader.basename);
    need(fs::is_regular_file(loader_source),"已发布的加载器缺失: "+text(loader_source));
    // The root-proxy strategy ships one extra file, the thin dxgi.dll that
    // the game itself loads. It is payload like any other: hashed here,
    // copied into the package, and verified on install.
    const auto proxy_name=loader.proxy_basename();
    std::string proxy_hash;
    if(!proxy_name.empty()){const auto proxy_source=plugin/wide(proxy_name);
        need(fs::is_regular_file(proxy_source),"已发布的根代理缺失: "+text(proxy_source));
        proxy_hash=digest(proxy_source);} // copied below: a replace pass clears pkg first
    const auto host=digest(loader_source),bridge=digest(plugin/L"overglaze_nvngx.dll"),checker=digest(tools/L"overglaze_install_check.exe"),model=digest(models/L"nvngx_dlssnr.dll");
    need(model_sha256_.empty()?lab::model::known(model)!=nullptr:model==model_sha256_,"模型在检查后改变，或不是已审阅的版本");
    auto facts=facts_in;facts.package=name;
    // The package carries the route the preflight actually measured. A fixed
    // "sl-rr" would be wrong for every NGX-direct title.
    facts.route=pre.route;facts.title=title;facts.executable=text(exe.filename());facts.executable_sha256=pre.executable_sha256;
    // The installation contract derives the capture origin from the route's
    // stage (rr for sl-rr/ngx-rr, sr otherwise). Writing "-rr-stage" for every
    // package would get any SR-route package -- ngx-sr included -- refused at
    // install.
    const std::string stage=(pre.route=="sl-rr"||pre.route=="ngx-rr")?"rr":"sr";
    if(facts.settings_file.empty())facts.settings_file="overlay-"+name+".json";if(facts.capture_origin.empty())facts.capture_origin=name+"-controlled-"+stage+"-stage";
    need(facts.settings_file=="overlay-"+name+".json"&&facts.capture_origin==name+"-controlled-"+stage+"-stage","包名与编译期行的偏好文件/采集来源不一致");
    // Which modules this route actually depends on. A game whose DLSS runs on
    // NGX has no sl.dlss_d.dll to pin, and demanding one there would be asking
    // for evidence that cannot exist rather than evidence we need.
    facts.modules.clear();
    const bool ngx_route=pre.route.rfind("ngx",0)==0;
    // A Streamline route pins the upscaler plugin it drives: dlss_d for RR,
    // dlss for SR.
    const char* sl_model=pre.route=="sl-sr"?"sl.dlss.dll":"sl.dlss_d.dll";
    for(const auto* n:ngx_route?std::initializer_list<const char*>{"nvngx_dlss.dll","nvngx_dlssd.dll","sl.interposer.dll"}
                              :std::initializer_list<const char*>{"sl.interposer.dll","sl.common.dll",sl_model})
        if(pre.modules.contains(n))facts.modules[n]=pre.modules.at(n).at("sha256").get<std::string>();
    // Each NGX route pins the model it actually drives: ray reconstruction
    // for ngx-rr, DLSS super resolution for ngx-sr. Requiring dlssd for both
    // made every pure-SR game unpackable by construction.
    if(ngx_route){const char* model=pre.route=="ngx-rr"?"nvngx_dlssd.dll":"nvngx_dlss.dll";
        need(facts.modules.contains(model),std::string("预检未找到 ")+model+"；该 NGX 路线需要这个模型");}
    else need(facts.modules.contains("sl.interposer.dll")&&facts.modules.contains(sl_model),std::string("预检未找到 sl.interposer.dll + ")+sl_model);
    directory(root_/L"app"/L"adapters");if(replace){for(const auto& n:{L"dxgi.dll",L"overglaze_controller.dll",L"overglaze_reframework.dll",L"overglaze_nvngx.dll",L"overglaze_install_check.exe",L"overglaze.install.json",
        // a package rewritten from its pre-rename version drops its old files
        L"dlsslab_controller.dll",L"dlsslab_reframework.dll",L"dlsslab_nvngx.dll",L"dlsslab_install_check.exe",L"dlsslab.install.json"})if(fs::exists(pkg/n)){winpath::require_no_reparse(pkg/n);fs::remove(pkg/n);}}else directory(pkg);
    copy_new(loader_source,pkg/wide(loader.basename),host);copy_new(plugin/L"overglaze_nvngx.dll",pkg/L"overglaze_nvngx.dll",bridge);copy_new(tools/L"overglaze_install_check.exe",pkg/L"overglaze_install_check.exe",checker);
    if(!proxy_name.empty())copy_new(plugin/wide(proxy_name),pkg/wide(proxy_name),proxy_hash);
    // exception_diagnostics is false. The controller host never reads it; the
    // research host does, and true there installs its first-chance vectored
    // exception observer, a risk next to anti-tamper code that raises its own
    // deliberate exceptions (RE9). Packages generated or refreshed here carry
    // false; no existing package is rewritten for it.
    //
    // Config version 4 (controller track): the user's risk acknowledgement and
    // the facts the checks found replace V3's offline / no-anti-cheat
    // declaration, so a game with anti-cheat is never recorded as having none.
    // Every install of it needs that acknowledgement (install_impl), and its
    // transaction records it. The research track's published host comes from
    // the research tools and reads V1-V3 only, so a research package keeps V3
    // -- and is refused for a game with anti-cheat files, where V3's
    // no_anticheat:true would be false.
    const bool v4=track=="controller";
    json config=v4
        ?json{{"version",4},{"profile",facts.id},{"game_sha256",pre.executable_sha256},{"host_sha256",host},{"bridge_sha256",bridge},
            {"output_root",text(root_/L"data")},{"in_game_controls",true},{"exception_diagnostics",false},{"package",name},{"facts",facts_json(facts)},
            {"risk",{{"acknowledged",true},{"anti_tamper",pre.denuvo_suspected},{"anticheat",pre.anticheat}}}}
        :json{{"version",3},{"profile",facts.id},{"offline_single_player",true},{"no_anticheat",true},{"game_sha256",pre.executable_sha256},{"host_sha256",host},{"bridge_sha256",bridge},
            {"output_root",text(root_/L"data")},{"in_game_controls",true},{"exception_diagnostics",false},{"package",name},{"facts",facts_json(facts)}};
    // Only stated when it is not the original layout, so a root package stays the
    // 12-key V3 config every existing installation checker already accepts.
    if(!loader.root())config["loader"]={{"strategy",loader.strategy},{"basename",loader.basename},{"subdir",text(loader.subdir)}};
    save(pkg/L"overglaze.install.json",config);const auto config_sha=digest(pkg/L"overglaze.install.json");
    json pins=json::object();pins[facts.executable]=pre.executable_sha256;for(const auto& [k,v]:facts.modules)pins[k]=v;
    json manifest={{"schema","overglaze-adapter-package-v1"},{"name",name},{"title",title},{"profile",facts.id},{"route",facts.route},{"track",track},{"game_root",text(exe.parent_path())},
        {"executable",facts.executable},{"process_name",text(exe.stem())},{"pins",pins},
        {"payload",proxy_name.empty()?json{{loader.basename,host},{"overglaze_nvngx.dll",bridge},{"nvngx_dlssnr.dll",model}}
                                    :json{{loader.basename,host},{"overglaze_nvngx.dll",bridge},{"nvngx_dlssnr.dll",model},{proxy_name,proxy_hash}}},
        {"loader",{{"strategy",loader.strategy},{"basename",loader.basename},{"subdir",text(loader.subdir)}}},
        {"checker_sha256",checker},{"config_sha256",config_sha},{"consent",consent.empty()?std::string("recorded per install in the transaction receipt; a manifest field is never an authorization"):consent},
        {"facts",facts_json(facts)},{"reviewed_row",facts.reviewed},{"preflight",pre.to_json()},{"generated_at",now_utc()},{"generator","overglaze-games native controller"},{"notes",std::move(notes)}};
    save(pkg/L"package.json",manifest);
    policies_=load_packages(root_,&package_notes_);for(const auto& p:policies_)if(p.name==name)return p;throw std::runtime_error("适配包写入后无法重新加载");}
Policy Manager::make_package(const std::string& id,const PackageOptions& opt){
    Writer lock(store_);const auto e=find(id);const auto exe=local(e.exe);const auto pre=games::preflight(exe);
    // A package writes only Overglaze's own folder, so anti-tamper and
    // anti-cheat do not stop it: they are recorded (notes, config) and the user
    // acknowledges them at install. The package is the same shape as any other,
    // and Overglaze never patches, spoofs, debugs or dumps a protection.
    // There is no observation-only override for an "unsupported-route" verdict:
    // every DLSS route the preflight classifies is admitting, so no verdict could
    // reach it.
    need(admitting_verdict(pre.verdict),"预检未通过（"+pre.verdict+"）；不生成适配包");
    need(pre.modules_signed||opt.allow_unsigned_modules,"Streamline 模块签名未通过校验；如确认为测试夹具请显式允许");
    const auto* row=profiles::executable(text(exe.filename()),pre.executable_sha256);
    std::string name=!opt.name.empty()?opt.name:row?package_from_settings(row->settings_file):derive_package_name(exe);
    profiles::Facts facts;json notes=json::array();
    if(pre.verdict=="ngx-rr-ready"||pre.verdict=="ngx-sr-ready"){
        notes.push_back("NGX 直连准入：有光线重构时接 RR，没有时接 DLSS 超分；每次 Evaluate 返回后，"
            "NR 录在同一张命令表上、紧接其后。NR 默认关闭。");
        // The create-time rule only bites when we arrive late; say which one applies.
        notes.push_back(opt.loader.late_host()
            ?"晚加载：会错过游戏启动时的 CreateFeature，而深度方向只在创建参数里——注入后需切一次 DLSS 档位，之前每帧都是有名字的跳帧。"
            :"根目录代理：游戏启动时的 CreateFeature 会被看到，不需要切档。");}
    if(!pre.risks().empty())notes.push_back(risk_note(pre));
    if(row){facts=profiles::Facts::from(*row);notes.push_back("facts taken from the reviewed compiled row "+std::string(row->id)+"; options ignored");}
    else{facts.id=name+"-rr-v1";facts.viewport=opt.viewport;facts.linear_depth=opt.linear_depth;facts.native_evaluate_host_rebind=opt.native_evaluate_host_rebind;
        facts.binding_preservation=opt.binding_preservation;facts.default_exposure_stops=opt.default_exposure_stops;
        // The controller host self-configures the Streamline route:
        // viewport and depth semantic come from the game's own RR calls, so on
        // that track the two package values are read by the research host only.
        notes.push_back(!opt.loader.root()&&pre.route.rfind("sl-",0)==0
            ?"控制器宿主在运行时从游戏自己的 RR 调用读出 viewport 与深度类型（自配置）；包里这两项只供研究宿主使用。NR 默认关闭"
            :"default-hypothesis facts (viewport/depth/rebind/preservation/exposure) until the first observation launch corrects them; NR defaults OFF");}
    const auto title=!opt.title.empty()?opt.title:e.title;
    // A root package made here is research-track: nothing moves onto the
    // controller host by default. A LATE package is the
    // controller by definition -- overglaze_controller.dll is the controller host
    // -- so asking for late loading is the explicit decision that moves a game to
    // the controller track, and the loader is then taken from app/plugin.
    const auto track=opt.loader.root()?std::string("research"):std::string("controller");
    return write_package(exe,name,facts,title,pre,std::move(notes),false,{},track,opt.loader);}
Policy Manager::refresh_package(const std::string& name,const std::string& strategy){
    Writer lock(store_);const Policy* p=nullptr;for(const auto& c:policies_)if(c.name==name)p=&c;need(p!=nullptr,"未找到该适配包");need(!p->game_root.empty(),"适配包无 game_root，无法重新预检");
    // Changing the strategy is a deliberate act, so it is spelled out rather
    // than inferred, and only between the two controller strategies: a package
    // on the research root layout belongs to the research tools and is never
    // moved by this tool.
    Loader loader=p->loader;
    if(!strategy.empty()){
        need(strategy=="root_proxy_d3d12"||strategy=="root_proxy_on_insert"||strategy=="late_d3d12","只能在根代理、按 Insert 加载的根代理与晚加载之间切换");
        need(!p->loader.root(),"研究轨的根布局适配包不由本工具改写策略");
        loader.strategy=strategy;loader.basename="overglaze_controller.dll";loader.subdir="overglaze";
    }
    // The in-game host trusts the installed files only through this very
    // manifest, so rewriting it under an installed game leaves a game whose Lab
    // refuses to start ("package refreshed, game not updated"). An
    // installed game is updated by update(), which uninstalls, refreshes and
    // installs in one operation.
    if(lab_files_present(*p))throw Refusal("refresh-would-break-installed","这款游戏已装插件：单独刷新适配包会让已装文件失效（游戏内插件启动时会拒绝）；请用 update，它依次卸载、刷新、安装");
    const auto exe=local(p->game_root/wide(p->executable));const auto pre=games::preflight(exe);
    // The EXE identity check is never relaxed. One function decides it, for
    // this call and for update()'s check before it uninstalls anything.
    // Anti-tamper and anti-cheat are not part of it (risks, acknowledged at install).
    if(const auto why=refresh_refusal(*p,pre);!why.code.empty()){
        if(why.params.value("verdict","").rfind("module ",0)==0)throw Refusal(why.code,"游戏模块身份与适配包不同: "+why.params.value("verdict","").substr(7),why.params);
        throw Refusal(why.code,"游戏预检失败或 EXE 身份与适配包不同；刷新只更新 Lab 载荷",why.params);}
    auto facts=p->facts;json notes=json::array();std::string consent=p->consent;try{auto m=read(p->directory/L"package.json");if(m.contains("notes")&&m.at("notes").is_array())notes=m.at("notes");}catch(...){}
    const auto track=p->track;
    const auto published=published_host(track);notes.push_back("refreshed "+now_utc()+" ("+track+" track) to published host "+published.at("dxgi.dll").substr(0,16)+"… / bridge "+published.at("overglaze_nvngx.dll").substr(0,16)+"…");
    // Refreshing never changes how a game is loaded: the package keeps its own
    // strategy, and only its payload is re-copied.
    return write_package(exe,name,facts,p->title,pre,std::move(notes),true,consent,track,loader);}
const std::vector<std::string>& operation_stages(std::string_view operation){
    if(operation=="install")return kInstallStages;if(operation=="uninstall")return kUninstallStages;
    if(operation=="update")return kUpdateStages;if(operation=="repin")return kRepinStages;return kNoStages;}
json progress_json(const ProgressEvent& e){return {{"event","progress"},{"operation",e.operation},{"stage",e.stage},{"status",e.status},
    {"parent",e.parent.empty()?json(nullptr):json(e.parent)},{"index",e.index},{"count",e.count},{"params",e.params}};}
json operation_error_json(const OperationError& e){return {{"ok",false},{"error",e.what()},{"operation",e.operation},{"stage",e.stage},
    {"install_after",e.install_after},{"reason",reason_json(e.reason)},{"text",render(e.reason)}};}
namespace {
// Where a failed operation actually left the game, measured rather than
// assumed (an update that failed after its uninstall is "not-installed").
std::string measured_install_state(const Manager& m,const std::string& id){
    try{for(const auto& e:m.list())if(e.id==id)return m.inspect(e).install_state;}catch(...){}return "unknown";}
std::string recovery_of(const fs::path& tx){try{return read(tx).value("recovery","");}catch(...){return {};}}
}
// The public install/uninstall report a failure as an OperationError carrying
// the stage it stopped in and the install state it left, measured afterwards.
// what() is the underlying message, so callers that only show e.what() see
// exactly what they saw before.
namespace {
template<class F> void reported(const Manager& m,const std::string& operation,const std::string& id,const Progress& progress,F&& run){
    std::string failed;
    const Progress spy=[&](const ProgressEvent& ev){if(ev.status=="failed"&&ev.operation==operation&&ev.parent.empty())failed=ev.stage;if(progress)progress(ev);};
    try{run(spy);}
    catch(const OperationError&){throw;}
    catch(const std::exception& ex){const auto after=measured_install_state(m,id);
        throw OperationError(operation,failed,after,Reason{"operation-failed",{{"operation",operation},{"stage",failed},{"message",ex.what()}}},ex.what());}}
}
void Manager::install(const std::string& id,bool risk_accepted,const std::string& consent,const Progress& progress){
    reported(*this,"install",id,progress,[&](const Progress& spy){install_impl(id,risk_accepted,consent,spy,{});});}
void Manager::install_impl(const std::string& id,bool risk_accepted,const std::string& consent,const Progress& progress,const std::string& parent){
    Steps steps(progress,"install",parent);try{
    steps.enter("check");
    // The user's own risk acknowledgement, for every game: the checks can name
    // what they found but never prove a game free of anti-cheat or online play.
    need(risk_accepted,"安装前需确认风险：联网或带反作弊的游戏可能无法启动、被踢出或被处罚；Overglaze 不绕过任何保护");need(!consent.empty()&&consent.size()<=1024,"安装需要记录本次确认文字");
    Writer writer(store_);const auto e=find(id);const auto s=inspect(e);need(s.can_install,"当前状态不允许安装；请重新检查并退出游戏");const Policy* p=match(local(e.exe));need(p!=nullptr,"无匹配适配包");
    steps.enter("pin-game");
    const auto dir=e.exe.parent_path();Pins pins;pins.parents(dir);pins.parents(root_/L"app");
    // A package-pinned title's EXE cannot be opened; its identity is re-read
    // from the OS package instead of held through a file handle.
    const bool package_pinned=is_package_token(p->pins.at(p->executable));
    if(package_pinned)need(game_identity_token(e.exe)==p->pins.at(p->executable),"游戏包身份与适配包不同（游戏已更新或换了包）");
    else pins.file(e.exe);
    // A pinned module does not have to sit beside the EXE: a UE title keeps its
    // DLSS plugin under Engine\Plugins. Looking only beside the EXE would make
    // this throw the reparse-point refusal for a file that is simply somewhere
    // else -- the same assumption preflight, the installed package's pin check
    // and route classification avoid too. The file is pinned WHERE IT ACTUALLY
    // IS, so the handle we hold for the transaction is the one we verified.
    const auto module_dirs=module_directories(dir);
    for(auto& [n,h]:p->pins){
        if(package_pinned&&n==p->executable)continue; // verified above through the OS package
        std::error_code ec;fs::path found;
        for(const auto& candidate:module_dirs){
            const auto f=candidate/wide(n);
            if(fs::is_regular_file(f,ec)){found=f;break;} // first place it exists is the one that must match
        }
        need(!found.empty(),"适配包钉住的模块已不在游戏目录中: "+n);
        pins.file(found);need(digest(found)==h,"检查后游戏 EXE/模块发生变化: "+n);}
    steps.enter("space");
    need(!running(e.exe),"游戏仍在运行");need(fs::space(dir).available>kGameDiskMinimumBytes&&fs::space(root_/L"data").available>=kDataDiskReserveBytes,
        "游戏盘需 "+std::to_string(kGameDiskMinimumBytes>>20)+" MiB，Lab 数据盘需保留 "+std::to_string(kDataDiskReserveBytes>>30)+" GiB");
    steps.enter("receipts");
    const auto box=store_/wide(id);directory(box);const auto tx=box/L"transaction.json";if(fs::exists(tx))need(receipt(tx,e).at("state")=="removed","仍有未完成的安装事务");
    // The second receipt, read in both places. The legacy one is only read
    // and is left exactly where and as it is; the manager's own is archived
    // beside it once uninstalled, never replayed.
    const auto legacy_receipt=legacy_receipt_file(root_,p->name);
    if(fs::exists(legacy_receipt))need(read(legacy_receipt).value("state","")=="uninstalled","已有该适配包的安装回执且状态不是 uninstalled；不重放，先检查");
    const auto docs_receipt=own_receipt_file(box);
    if(fs::exists(docs_receipt)){auto old=read(docs_receipt);need(old.value("state","")=="uninstalled","已有该游戏的安装回执且状态不是 uninstalled；不重放，先检查");
        const auto archived=box/wide("install-receipt-uninstalled-"+uuid().substr(1,8)+".json");need(MoveFileExW(docs_receipt.c_str(),archived.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"无法归档旧回执");}
    steps.enter("stage-files");
    const auto staging=box/wide("package-"+uuid());directory(staging);json files=json::object();
    // Every Lab-owned file goes to the loader directory; the game's own files
    // stay where they are. For the root strategy this IS the game directory.
    const auto lab_dir=lab_directory(dir,*p);
    const bool own_subdirectory=!p->loader.subdir.empty();
    bool made_subdirectory=false;
    if(own_subdirectory){
        // One level at a time, refusing a reparse point at each step, and never
        // adopting a directory that already holds files we do not own.
        fs::path walk=dir;
        for(const auto& part:p->loader.subdir){walk/=part;winpath::require_no_reparse(walk.parent_path());
            if(!fs::exists(walk)){need(fs::create_directory(walk),"无法创建载荷子目录: "+text(walk));made_subdirectory=true;}
            winpath::require_no_reparse(walk);need(fs::is_directory(walk),"载荷子目录不是目录: "+text(walk));}
        for(const auto& item:fs::directory_iterator(lab_dir))
            need(owned_name(text(item.path().filename())),"载荷子目录里有不属于 Lab 的文件，不覆盖: "+text(item.path().filename()));
    }
    // Stage and verify first. No target is replaced. Model reuse is not owned.
    std::map<std::string,fs::path> sources{{p->loader.basename,p->directory/wide(p->loader.basename)},{"overglaze_nvngx.dll",p->directory/L"overglaze_nvngx.dll"},{"nvngx_dlssnr.dll",model_file()}};
    // The root proxy is the only payload entry that does NOT go to the
    // payload directory: the game can only load it from beside its own exe.
    const auto proxy_name=p->loader.proxy_basename();
    if(!proxy_name.empty())sources[proxy_name]=p->directory/wide(proxy_name);
    const auto destination=[&](const std::string& n){return n==proxy_name?dir/wide(n):lab_dir/wide(n);};
    for(auto& [n,h]:p->payload){const auto dest=destination(n);if(n=="nvngx_dlssnr.dll"&&fs::exists(dest)){pins.file(dest);need(digest(dest)==h,"已有模型改变");continue;}need(!fs::exists(dest),"目标文件已存在");
        // The model is not staged: its identity is checked here and it is copied
        // straight from app\models at activation, re-checked by handle again.
        if(n==kModelName){Pins check;need(handle_digest(check.file(sources.at(n)))==h,"本地模型身份改变");files[n]=h;continue;}
        copy_new(sources.at(n),staging/wide(n),h);files[n]=h;}
    need(!fs::exists(lab_dir/L"overglaze.install.json"),"目标配置已存在");copy_new(p->directory/L"overglaze.install.json",staging/L"overglaze.install.json",p->config_sha256);files["overglaze.install.json"]=p->config_sha256;
    steps.enter("record-transaction");
    json j={{"schema","overglaze-install-transaction-v1"},{"id",id},{"exe",text(e.exe)},{"state","installing"},{"files",files},{"recovery",text(staging)},{"game_started",false},{"package",p->name},{"consent",consent},
        {"loader_subdirectory",text(p->loader.subdir)},{"loader_basename",p->loader.basename},{"strategy",p->loader.strategy},
        {"risk_acknowledged",true},{"risks",p->risks}};save(tx,j);
    steps.enter("activate");
    // Activate the loader LAST, after its config/bridge/model are durable.
    // The proxy is activated after the host, because it is what the game
    // loads: a proxy in place with no host behind it would be a game that
    // starts and silently has no Lab at all.
    for(const auto& n:{std::string("nvngx_dlssnr.dll"),std::string("overglaze_nvngx.dll"),std::string("overglaze.install.json"),p->loader.basename,proxy_name})
        if(!n.empty()&&files.contains(n)){need(!running(e.exe),"操作中检测到游戏启动；停止，不再复制。退出后可清理未完成安装。");copy_new(n==kModelName?sources.at(n):staging/wide(n),destination(n),files[n]);}
    if(run_checker_){steps.enter("verify");
        // The package's own installation checker (the host's load_installation_file
        // contract incl. the V3 package hash chain) must accept the installed files,
        // exactly as Manage-LabGame.ps1 requires. On refusal, remove what we just
        // copied (by handle, identity re-checked) and fail the transaction.
        const auto checker=p->directory/L"overglaze_install_check.exe";need(digest(checker)==p->checker_sha256,"适配包的安装检查器身份与清单不一致");
        std::wstring command=L"\""+checker.wstring()+L"\" \""+(lab_dir/L"overglaze.install.json").wstring()+L"\" \""+e.exe.wstring()+L"\"";
        // Capture what the checker SAYS, not just whether it refused. An exit
        // code alone turned every distinct contract violation into one opaque
        // message and the rollback then erased the evidence, so the reason had
        // to be guessed. Same defect as collapsing several NR faults into one
        // report; the fix is the same: name it.
        std::string checker_said;DWORD code=1;
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE read_end=nullptr,write_end=nullptr;
        const bool piped=CreatePipe(&read_end,&write_end,&sa,64*1024)!=FALSE;
        if(piped)SetHandleInformation(read_end,HANDLE_FLAG_INHERIT,0);
        STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
        if(piped){si.dwFlags|=STARTF_USESTDHANDLES;si.hStdOutput=si.hStdError=write_end;si.hStdInput=nullptr;}
        if(CreateProcessW(checker.c_str(),command.data(),nullptr,nullptr,piped?TRUE:FALSE,CREATE_NO_WINDOW,nullptr,p->directory.c_str(),&si,&pi)){
            // Close our copy of the write end first, or the read below never
            // sees end-of-file and the 120 s wait becomes a deadlock.
            if(piped){CloseHandle(write_end);write_end=nullptr;}
            if(WaitForSingleObject(pi.hProcess,120000)==WAIT_OBJECT_0)GetExitCodeProcess(pi.hProcess,&code);else{TerminateProcess(pi.hProcess,1);code=2;}
            CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
            if(piped){char buffer[4096];DWORD got=0;
                while(checker_said.size()<16*1024&&ReadFile(read_end,buffer,sizeof(buffer),&got,nullptr)&&got)checker_said.append(buffer,got);}}
        if(write_end)CloseHandle(write_end);
        if(read_end)CloseHandle(read_end);
        while(!checker_said.empty()&&(checker_said.back()=='\n'||checker_said.back()=='\r'))checker_said.pop_back();
        // Undo removes the proxy FIRST and from where it actually is (beside
        // the EXE), then the payload: an undo that looked for every file in the
        // payload directory would leave a refused root-proxy install's thin
        // dxgi.dll in the game directory under a "removed" transaction, and the
        // game could then neither start clean nor be reinstalled.
        if(code!=0){Pins undo;for(const auto& n:{proxy_name,p->loader.basename,std::string("overglaze.install.json"),std::string("overglaze_nvngx.dll"),std::string("nvngx_dlssnr.dll")})if(!n.empty()&&files.contains(n)){const auto f=destination(n);if(!fs::exists(f))continue;auto h=undo.file(f,true);if(handle_digest(h)==files[n].get<std::string>())erase_handle(h);}
            undo.handles.clear();
            if(made_subdirectory){std::error_code ec;if(fs::is_directory(lab_dir,ec)&&fs::is_empty(lab_dir,ec))fs::remove(lab_dir,ec);}
            j["state"]="removed";
            j["error"]="installation checker refused the installed contract (exit "+std::to_string(code)+")";
            j["checker_output"]=checker_said;save(tx,j);
            throw std::runtime_error("安装检查器拒绝了已安装的契约（退出码 "+std::to_string(code)+"）；已按身份移除本次复制的文件。检查器说："+
                (checker_said.empty()?std::string("（无输出）"):checker_said));}
        j["checker_exit"]=0;}
    else steps.skip("verify",json{{"why","checker disabled (synthetic fixture)"}});
    steps.enter("commit");
    j["state"]="installed";save(tx,j);
    json after=json::object();for(const auto& n:{p->loader.basename,std::string("overglaze_nvngx.dll"),std::string("overglaze.install.json"),std::string("nvngx_dlssnr.dll")})after[n]=digest(lab_dir/wide(n));json game_pins=json::object();for(auto& [n,h]:p->pins)game_pins[n]=h;
    save(docs_receipt,{{"version",1},{"state","installed"},{"game_directory",text(dir)},{"profile",p->profile},{"package",p->name},{"adopted",false},{"consent",consent},{"risk_acknowledged",true},{"risks",p->risks},
        {"before","all owned targets absent (model reused only if byte-identical)"},{"after",after},{"game_pins",game_pins},{"nr_default","off"},{"heavy_sampling",false},
        {"game_started_by_installer",false},{"runtime_accepted",false},{"installer","overglaze-games native controller"},{"transaction",text(tx)},{"installed_at",now_utc()}});
    steps.enter("cleanup-staging");
    // Retention: a successful install's staging copy is only a second copy of
    // what is now verified in the game directory, so it goes. A failed install
    // never reaches this line and keeps its staging for inspection. A staging
    // directory that cannot be removed stays; the install has already succeeded.
    json kept=json::object();
    try{remove_made_directory(box,staging,L"package-",files);j["recovery"]=nullptr;j["staging_removed"]=text(staging);}
    catch(const std::exception& ex){j["staging_retained"]=ex.what();kept["staging_retained"]=ex.what();}
    try{save(tx,j);}catch(...){}
    steps.done(kept);
    }catch(const std::exception& ex){steps.fail(ex.what());throw;}
}
void Manager::uninstall(const std::string& id,bool confirmed,const Progress& progress){
    reported(*this,"uninstall",id,progress,[&](const Progress& spy){uninstall_impl(id,confirmed,spy,{});});}
void Manager::uninstall_impl(const std::string& id,bool confirmed,const Progress& progress,const std::string& parent){
    Steps steps(progress,"uninstall",parent);try{
    steps.enter("check");
    need(confirmed,"尚未确认卸载");Writer writer(store_);const auto e=find(id);const auto s=inspect_impl(e,false);need(s.can_uninstall,"无法安全卸载；请退出游戏并重新检查文件身份");
    steps.enter("pin-files");
    Pins pins;pins.parents(e.exe.parent_path());if(!is_package_token(game_identity_token(e.exe)))pins.file(e.exe);need(!running(e.exe),"游戏仍在运行");const auto box=store_/wide(id);directory(box);const auto tx=box/L"transaction.json";json j;
    // Where the files actually are. A transaction written by this controller
    // records the subdirectory; an adopted install predates the field and is root.
    fs::path lab_dir=e.exe.parent_path();std::string loader_name="dxgi.dll",proxy_name;
    if(s.state!="existing"&&fs::exists(tx)){const auto pre=receipt(tx,e);
        const auto sub=pre.value("loader_subdirectory","");if(!sub.empty())lab_dir=(lab_dir/wide(sub)).lexically_normal();
        loader_name=pre.value("loader_basename","dxgi.dll");
        // Written by this controller; absent on installs that predate the field.
        if(proxied(pre.value("strategy","")))proxy_name="dxgi.dll";}
    const auto located=[&](const std::string& n){return n==proxy_name&&!proxy_name.empty()?e.exe.parent_path()/wide(n):lab_dir/wide(n);};
    if(s.state=="existing"){json files=json::object();
        // Adopted root-layout files, under whichever identity is present.
        const bool old=fs::exists(e.exe.parent_path()/identity::legacy::kInstallationFile)&&!fs::exists(e.exe.parent_path()/identity::kInstallationFile);
        for(const auto* n:old?std::initializer_list<const char*>{"dxgi.dll","dlsslab_nvngx.dll","dlsslab.install.json"}:std::initializer_list<const char*>{"dxgi.dll","overglaze_nvngx.dll","overglaze.install.json"})files[n]=digest(e.exe.parent_path()/wide(n));j={{"schema","overglaze-install-transaction-v1"},{"id",id},{"exe",text(e.exe)},{"state","installed"},{"files",files},{"adopted",true}};}
    else j=receipt(tx,e);
    std::map<std::string,HANDLE> targets;for(auto it=j["files"].begin();it!=j["files"].end();++it){const auto f=located(it.key());if(!fs::exists(f))continue;auto h=pins.file(f,true);need(handle_digest(h)==it.value().get<std::string>(),"卸载前文件发生变化，停止");targets[it.key()]=h;}
    steps.enter("space");
    // Uninstall writes only the recovery copy of what it removes: it needs that
    // copy's size plus a margin, not the 30 GiB install reserve. Checked
    // before the directory exists, so a refusal leaves nothing behind.
    std::uint64_t copy_bytes=0;json copied=json::object(),shared=json::object();
    for(auto& [n,h]:targets){const auto sha=j["files"][n].get<std::string>();
        if(n==kModelName){shared[n]=sha;if(!fs::exists(shared_model_file(store_,sha)))copy_bytes+=file_bytes(h);continue;}
        copy_bytes+=file_bytes(h);copied[n]=sha;}
    need(fs::space(root_/L"data").available>=recovery_space_needed(copy_bytes),"恢复副本需要数据盘至少 "+std::to_string((recovery_space_needed(copy_bytes)+(1ULL<<20)-1)>>20)+" MiB 可用空间");
    steps.enter("recovery-copy");
    const auto recovery=box/wide("uninstall-"+uuid());directory(recovery);
    for(auto& [n,h]:targets){if(shared.contains(n)){keep_shared_model(store_,h,shared[n].get<std::string>());continue;}copy_handle(h,recovery/wide(n),j["files"][n]);}
    if(!shared.empty()){json where=json::object();for(auto it=shared.begin();it!=shared.end();++it)where[it.key()]={{"sha256",it.value()},{"path",text(shared_model_file(store_,it.value().get<std::string>()))}};j["recovery_shared"]=where;}
    steps.enter("record-transaction");
    j["state"]="removing";j["recovery"]=text(recovery);save(tx,j);need(!running(e.exe),"游戏启动，保留恢复副本并停止卸载");
    steps.enter("remove-files");
    // By-handle deletion prevents a path swap after the identity check. No
    // directory recursion, game assets, other mods or captures are removed.
    // The proxy goes FIRST: once it is gone the game loads nothing of ours,
    // so a half-finished uninstall can never leave a live loader behind.
    // Every file the record names goes, under the names it was installed with
    // (a pre-rename transaction names dlsslab_*): proxy, loader, then the rest.
    for(const auto& n:removal_order(j["files"],proxy_name,loader_name))
        if(targets.contains(n))erase_handle(targets.at(n));
    pins.handles.clear();
    steps.enter("commit");
    j["state"]="removed";save(tx,j);
    // A subdirectory we created is removed only when it is empty: never recursively.
    if(lab_dir!=e.exe.parent_path()){std::error_code ec;if(fs::is_directory(lab_dir,ec)&&fs::is_empty(lab_dir,ec))fs::remove(lab_dir,ec);}
    // Keep both second receipts truthful: the manager's own, and the research
    // tools' legacy one inside the program root that still says "installed" for
    // this game (see "second receipt" above).
    mark_receipt_uninstalled(own_receipt_file(box),e.exe.parent_path(),recovery);
    if(!s.package.empty())mark_receipt_uninstalled(legacy_receipt_file(root_,s.package),e.exe.parent_path(),recovery);
    steps.enter("retention");
    // Retention: this copy joins the ledger and the oldest ledger copies beyond
    // the newest kRecoveryCopiesKept are deleted. Copies made before the rule
    // are not in the ledger and stay until the owner decides what to do with
    // them. Only a SUCCESSFUL uninstall gets
    // here; a failed one keeps its copy unmanaged, for inspection.
    retain_recovery(box,recovery,copied);
    steps.done(json{{"recovery",text(recovery)}});
    }catch(const std::exception& ex){steps.fail(ex.what());throw;}
}
void Manager::update(const std::string& id,bool risk_accepted,const std::string& consent,const Progress& progress){
    Steps steps(progress,"update",{});bool uninstalled=false;std::string recovery;
    try{
        steps.enter("check");
        // The install's own confirmations are checked BEFORE anything is
        // uninstalled; an update that would refuse at its install step must not
        // first take the working plugin away.
        need(risk_accepted,"安装前需确认风险：联网或带反作弊的游戏可能无法启动、被踢出或被处罚；Overglaze 不绕过任何保护");need(!consent.empty()&&consent.size()<=1024,"安装需要记录本次确认文字");
        const auto e=find(id);const auto s=inspect(e);
        need(s.state=="installed"&&s.update_available&&!s.running,"没有可用更新，或游戏正在运行／文件身份不符");
        std::string package;{const Policy* p=match(local(e.exe));need(p!=nullptr,"无匹配适配包");package=p->name;}
        const bool refresh=s.update.package_behind_published;
        if(refresh){steps.enter("refresh-check");
            // The same gates refresh_package() applies, run before the uninstall
            // so that a refusal leaves the working install exactly as it was.
            const Policy* p=match(local(e.exe));need(p!=nullptr,"无匹配适配包");
            const auto why=refresh_refusal(*p,games::preflight(local(e.exe)));
            if(!why.code.empty())throw Refusal(why.code,render(why),why.params);}
        else steps.skip("refresh-check");
        // Uninstall first: its recovery copy and receipt are the rollback. With
        // the game's Lab files gone, the package can be refreshed without ever
        // leaving "package refreshed, game not updated" behind.
        steps.enter("uninstall");uninstall_impl(id,true,progress,"update");uninstalled=true;recovery=recovery_of(store_/wide(id)/L"transaction.json");
        if(refresh){steps.enter("refresh-package");refresh_package(package);}else steps.skip("refresh-package");
        steps.enter("install");install_impl(id,risk_accepted,consent,progress,"update");
        steps.done();
    }catch(const std::exception& ex){
        const auto stage=steps.where().empty()?std::string("check"):steps.where();steps.fail(ex.what());
        const auto after=measured_install_state(*this,id);
        Reason why=uninstalled&&after=="not-installed"
            ?Reason{"update-failed-uninstalled",{{"operation","update"},{"stage",stage},{"recovery",recovery},{"message",ex.what()}}}
            :Reason{"operation-failed",{{"operation","update"},{"stage",stage},{"message",ex.what()}}};
        throw OperationError("update",stage,after,std::move(why),ex.what());}
}
fs::path Manager::retire_package(const Policy& p){
    const auto adapters=root_/L"app"/L"adapters";
    need(plain_name(p.name)&&winpath::same_spelling(p.directory,adapters/wide(p.name)),"只退役 app/adapters 下本工具认得的适配包");
    winpath::require_no_reparse(p.directory);need(fs::is_directory(p.directory),"适配包目录不存在");
    const auto retired=root_/L"app"/L"adapters-retired";directory(retired);
    SYSTEMTIME t{};GetSystemTime(&t);char stamp[32];std::snprintf(stamp,sizeof(stamp),"%04u%02u%02uT%02u%02u%02uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    auto target=retired/wide(p.name+"-repin-"+stamp);
    if(fs::exists(target))target=retired/wide(p.name+"-repin-"+stamp+"-"+uuid().substr(1,8));
    need(!fs::exists(target),"退役目录已存在；不覆盖");
    // A move within the same volume: the old package stays byte-for-byte, only
    // its directory name and parent change. Nothing is deleted.
    need(MoveFileExW(p.directory.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"无法把旧适配包移到 adapters-retired（文件占用或权限不足）；未继续");
    policies_=load_packages(root_,&package_notes_);
    return target;}
fs::path Manager::repin(const std::string& id,bool risk_accepted,const std::string& consent,bool allow_unsigned_modules,const Progress& progress){
    Steps steps(progress,"repin",{});bool uninstalled=false;std::string recovery;fs::path retired;bool package_made=false;
    try{
        steps.enter("check");
        need(risk_accepted,"安装前需确认风险：联网或带反作弊的游戏可能无法启动、被踢出或被处罚；Overglaze 不绕过任何保护");need(!consent.empty()&&consent.size()<=1024,"安装需要记录本次确认文字");
        const auto e=find(id);const auto exe=local(e.exe);const auto s=inspect(e);
        need(s.state=="changed","游戏版本没有变化，不需要重新适配");need(!s.running,"游戏仍在运行");
        Policy old;{const Policy* p=match(exe);need(p!=nullptr,"无匹配适配包；请先生成适配包");old=*p;}
        // Files of ours with no transaction of ours (another tool's install)
        // are never adopted by a repin.
        if(!s.installed&&lab_files_present(old))throw Refusal("repin-foreign-install","游戏目录里有不经本管理器安装的 Lab 文件；先用安装它的工具卸载");
        steps.enter("preflight");
        Preflight pre;
        if(const auto why=repin_refusal(old,exe,allow_unsigned_modules,&pre);!why.code.empty())throw Refusal(why.code,render(why),why.params);
        if(s.installed||s.can_uninstall){steps.enter("uninstall");uninstall_impl(id,true,progress,"repin");uninstalled=true;recovery=recovery_of(store_/wide(id)/L"transaction.json");}
        else steps.skip("uninstall");
        {Writer lock(store_);
         steps.enter("retire-package");retired=retire_package(old);
         steps.enter("make-package");
         // The old package's facts are the game's (viewport, depth, binding
         // preservation, exposure); only the pins, the preflight and the payload
         // are new. Same name, same profile id, same loader, same consent text.
         json notes=json::array();try{auto m=read(retired/L"package.json");if(m.contains("notes")&&m.at("notes").is_array())notes=m.at("notes");}catch(...){}
         notes.push_back("repinned "+now_utc()+": "+old.executable+" "+old.pins.at(old.executable).substr(0,16)+"… -> "+pre.executable_sha256.substr(0,16)+
             "…; full preflight re-run; previous package moved to app/adapters-retired/"+text(retired.filename()));
         if(!pre.risks().empty())notes.push_back(risk_note(pre));
         write_package(exe,old.name,old.facts,old.title,pre,std::move(notes),false,old.consent,"controller",old.loader);package_made=true;}
        steps.enter("install");install_impl(id,risk_accepted,consent,progress,"repin");
        steps.done(json{{"retired",text(retired)}});
        return retired;
    }catch(const std::exception& ex){
        const auto stage=steps.where().empty()?std::string("check"):steps.where();steps.fail(ex.what());
        const auto after=measured_install_state(*this,id);
        Reason why=!retired.empty()&&!package_made
            ?Reason{"repin-failed-retired",{{"operation","repin"},{"stage",stage},{"retired",text(retired)},{"message",ex.what()}}}
            :uninstalled&&after=="not-installed"
            ?Reason{"repin-failed-uninstalled",{{"operation","repin"},{"stage",stage},{"recovery",recovery},{"message",ex.what()}}}
            :Reason{"operation-failed",{{"operation","repin"},{"stage",stage},{"message",ex.what()}}};
        throw OperationError("repin",stage,after,std::move(why),ex.what());}
}
json Manager::plan_install(const std::string& id)const{
    const auto e=find(id);const auto s=inspect(e);
    json reasons=json::array();for(const auto& r:s.reasons)reasons.push_back(reason_json(r));
    json out={{"schema","overglaze-install-plan-v1"},{"id",id},{"title",e.title},{"exe",text(e.exe)},{"state",s.state},{"install",s.install_state},
        {"can_install",s.can_install},{"reasons",reasons},{"stages",kInstallStages},{"checker_runs",run_checker_},
        {"scope","read-only dry run of install: nothing is copied, created, locked or recorded; install re-checks every hash by handle"}};
    const Policy* p=nullptr;try{p=match(local(e.exe));}catch(...){}
    if(!p){out["package"]=nullptr;out["files"]=json::array();return out;}
    const auto dir=e.exe.parent_path();const auto lab_dir=lab_directory(dir,*p);const auto proxy_name=p->loader.proxy_basename();
    std::map<std::string,fs::path> sources{{p->loader.basename,p->directory/wide(p->loader.basename)},{"overglaze_nvngx.dll",p->directory/L"overglaze_nvngx.dll"},{"nvngx_dlssnr.dll",model_file()}};
    if(!proxy_name.empty())sources[proxy_name]=p->directory/wide(proxy_name);
    const auto destination=[&](const std::string& n){return n==proxy_name?dir/wide(n):lab_dir/wide(n);};
    json files=json::array();std::uint64_t to_game=0,to_staging=0;bool conflict=false;
    auto add=[&](const std::string& n,const fs::path& from,const std::string& hash){std::error_code ec;const auto dest=destination(n);
        const auto size=fs::file_size(from,ec);json bytes=ec?json(nullptr):json(static_cast<std::uint64_t>(size));
        std::string action="copy";
        if(fs::exists(dest,ec)){if(n=="nvngx_dlssnr.dll"){std::string have;try{have=digest(dest);}catch(...){}action=have==hash?"reuse-existing":"conflict";}else action="conflict";}
        if(action=="conflict")conflict=true;
        if(action=="copy"&&!ec){to_game+=size;if(n!=kModelName)to_staging+=size;}
        files.push_back({{"name",n},{"source",text(from)},{"destination",text(dest)},{"sha256",hash},{"bytes",bytes},{"action",action}});};
    for(const auto& [n,h]:p->payload)if(sources.contains(n))add(n,sources.at(n),h);
    add("overglaze.install.json",p->directory/L"overglaze.install.json",p->config_sha256);
    const auto box=store_/wide(id);
    std::error_code ge,de;const auto game_space=fs::space(dir,ge);const auto data_space=fs::space(root_/L"data",de);
    json created=json::array();if(!p->loader.subdir.empty()){std::error_code ec;if(!fs::exists(lab_dir,ec))created.push_back(text(lab_dir));}
    out["package"]=p->name;out["load_mode"]=p->loader.strategy;out["track"]=p->track;
    out["launch"]=reason_json(launch_reason(p->loader.strategy,text(p->loader.subdir),root_,store_kind(dir,is_package_token(p->pins.at(p->executable)))));
    out["files"]=files;out["conflicts"]=conflict;out["directories_created"]=created;
    out["records"]={{"transaction",text(box/L"transaction.json")},{"receipt",text(own_receipt_file(box))},{"staging",text(box/L"package-{GUID}")},
        {"legacy_receipt_read_only",text(legacy_receipt_file(root_,p->name))}};
    out["space"]={
        {"game_disk",{{"path",text(dir)},{"available",ge?json(nullptr):json(static_cast<std::uint64_t>(game_space.available))},{"required_free",kGameDiskMinimumBytes},{"writes",to_game},
            {"ok",ge?json(nullptr):json(game_space.available>kGameDiskMinimumBytes)}}},
        {"data_disk",{{"path",text(root_/L"data")},{"available",de?json(nullptr):json(static_cast<std::uint64_t>(data_space.available))},{"required_free",kDataDiskReserveBytes},{"writes",to_staging},
            {"ok",de?json(nullptr):json(data_space.available>=kDataDiskReserveBytes)},{"note","the staging copy is removed after a successful install"}}}};
    out["checker"]={{"path",text(p->directory/L"overglaze_install_check.exe")},{"sha256",p->checker_sha256}};
    return out;}
void Manager::forget(const std::string& id){Writer writer(store_);const auto e=find(id);const auto dir=e.exe.parent_path();const auto tx=store_/wide(id)/L"transaction.json";if(fs::exists(tx))need(receipt(tx,e).at("state")=="removed","请先卸载或清理未完成安装");
    for(const auto* n:{L"dxgi.dll",L"overglaze.install.json",L"overglaze_nvngx.dll",L"dlsslab.install.json",L"dlsslab_nvngx.dll"})need(!fs::exists(dir/n),"路径仍有插件文件，请先处理；不把移除记录当卸载");auto j=registry(store_);auto& items=j["entries"];for(auto it=items.begin();it!=items.end();++it)if(it->at("id")==id){items.erase(it);break;}save(store_/L"games.json",j);}
StorageUsage Manager::storage_usage()const{StorageUsage u;
    {std::error_code ec;const auto space=fs::space(root_/L"data",ec);if(ec)u.complete=false;else u.data_disk_available=space.available;}
    unsigned visited=0;constexpr unsigned kBound=65536;
    // Only the entry kinds the manager itself creates are classified; nothing is
    // followed through a reparse point and nothing is opened for writing.
    auto plain=[](const fs::path& p,bool directory){const auto a=GetFileAttributesW(p.c_str());
        return a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_REPARSE_POINT)&&((a&FILE_ATTRIBUTE_DIRECTORY)!=0)==directory;};
    std::error_code outer;
    for(fs::directory_iterator it(store_,outer),end;!outer&&it!=end;it.increment(outer)){
        const auto box=it->path();const auto id=text(box.filename());
        if(!id_ok(id))continue; // games.json, writer.lock
        StorageUsage::Game g;g.id=id;
        if(!plain(box,true)){g.complete=false;u.complete=false;u.games.push_back(std::move(g));continue;}
        std::set<std::string> managed;
        try{const auto ledger=load_ledger(box);for(const auto& c:ledger.at("recovery_copies"))managed.insert(c.value("directory",""));}catch(...){g.complete=false;}
        std::error_code inner;
        for(fs::directory_iterator child(box,inner),stop;!inner&&child!=stop;child.increment(inner)){
            if(++visited>kBound){g.complete=false;break;}
            const auto path=child->path();
            if(plain(path,false)){std::error_code size_error;const auto n=fs::file_size(path,size_error);if(size_error)g.complete=false;else g.other_bytes+=n;continue;}
            if(!plain(path,true)){g.complete=false;continue;}
            std::uint64_t bytes=0;std::error_code files_error;
            for(fs::directory_iterator f(path,files_error),fend;!files_error&&f!=fend;f.increment(files_error)){
                if(++visited>kBound){g.complete=false;break;}
                if(!plain(f->path(),false)){g.complete=false;continue;}
                std::error_code size_error;const auto n=fs::file_size(f->path(),size_error);if(size_error)g.complete=false;else bytes+=n;}
            if(files_error)g.complete=false;
            if(made_here(box,path,L"package-")){++g.staging_directories;g.staging_bytes+=bytes;}
            else if(made_here(box,path,L"uninstall-")){++g.recovery_copies;g.recovery_bytes+=bytes;
                if(managed.contains(text(path.filename()))){++g.managed_recovery_copies;g.managed_recovery_bytes+=bytes;}}
            else g.other_bytes+=bytes;}
        if(inner)g.complete=false;
        u.total_bytes+=g.staging_bytes+g.recovery_bytes+g.other_bytes;if(!g.complete)u.complete=false;u.games.push_back(std::move(g));}
    if(outer)u.complete=false;
    // The shared model copies (one per distinct model hash), counted on their own.
    {std::error_code me;const auto models=shared_models(store_);
     if(fs::exists(models,me))for(fs::directory_iterator it(models,me),end;!me&&it!=end;it.increment(me)){
        const auto file=it->path()/L"nvngx_dlssnr.dll";
        if(!plain(it->path(),true)||!hex_sha256(text(it->path().filename()))||!plain(file,false)){u.complete=false;continue;}
        std::error_code size_error;const auto n=fs::file_size(file,size_error);if(size_error){u.complete=false;continue;}
        ++u.shared_models;u.shared_model_bytes+=n;u.total_bytes+=n;}
     if(me)u.complete=false;}
    return u;}
json storage_json(const StorageUsage& u,const std::vector<Entry>& titles){json games=json::array();
    for(const auto& g:u.games){std::string title;for(const auto& e:titles)if(e.id==g.id)title=e.title;
        games.push_back({{"id",g.id},{"title",title},{"staging_directories",g.staging_directories},{"staging_bytes",g.staging_bytes},
            {"recovery_copies",g.recovery_copies},{"recovery_bytes",g.recovery_bytes},{"managed_recovery_copies",g.managed_recovery_copies},{"managed_recovery_bytes",g.managed_recovery_bytes},
            {"unmanaged_recovery_copies",g.recovery_copies-g.managed_recovery_copies},{"other_bytes",g.other_bytes},{"complete",g.complete}});}
    return {{"schema","overglaze-manager-storage-v1"},{"games",games},{"total_bytes",u.total_bytes},{"data_disk_available_bytes",u.data_disk_available},
        {"data_disk_reserve_bytes",u.data_disk_reserve},{"recovery_copies_kept_per_game",kRecoveryCopiesKept},{"shared_models",u.shared_models},{"shared_model_bytes",u.shared_model_bytes},{"complete",u.complete},
        {"scope","read-only count of data/settings/plugin-manager; nothing is deleted by this. A successful install removes its own staging; a successful uninstall keeps the newest "+
            std::to_string(kRecoveryCopiesKept)+" ledger recovery copies. Unmanaged copies and failed installs' staging predate the rule or are kept for inspection; they wait for an explicit decision."}};}
void Manager::import_known_installations(){for(const auto& p:policies_){if(p.game_root.empty())continue;const auto exe=p.game_root/wide(p.executable);std::error_code ec;
    if(fs::is_regular_file(exe,ec)&&(fs::is_regular_file(p.game_root/identity::kInstallationFile,ec)||fs::is_regular_file(p.game_root/identity::legacy::kInstallationFile,ec)))try{add(exe);}catch(...){}}}
fs::path Manager::models_dir()const{return root_/L"app"/L"models";}
fs::path Manager::model_file()const{return models_dir()/L"nvngx_dlssnr.dll";}
ModelStatus Manager::import_model(const fs::path& source_path){
    const auto source=local(source_path);Pins pins;pins.parents(source.parent_path());
    const auto source_handle=pins.file(source);LARGE_INTEGER size{};
    need(GetFileSizeEx(source_handle,&size)&&size.QuadPart>0&&size.QuadPart<=512LL*1024*1024,"模型大小不在导入范围内");
    const auto hash=handle_digest(source_handle);const auto* reviewed=lab::model::known(hash);
    need(model_sha256_.empty()?reviewed!=nullptr:hash==model_sha256_,"模型版本未识别：文件未导入，请选择支持的原版模型");
    Writer writer(store_);directory(root_/L"app");directory(models_dir());pins.parents(models_dir());
    const auto destination=model_file();
    if(fs::exists(destination)){
        need(handle_digest(pins.file(destination))==hash,"模型目录已有不同文件；请先核对该文件，不会自动覆盖");
    }else{
        const auto staging=models_dir()/wide(".model-import-"+uuid()+".tmp");
        try{copy_handle(source_handle,staging,hash);
            need(MoveFileExW(staging.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"模型导入未完成：目标文件已存在或目录不可写");}
        catch(...){DeleteFileW(staging.c_str());throw;}
    }
    ModelStatus out;out.path=destination;out.present=out.known=true;out.sha256=hash;
    out.label=reviewed?reviewed->label:"test model";return out;
}

AppMaintenanceStatus app_maintenance_check(const fs::path& selected,const std::string& operation){
    AppMaintenanceStatus out;out.root=selected;out.operation=operation;
    auto block=[&](const std::string& code,const std::string& message,const fs::path& path=fs::path{},const std::string& title=std::string{}){
        out.allowed=false;json row={{"code",code},{"message",message}};
        if(!path.empty())row["path"]=text(path);if(!title.empty())row["title"]=title;out.blockers.push_back(std::move(row));};
    try{
        need(operation=="update"||operation=="uninstall","未知应用维护操作");
        need(selected.is_absolute()&&selected==selected.lexically_normal()&&selected!=selected.root_path(),"请选择规范的本地应用目录");
        const auto spelling=selected.native();need(spelling.size()>3&&spelling[1]==L':'&&spelling.find(L':',2)==std::wstring::npos,"应用目录不能是网络、设备或替代数据流路径");
        need(GetDriveTypeW(selected.root_path().c_str())==DRIVE_FIXED,"应用目录必须在本地固定磁盘上");
        for(const auto& part:selected.relative_path()){const auto n=part.native();need(!n.empty()&&n.back()!=L'.'&&n.back()!=L' ',"路径末尾不能有空格或句点");}
        for(auto p=selected;!p.empty();){const auto a=GetFileAttributesW(p.c_str());
            if(a==INVALID_FILE_ATTRIBUTES)need(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND,"应用目录不可读取");
            else need(!(a&FILE_ATTRIBUTE_REPARSE_POINT),"应用目录含链接，请选择普通文件夹");
            const auto parent=p.parent_path();if(parent==p)break;p=parent;}
        if(!fs::exists(selected)){need(operation=="update","应用目录已不存在，请检查卸载记录");out.message="可以安装。";return out;}
        need(fs::is_directory(selected),"应用路径不是文件夹");out.root=fs::canonical(selected);
        const auto manifest=out.root/L"app"/L"release-manifest.json";
        const auto marker=out.root/L"data"/L"settings"/L"application-install.json";
        bool recognised=false;
        if(fs::exists(manifest)){const auto j=read(manifest);
            need(j.is_object()&&j.value("schema","")=="overglaze-release-v2"&&j.value("platform","")=="windows-x64"&&j.contains("version")&&j.at("version").is_string(),"应用版本清单无效");recognised=true;}
        if(fs::exists(marker)){const auto j=read(marker);
            need(j.is_object()&&j.value("schema","")=="overglaze-application-install-v1"&&j.value("product","")=="overglaze"&&
                winpath::same_spelling(wide(j.at("root").get<std::string>()),out.root),"应用安装记录与目录不匹配");recognised=true;}
        if(!recognised){need(operation=="update"&&fs::is_empty(out.root),"所选目录已有其他内容；请选择空目录或既有 Overglaze 安装目录");out.message="可以安装。";return out;}
        // The caller itself is the preflight helper during uninstall. Any other
        // process mapped from this application directory blocks replacement.
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));need(snapshot.valid(),"无法检查应用进程");
        PROCESSENTRY32W proc{sizeof(proc)};need(Process32FirstW(snapshot.value,&proc)!=FALSE,"无法枚举应用进程");
        std::vector<PROCESSENTRY32W> processes;std::map<DWORD,DWORD> parents;
        do{need(processes.size()<16384,"应用进程检查超出范围");processes.push_back(proc);parents[proc.th32ProcessID]=proc.th32ParentProcessID;}
        while(Process32NextW(snapshot.value,&proc));need(GetLastError()==ERROR_NO_MORE_FILES,"应用进程检查不完整");
        // Inno's original uninstaller waits for a temporary worker, which in
        // turn launches this helper. Exempt only that exact owned uninstaller
        // on our ancestry chain, not every process named unins000.exe.
        std::set<DWORD> ancestors;DWORD ancestor=GetCurrentProcessId();
        for(unsigned depth=0;depth<16;++depth){const auto it=parents.find(ancestor);if(it==parents.end()||!it->second||!ancestors.insert(it->second).second)break;ancestor=it->second;}
        const auto prefix=out.root.native()+L"\\";
        for(const auto& process_info:processes){const auto& proc=process_info;if(proc.th32ProcessID==GetCurrentProcessId())continue;Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,proc.th32ProcessID));
            if(!process.valid()){if(wcsncmp(proc.szExeFile,L"overglaze",9)==0)block("process-unreadable","无法确认某个 Overglaze 进程已退出");continue;}
            std::wstring image(32768,0);DWORD count=DWORD(image.size());if(!QueryFullProcessImageNameW(process.value,0,image.data(),&count))continue;image.resize(count);
            if(operation=="uninstall"&&ancestors.contains(proc.th32ProcessID)&&winpath::same_spelling(fs::path(image),out.root/L"app/unins000.exe"))continue;
            if(image.size()>prefix.size()&&CompareStringOrdinal(image.data(),int(prefix.size()),prefix.data(),int(prefix.size()),TRUE)==CSTR_EQUAL)
                block("application-running","请先退出应用："+text(fs::path(image).filename()),fs::path(image));
        }
        const auto store=out.root/L"data"/L"settings"/L"plugin-manager";
        std::map<std::string,Entry> games;
        if(fs::exists(store)){winpath::require_no_reparse(store);need(fs::is_directory(store),"游戏登记路径无效");
            const auto registered=registry(store);for(const auto& row:registered.at("entries")){const auto e=entry(row);games.emplace(e.id,e);
                if(running(e.exe))block("game-running","请先退出游戏："+e.title,e.exe,e.title);
                if(operation=="uninstall")for(const auto& relative:{fs::path(L"overglaze.install.json"),fs::path(L"overglaze/overglaze.install.json"),fs::path(L"dlsslab.install.json"),fs::path(L"dlsslab/dlsslab.install.json")}){
                    const auto config=e.exe.parent_path()/relative;if(!fs::exists(config))continue;const auto j=read(config);
                    if(j.contains("output_root")&&j.at("output_root").is_string()&&winpath::same_spelling(wide(j.at("output_root").get<std::string>()),out.root/L"data"))
                        block("game-installed","请先在游戏库卸载插件："+e.title,e.exe,e.title);
                }}
            unsigned boxes=0;for(const auto& box:fs::directory_iterator(store)){
                winpath::require_no_reparse(box.path());if(!box.is_directory()||box.path().filename()==L"_models")continue;
                need(++boxes<=512,"游戏维护记录数量超出检查范围");const auto id=text(box.path().filename());
                const auto found=games.find(id);const std::string title=found==games.end()?id:found->second.title;
                for(const auto& name:{L"transaction.json",L"install-receipt.json"}){const auto file=box.path()/name;if(!fs::exists(file))continue;
                    const auto j=read(file);need(j.is_object()&&j.contains("state")&&j.at("state").is_string(),"游戏安装记录无效："+title);
                    const auto state=j.at("state").get<std::string>();const bool transaction=std::wstring_view(name)==L"transaction.json";
                    if(transaction){need((j.value("schema","")=="overglaze-install-transaction-v1"||j.value("schema","")=="dlsslab-install-transaction-v1")&&j.value("id","")==id,"游戏事务身份无效："+title);
                        need(state=="installed"||state=="installing"||state=="removing"||state=="removed","游戏事务状态无效："+title);}
                    else need(state=="installed"||state=="uninstalled","游戏回执状态无效："+title);
                    const bool gone=transaction?state=="removed":state=="uninstalled";
                    if(!gone&&(operation=="uninstall"||state!="installed"))block("game-dependency","请先在游戏库处理安装记录："+title,file,title);
                }
            }
        }
    }catch(const std::exception& e){block("check-incomplete",e.what(),selected);}
    out.message=out.allowed?"可以继续。":"请先处理以下项目：";
    if(!out.allowed)for(const auto& item:out.blockers)out.message+="\n"+item.at("message").get<std::string>();
    return out;
}
json app_maintenance_json(const AppMaintenanceStatus& s){return {{"schema","overglaze-app-maintenance-v1"},{"root",text(s.root)},{"operation",s.operation},{"allowed",s.allowed},{"message",s.message},{"blockers",s.blockers}};}
// What the user has supplied. Never throws for an absent or unreadable file:
// that is exactly the state the page has to show in words.
ModelStatus Manager::model_status()const{ModelStatus m;m.path=model_file();std::error_code ec;
    if(!fs::is_regular_file(m.path,ec))return m;
    try{m.sha256=digest(m.path);m.present=true;}catch(const std::exception& e){m.error=e.what();return m;}
    if(!model_sha256_.empty()){m.known=m.sha256==model_sha256_;if(m.known)m.label="test model";return m;}
    if(const auto* v=lab::model::known(m.sha256)){m.known=true;m.label=v->label;}
    return m;}
json model_json(const ModelStatus& m){json known=json::array();for(const auto& v:lab::model::kKnownVersions)known.push_back({{"sha256",v.sha256},{"label",v.label}});
    const char* code=!m.present?"model-missing":!m.known?"model-unknown-version":"model-ready";
    return {{"schema","overglaze-model-status-v1"},{"path",text(m.path)},{"present",m.present},{"sha256",m.sha256.empty()?json(nullptr):json(m.sha256)},
        {"known",m.known},{"label",m.label.empty()?json(nullptr):json(m.label)},{"error",m.error.empty()?json(nullptr):json(m.error)},
        {"reason",reason_json({code,!m.present?json{{"path",text(m.path)}}:!m.known?json{{"hash",m.sha256}}:json::object()})},{"known_versions",known},
        {"scope","read-only. This project does not include or redistribute nvngx_dlssnr.dll; the user supplies their own copy. Only reviewed versions are used."}};}
void Manager::migrate(const std::string& id,bool risk_accepted,const std::string& consent,const Progress& progress){
    {const auto s=inspect(find(id));
     if(s.install_state!="legacy")throw Refusal("migrate-not-legacy","这款游戏没有旧版（DLSS Lab）安装，不需要迁移",json{{"install",s.install_state}});}
    update(id,risk_accepted,consent,progress);}
}
