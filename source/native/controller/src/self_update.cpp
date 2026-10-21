// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Self-update of the desktop program (lab_self_update.hpp). Out of game only.
#include "lab_self_update.hpp"
#include "lab_game_manager.hpp"
#include "lab_windows_path.hpp"
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>

namespace lab::update {
namespace fs=std::filesystem;
namespace {
[[noreturn]] void fail(const std::string& why){throw Error(why);}
void need(bool ok,const std::string& why){if(!ok)fail(why);}

bool digits(std::string_view s){return !s.empty()&&std::all_of(s.begin(),s.end(),[](char c){return c>='0'&&c<='9';});}
bool identifier(std::string_view s){return !s.empty()&&std::all_of(s.begin(),s.end(),[](char c){
    return (c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='-';});}
// A SemVer numeric identifier: no leading zero, bounded.
std::optional<std::uint64_t> number(std::string_view s){
    if(!digits(s)||s.size()>15||(s.size()>1&&s[0]=='0'))return std::nullopt;
    std::uint64_t v=0;for(char c:s)v=v*10+std::uint64_t(c-'0');return v;}
bool lower_hex(std::string_view s,std::size_t min,std::size_t max){
    return s.size()>=min&&s.size()<=max&&std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool starts_with(std::string_view s,std::string_view prefix){return s.size()>=prefix.size()&&s.substr(0,prefix.size())==prefix;}
std::string hex(const unsigned char* p,std::size_t n){static const char* d="0123456789abcdef";std::string s;s.reserve(n*2);
    for(std::size_t i=0;i<n;++i){s+=d[p[i]>>4];s+=d[p[i]&15];}return s;}

class Sha256 {
    BCRYPT_ALG_HANDLE alg_=nullptr;BCRYPT_HASH_HANDLE hash_=nullptr;
public:
    Sha256(){need(BCryptOpenAlgorithmProvider(&alg_,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0,"系统 SHA-256 不可用");
        if(BCryptCreateHash(alg_,&hash_,nullptr,0,nullptr,0,0)!=0){BCryptCloseAlgorithmProvider(alg_,0);alg_=nullptr;fail("系统 SHA-256 不可用");}}
    ~Sha256(){if(hash_)BCryptDestroyHash(hash_);if(alg_)BCryptCloseAlgorithmProvider(alg_,0);}
    Sha256(const Sha256&)=delete;Sha256& operator=(const Sha256&)=delete;
    void add(const void* data,std::size_t n){
        auto* p=static_cast<const unsigned char*>(data);
        while(n){const auto part=ULONG(std::min<std::size_t>(n,1u<<20));
            need(BCryptHashData(hash_,const_cast<PUCHAR>(p),part,0)==0,"SHA-256 计算失败");p+=part;n-=part;}}
    std::array<unsigned char,32> digest(){std::array<unsigned char,32> d{};need(BCryptFinishHash(hash_,d.data(),ULONG(d.size()),0)==0,"SHA-256 计算失败");return d;}
    std::string finish(){const auto d=digest();return hex(d.data(),d.size());}
};

std::optional<std::vector<unsigned char>> base64(std::string_view s){
    if(s.empty()||s.size()%4)return std::nullopt;
    auto value=[](char c)->int{
        if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;
        if(c=='+')return 62;if(c=='/')return 63;return -1;};
    std::size_t pad=0;if(s.back()=='='){pad=1;if(s[s.size()-2]=='=')pad=2;}
    std::vector<unsigned char> out;out.reserve(s.size()/4*3);
    for(std::size_t i=0;i<s.size();i+=4){
        int v[4];const bool last=i+4==s.size();
        for(unsigned k=0;k<4;++k){const bool padded=last&&k>=4-pad;v[k]=padded?0:value(s[i+k]);if(v[k]<0)return std::nullopt;}
        const std::uint32_t q=(std::uint32_t(v[0])<<18)|(std::uint32_t(v[1])<<12)|(std::uint32_t(v[2])<<6)|std::uint32_t(v[3]);
        out.push_back(static_cast<unsigned char>(q>>16));
        if(!(last&&pad==2))out.push_back(static_cast<unsigned char>(q>>8));else if(q&0xFFFF)return std::nullopt; // non-canonical bits
        if(!(last&&pad>=1))out.push_back(static_cast<unsigned char>(q));else if(pad==1&&(q&0xFF))return std::nullopt;
    }
    return out;
}

struct Internet {
    HINTERNET h=nullptr;explicit Internet(HINTERNET x):h(x){}
    ~Internet(){if(h)WinHttpCloseHandle(h);}
    Internet(const Internet&)=delete;Internet& operator=(const Internet&)=delete;
};
std::string network_error(const char* step){
    const auto code=GetLastError();std::string what;
    switch(code){
        case ERROR_WINHTTP_TIMEOUT:what="连接超时";break;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:what="无法解析服务器地址";break;
        case ERROR_WINHTTP_CANNOT_CONNECT:case ERROR_WINHTTP_CONNECTION_ERROR:what="无法连接服务器";break;
        case ERROR_WINHTTP_SECURE_FAILURE:what="TLS 证书校验失败";break;
        case ERROR_WINHTTP_REDIRECT_FAILED:what="服务器重定向被拒绝";break;
        default:what=step;}
    return "网络请求失败："+what+"（WinHTTP "+std::to_string(code)+"）";
}
struct Url {std::wstring host,path;INTERNET_PORT port=0;bool secure=false;};
// HTTPS only; plain HTTP only to 127.0.0.1 for the loopback test endpoint.
Url crack(const Endpoint& e,const std::wstring& url){
    need(!url.empty()&&url.size()<4096,"无效的下载地址");
    URL_COMPONENTS c{};c.dwStructSize=sizeof c;
    c.dwSchemeLength=c.dwHostNameLength=c.dwUrlPathLength=c.dwExtraInfoLength=c.dwUserNameLength=c.dwPasswordLength=DWORD(-1);
    need(WinHttpCrackUrl(url.c_str(),DWORD(url.size()),0,&c)!=FALSE,"无效的下载地址");
    Url u;u.secure=c.nScheme==INTERNET_SCHEME_HTTPS;u.port=c.nPort;
    u.host.assign(c.lpszHostName,c.dwHostNameLength);u.path.assign(c.lpszUrlPath,c.dwUrlPathLength);
    if(c.lpszExtraInfo)u.path.append(c.lpszExtraInfo,c.dwExtraInfoLength);
    need(c.dwUserNameLength==0&&c.dwPasswordLength==0&&!u.host.empty(),"无效的下载地址");
    if(!u.secure)need(e.loopback_test&&c.nScheme==INTERNET_SCHEME_HTTP&&u.host==L"127.0.0.1","只允许 HTTPS 连接");
    if(u.path.empty())u.path=L"/";
    return u;
}
using Sink=std::function<void(const char*,std::size_t)>;
// One GET. Redirects are followed by WinHTTP (never HTTPS to HTTP); the final
// URL obeys the same scheme rule. No cookies, no credentials, no Referer.
void get(const Endpoint& e,const std::string& address,const wchar_t* accept,std::uint64_t limit,const Cancel* cancel,const Sink& sink,std::uint64_t* announced=nullptr){
    const auto url=wide(address);const auto u=crack(e,url);
    Internet session(WinHttpOpen(wide(e.user_agent).c_str(),e.loopback_test?WINHTTP_ACCESS_TYPE_NO_PROXY:WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session.h)fail(network_error("无法初始化"));
    WinHttpSetTimeouts(session.h,10000,10000,10000,10000);
    DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session.h,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof policy);
    Internet connection(WinHttpConnect(session.h,u.host.c_str(),u.port,0));
    if(!connection.h)fail(network_error("无法连接服务器"));
    Internet request(WinHttpOpenRequest(connection.h,L"GET",u.path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,u.secure?WINHTTP_FLAG_SECURE:0));
    if(!request.h)fail(network_error("无法创建请求"));
    DWORD features=WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(request.h,WINHTTP_OPTION_DISABLE_FEATURE,&features,sizeof features);
    DWORD logon=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    WinHttpSetOption(request.h,WINHTTP_OPTION_AUTOLOGON_POLICY,&logon,sizeof logon);
    const std::wstring headers=std::wstring(L"Accept: ")+accept;
    if(!WinHttpSendRequest(request.h,headers.c_str(),DWORD(headers.size()),WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request.h,nullptr))
        fail(network_error("请求未完成"));
    DWORD status=0,size=sizeof status;
    need(WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)!=FALSE,"服务器响应无效");
    if(status==403||status==429)fail("GitHub 暂时限制了请求（HTTP "+std::to_string(status)+"），请稍后再试");
    need(status==200,"服务器返回 HTTP "+std::to_string(status));
    {DWORD bytes=0;WinHttpQueryOption(request.h,WINHTTP_OPTION_URL,nullptr,&bytes);
     need(bytes>0&&bytes<65536,"服务器响应无效");std::wstring final_url(bytes/sizeof(wchar_t),L'\0');
     need(WinHttpQueryOption(request.h,WINHTTP_OPTION_URL,final_url.data(),&bytes)!=FALSE,"服务器响应无效");
     final_url.resize(wcsnlen(final_url.c_str(),final_url.size()));crack(e,final_url);}
    {wchar_t text[32]{};DWORD bytes=sizeof text;
     if(WinHttpQueryHeaders(request.h,WINHTTP_QUERY_CONTENT_LENGTH,WINHTTP_HEADER_NAME_BY_INDEX,text,&bytes,WINHTTP_NO_HEADER_INDEX)){
        const auto length=_wcstoui64(text,nullptr,10);need(length<=limit,"服务器返回的内容超出上限");if(announced)*announced=length;}}
    std::vector<char> buffer(64*1024);std::uint64_t total=0;
    for(;;){
        if(cancel&&cancel->load())throw Cancelled();
        DWORD available=0;if(!WinHttpQueryDataAvailable(request.h,&available))fail(network_error("接收中断"));
        if(!available)break;
        DWORD read=0;if(!WinHttpReadData(request.h,buffer.data(),std::min<DWORD>(available,DWORD(buffer.size())),&read))fail(network_error("接收中断"));
        if(!read)break;
        total+=read;need(total<=limit,"服务器返回的内容超出上限");sink(buffer.data(),read);
    }
    if(cancel&&cancel->load())throw Cancelled();
}
std::string get_text(const Endpoint& e,const std::string& url,const wchar_t* accept,std::uint64_t limit,const Cancel* cancel){
    std::string body;get(e,url,accept,limit,cancel,[&](const char* p,std::size_t n){body.append(p,n);});return body;}

// A plain file name we may create: no path, no stream, nothing hidden.
bool plain_name(std::string_view s,std::string_view suffix){
    if(s.empty()||s.size()>128||s[0]=='.'||s.size()<=suffix.size()||s.substr(s.size()-suffix.size())!=suffix)return false;
    return std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='.'||c=='-'||c=='_';});}
const json& field(const json& j,const char* key){const auto it=j.find(key);need(it!=j.end(),std::string("更新清单缺少字段 ")+key);return *it;}
std::string text_field(const json& j,const char* key,std::size_t max,bool allow_empty=false){
    const auto& v=field(j,key);need(v.is_string(),std::string("更新清单字段类型错误：")+key);
    auto s=v.get<std::string>();need((allow_empty||!s.empty())&&s.size()<=max,std::string("更新清单字段无效：")+key);return s;}
FileRef file_field(const json& j,const char* key,std::string_view suffix){
    const auto& v=field(j,key);need(v.is_object(),std::string("更新清单字段类型错误：")+key);FileRef f;
    f.name=text_field(v,"name",128);need(plain_name(f.name,suffix),std::string("更新清单文件名无效：")+key);
    const auto& size=field(v,"size");need(size.is_number_unsigned(),std::string("更新清单文件大小无效：")+key);
    f.size=size.get<std::uint64_t>();need(f.size>0&&f.size<=kSetupLimit,std::string("更新清单文件大小无效：")+key);
    f.sha256=text_field(v,"sha256",64);need(lower_hex(f.sha256,64,64),std::string("更新清单 SHA-256 无效：")+key);
    return f;}

void verify_handle(HANDLE h,const FileRef& expected){
    LARGE_INTEGER size{};need(GetFileSizeEx(h,&size)!=FALSE,"无法读取安装程序");
    need(std::uint64_t(size.QuadPart)==expected.size,"安装程序大小与签名清单不符");
    LARGE_INTEGER zero{};need(SetFilePointerEx(h,zero,nullptr,FILE_BEGIN)!=FALSE,"无法读取安装程序");
    Sha256 hash;std::vector<char> buffer(1<<20);
    for(;;){DWORD read=0;need(ReadFile(h,buffer.data(),DWORD(buffer.size()),&read,nullptr)!=FALSE,"无法读取安装程序");if(!read)break;hash.add(buffer.data(),read);}
    need(hash.finish()==expected.sha256,"安装程序 SHA-256 与签名清单不符");
}
// Small own files (settings, markers): read bounded, written through a temp file.
std::optional<std::string> read_small(const fs::path& file,std::uint64_t limit){
    std::error_code ec;if(!fs::is_regular_file(file,ec))return std::nullopt;
    const auto a=GetFileAttributesW(file.c_str());if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))return std::nullopt;
    const auto n=fs::file_size(file,ec);if(ec||n>limit)return std::nullopt;
    std::ifstream in(file,std::ios::binary);std::string s(static_cast<std::size_t>(n),'\0');in.read(s.data(),std::streamsize(n));
    if(!in)return std::nullopt;return s;}
void write_atomic(const fs::path& file,const std::string& bytes){
    const auto temp=fs::path(file.wstring()+L".tmp-"+wide(uuid()));
    Handle out(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    need(out.valid(),"无法写入 "+utf8(file.filename().wstring()));
    DWORD written=0;const bool ok=WriteFile(out.value,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(out.value);
    CloseHandle(out.value);out.value=INVALID_HANDLE_VALUE;
    if(!ok||!MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp.c_str());fail("无法写入 "+utf8(file.filename().wstring()));}
}
bool reparse(const fs::path& p){const auto a=GetFileAttributesW(p.c_str());return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT);}
constexpr const wchar_t* kMarker=L".overglaze-download.json";
constexpr const char* kMarkerKind="overglaze-update-download";
// The marker of a folder this program created, naming the version and the Setup file.
std::optional<std::pair<std::string,std::string>> marker(const fs::path& folder){
    const auto bytes=read_small(folder/kMarker,4096);if(!bytes)return std::nullopt;
    const auto j=json::parse(*bytes,nullptr,false);
    if(j.is_discarded()||!j.is_object()||!j.contains("kind")||j["kind"]!=kMarkerKind||!j.contains("version")||!j["version"].is_string()||!j.contains("setup")||!j["setup"].is_string())return std::nullopt;
    const auto version=j["version"].get<std::string>(),setup=j["setup"].get<std::string>();
    if(!parse_version(version)||!plain_name(setup,".exe")||utf8(folder.filename().wstring())!=version)return std::nullopt;
    return std::make_pair(version,setup);}
// The Setup a marker names and its partial download: the only files we delete.
void remove_own_files(const fs::path& folder,const std::string& setup){
    std::error_code ec;
    for(const auto& name:{wide(setup),wide(setup)+L".partial"}){
        const auto file=folder/name;if(fs::is_regular_file(file,ec)&&!reparse(file))DeleteFileW(file.c_str());}}
std::uint64_t free_space(const fs::path& where){ULARGE_INTEGER available{};need(GetDiskFreeSpaceExW(where.c_str(),&available,nullptr,nullptr)!=FALSE,"无法读取磁盘剩余空间");return available.QuadPart;}
UninstallLookup registry_install_location(){
    const std::wstring key=std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\")+kSetupAppId+L"_is1";
    DWORD bytes=0;auto status=RegGetValueW(HKEY_CURRENT_USER,key.c_str(),L"InstallLocation",RRF_RT_REG_SZ,nullptr,nullptr,&bytes);
    if(status==ERROR_FILE_NOT_FOUND){
        // RegGetValueW answers the same for a missing entry and a missing value.
        HKEY opened=nullptr;const auto open=RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_QUERY_VALUE,&opened);
        if(open==ERROR_SUCCESS)RegCloseKey(opened);
        return {std::nullopt,long(status),open==ERROR_FILE_NOT_FOUND};}
    if(status!=ERROR_SUCCESS)return {std::nullopt,long(status)};
    if(bytes<sizeof(wchar_t)||bytes>65536)return {std::nullopt,long(ERROR_INVALID_DATA)};
    std::wstring value(bytes/sizeof(wchar_t),L'\0');
    status=RegGetValueW(HKEY_CURRENT_USER,key.c_str(),L"InstallLocation",RRF_RT_REG_SZ,nullptr,value.data(),&bytes);
    if(status!=ERROR_SUCCESS)return {std::nullopt,long(status)};
    value.resize(wcsnlen(value.c_str(),value.size()));return UninstallLookup{std::move(value)};}
// Why the uninstall entry gives no location, with the Win32 error when there is one.
std::string uninstall_reason(const UninstallLookup& r){
    const auto code=r.error?"（错误 "+std::to_string(r.error)+"）":std::string();
    if(r.location)return "Windows 卸载登记里的安装位置为空";
    if(!r.error||r.key_missing)return "Windows 卸载登记里没有这个程序"+code;
    if(r.error==ERROR_FILE_NOT_FOUND)return "Windows 卸载登记里没有安装位置"+code;
    if(r.error==ERROR_UNSUPPORTED_TYPE)return "Windows 卸载登记里的安装位置类型不对"+code;
    if(r.error==ERROR_INVALID_DATA)return "Windows 卸载登记里的安装位置无效"+code;
    return "无法读取 Windows 卸载登记"+code;}
std::string mib(std::uint64_t bytes){return std::to_string((bytes+(1ULL<<20)-1)>>20)+" MiB";}
}

// ---- versions
std::string Version::text()const{
    auto s=std::to_string(major)+"."+std::to_string(minor)+"."+std::to_string(patch);
    for(std::size_t i=0;i<pre.size();++i)s+=(i?".":"-")+pre[i];return s;}
std::optional<Version> parse_version(std::string_view text){
    if(text.empty()||text.size()>64)return std::nullopt;
    Version v;const auto dash=text.find('-');const auto core=text.substr(0,dash);
    std::uint64_t* parts[]{&v.major,&v.minor,&v.patch};std::size_t start=0;
    for(unsigned i=0;i<3;++i){
        const auto end=i<2?core.find('.',start):core.size();if(end==std::string_view::npos)return std::nullopt;
        const auto n=number(core.substr(start,end-start));if(!n)return std::nullopt;*parts[i]=*n;start=end+1;}
    if(dash!=std::string_view::npos){
        const auto pre=text.substr(dash+1);if(pre.empty())return std::nullopt;
        for(std::size_t s=0;;){
            const auto e=pre.find('.',s);const auto id=pre.substr(s,e==std::string_view::npos?std::string_view::npos:e-s);
            if(!identifier(id)||(digits(id)&&!number(id)))return std::nullopt;
            v.pre.emplace_back(id);if(e==std::string_view::npos)break;s=e+1;}
    }
    return v;
}
std::optional<Version> parse_tag(std::string_view tag){if(!tag.empty()&&tag[0]=='v')tag.remove_prefix(1);return parse_version(tag);}
int compare(const Version& a,const Version& b){
    if(a.major!=b.major)return a.major<b.major?-1:1;
    if(a.minor!=b.minor)return a.minor<b.minor?-1:1;
    if(a.patch!=b.patch)return a.patch<b.patch?-1:1;
    if(a.pre.empty()||b.pre.empty())return a.pre.empty()==b.pre.empty()?0:a.pre.empty()?1:-1;
    for(std::size_t i=0;i<std::min(a.pre.size(),b.pre.size());++i){
        const auto& x=a.pre[i];const auto& y=b.pre[i];const bool nx=digits(x),ny=digits(y);
        if(nx&&ny){const auto vx=*number(x),vy=*number(y);if(vx!=vy)return vx<vy?-1:1;}
        else if(nx!=ny)return nx?-1:1; // numeric identifiers rank below alphanumeric ones
        else if(x!=y)return x<y?-1:1;
    }
    if(a.pre.size()!=b.pre.size())return a.pre.size()<b.pre.size()?-1:1;
    return 0;
}

// ---- signature
std::optional<std::vector<unsigned char>> decode_signature(std::string_view file){
    if(file.size()>=2&&file.substr(file.size()-2)=="\r\n")file.remove_suffix(2);
    else if(!file.empty()&&file.back()=='\n')file.remove_suffix(1);
    if(file.size()!=88)return std::nullopt;
    auto bytes=base64(file);if(!bytes||bytes->size()!=64)return std::nullopt;return bytes;}
std::string sha256_hex(std::string_view bytes){Sha256 h;h.add(bytes.data(),bytes.size());return h.finish();}
bool verify(std::string_view message,std::string_view signature_file,std::span<const unsigned char> key,std::string* why){
    auto refuse=[&](const char* reason){if(why)*why=reason;return false;};
    try{
        const auto signature=decode_signature(signature_file);if(!signature)return refuse("签名文件格式无效");
        if(key.size()!=sizeof(BCRYPT_ECCKEY_BLOB)+64)return refuse("公钥格式无效");
        BCRYPT_ECCKEY_BLOB header{};std::memcpy(&header,key.data(),sizeof header);
        if(header.dwMagic!=BCRYPT_ECDSA_PUBLIC_P256_MAGIC||header.cbKey!=32)return refuse("公钥格式无效");
        if(message.empty()||message.size()>kManifestLimit)return refuse("更新清单大小无效");
        auto digest=[&]{Sha256 h;h.add(message.data(),message.size());return h.digest();}();
        BCRYPT_ALG_HANDLE alg=nullptr;if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_ECDSA_P256_ALGORITHM,nullptr,0)!=0)return refuse("系统 ECDSA 不可用");
        BCRYPT_KEY_HANDLE handle=nullptr;
        if(BCryptImportKeyPair(alg,nullptr,BCRYPT_ECCPUBLIC_BLOB,&handle,const_cast<PUCHAR>(key.data()),ULONG(key.size()),0)!=0){BCryptCloseAlgorithmProvider(alg,0);return refuse("公钥无法导入");}
        const auto status=BCryptVerifySignature(handle,nullptr,digest.data(),ULONG(digest.size()),const_cast<PUCHAR>(signature->data()),ULONG(signature->size()),0);
        BCryptDestroyKey(handle);BCryptCloseAlgorithmProvider(alg,0);
        if(status!=0)return refuse("签名与发布密钥不匹配");
        if(why)why->clear();return true;
    }catch(const std::exception&){return refuse("签名校验失败");}
}

// ---- manifest
Manifest parse_manifest(std::string_view bytes){
    need(!bytes.empty()&&bytes.size()<=kManifestLimit,"更新清单大小无效");
    const auto j=json::parse(bytes.begin(),bytes.end(),nullptr,false);
    need(!j.is_discarded()&&j.is_object(),"更新清单不是有效的 JSON");
    need(text_field(j,"schema",64)=="overglaze-update-v1","更新清单格式版本未知");
    Manifest m;m.version=text_field(j,"version",64);need(parse_version(m.version).has_value(),"更新清单版本号无效");
    m.channel=text_field(j,"channel",16);need(m.channel=="preview"||m.channel=="stable","更新清单渠道无效");
    m.published=text_field(j,"published",10);
    need(m.published.size()==10&&m.published[4]=='-'&&m.published[7]=='-'&&digits(m.published.substr(0,4))&&digits(m.published.substr(5,2))&&digits(m.published.substr(8,2)),"更新清单发布日期无效");
    m.source_commit=text_field(j,"source_commit",64);need(lower_hex(m.source_commit,40,64),"更新清单源码提交无效");
    m.notes_zh=text_field(j,"notes_zh",32*1024,true);
    m.setup=file_field(j,"setup",".exe");m.portable=file_field(j,"portable",".zip");
    return m;
}
void verify_file(const fs::path& file,const FileRef& expected){
    Handle h(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    need(h.valid(),"无法读取下载的安装程序");verify_handle(h.value,expected);}

// ---- endpoint and release list
Endpoint official(const std::string& running_version){
    Endpoint e;e.releases_api=kReleasesApi;e.asset_prefix=kAssetPrefix;e.page_prefix=kPagePrefix;e.releases_page=kReleasesPage;
    e.user_agent="Overglaze/"+running_version;e.release_key.assign(kReleaseKey.begin(),kReleaseKey.end());return e;}
const Asset* Release::asset(std::string_view name)const{for(const auto& a:assets)if(a.name==name)return &a;return nullptr;}
Selection select_release(const json& list,const Version& running,const Endpoint& e){
    Selection best;std::optional<Version> top;
    if(!list.is_array())return best;
    for(const auto& r:list){
        if(!r.is_object())continue;
        const auto draft=r.find("draft");if(draft==r.end()||!draft->is_boolean()||draft->get<bool>())continue;
        const auto tag=r.find("tag_name");if(tag==r.end()||!tag->is_string())continue;
        const auto version=parse_tag(tag->get<std::string>());if(!version||compare(*version,running)<=0)continue;
        if(top&&compare(*version,*top)<=0)continue;
        Release rel;rel.tag=tag->get<std::string>();rel.version=*version;rel.page=e.releases_page;
        const auto page=r.find("html_url");
        if(page!=r.end()&&page->is_string()&&starts_with(page->get<std::string>(),e.page_prefix)&&page->get<std::string>().size()<1024)rel.page=page->get<std::string>();
        const auto assets=r.find("assets");
        if(assets!=r.end()&&assets->is_array())for(const auto& a:*assets){
            if(!a.is_object())continue;const auto name=a.find("name"),url=a.find("browser_download_url"),size=a.find("size");
            if(name==a.end()||url==a.end()||size==a.end()||!name->is_string()||!url->is_string()||!size->is_number_unsigned())continue;
            if(!starts_with(url->get<std::string>(),e.asset_prefix)||url->get<std::string>().size()>=2048)continue;
            rel.assets.push_back({name->get<std::string>(),url->get<std::string>(),size->get<std::uint64_t>()});}
        best.release=std::move(rel);top=version;
    }
    if(top)best.kind=best.release.asset(kManifestAsset)&&best.release.asset(kSignatureAsset)?Selection::Kind::signed_update:Selection::Kind::manual_only;
    return best;
}

// ---- check
namespace {
// What a fresh check and a cached offer must both pass: the signature over the
// exact bytes first (nothing in the manifest is read before it verifies), then
// the manifest, its version against the tag and the running version, and the
// release's Setup asset against the manifest.
Offer signed_offer(const Endpoint& e,const Version& running,const Release& release,std::string manifest,std::string signature){
    std::string why;if(!verify(manifest,signature,e.release_key,&why))fail("更新清单签名校验失败（"+why+"），已拒绝这个更新");
    Offer o;o.version=release.version.text();o.page=release.page;o.tag=release.tag;
    o.manifest=parse_manifest(manifest);
    const auto version=*parse_version(o.manifest.version);
    need(compare(version,release.version)==0,"签名清单的版本 "+o.manifest.version+" 与发布标签 "+release.tag+" 不一致，已拒绝");
    need(compare(version,running)>0,"签名清单的版本 "+o.manifest.version+" 不比当前版本新，已拒绝");
    const auto* setup=release.asset(o.manifest.setup.name);
    need(setup!=nullptr,"发布中没有签名清单所列的安装程序 "+o.manifest.setup.name);
    need(setup->size==o.manifest.setup.size,"发布中安装程序的大小与签名清单不符");
    o.setup=*setup;o.manifest_bytes=std::move(manifest);o.signature=std::move(signature);
    o.state=Offer::State::available;return o;
}
}
Offer check(const Endpoint& e,const Version& running,const Cancel* cancel){
    const auto body=get_text(e,e.releases_api,L"application/vnd.github+json",kReleasesLimit,cancel);
    const auto list=json::parse(body,nullptr,false);need(!list.is_discarded()&&list.is_array(),"GitHub 返回的发布列表无法解析");
    const auto selection=select_release(list,running,e);
    Offer o;if(selection.kind==Selection::Kind::none)return o;
    const auto& release=selection.release;o.version=release.version.text();o.page=release.page;
    if(selection.kind==Selection::Kind::manual_only){o.state=Offer::State::manual_only;return o;}
    const auto* manifest_asset=release.asset(kManifestAsset);const auto* signature_asset=release.asset(kSignatureAsset);
    need(manifest_asset->size<=kManifestLimit&&signature_asset->size<=kSignatureLimit,"发布中的更新清单大小无效");
    auto manifest=get_text(e,manifest_asset->url,L"application/octet-stream",kManifestLimit,cancel);
    auto signature=get_text(e,signature_asset->url,L"application/octet-stream",kSignatureLimit,cancel);
    return signed_offer(e,running,release,std::move(manifest),std::move(signature));
}

// ---- offer cache
namespace {
constexpr const char* kOfferKind="overglaze-update-offer";
// A file of ours under data\settings, written atomically; no reparse point on the way.
void save_setting(const fs::path& file,const std::string& bytes){
    std::error_code ec;const auto folder=file.parent_path();
    if(!fs::exists(folder,ec))need(CreateDirectoryW(folder.c_str(),nullptr)!=FALSE||GetLastError()==ERROR_ALREADY_EXISTS,"无法创建设置目录");
    winpath::require_no_reparse(folder);if(fs::exists(file,ec))need(!reparse(file),"设置文件路径无效");
    write_atomic(file,bytes);}
std::string cached_text(const json& j,const char* key){
    const auto it=j.find(key);need(it!=j.end()&&it->is_string(),std::string("缺少字段 ")+key);return it->get<std::string>();}
}
fs::path offer_cache_file(const fs::path& data_root){return data_root/L"settings"/L"update-offer-v1.json";}
bool clear_offer(const fs::path& data_root){
    if(data_root.empty())return false;
    const auto file=offer_cache_file(data_root);
    const auto a=GetFileAttributesW(file.c_str());
    if(a==INVALID_FILE_ATTRIBUTES||(a&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))return false;
    need(DeleteFileW(file.c_str())!=FALSE||GetLastError()==ERROR_FILE_NOT_FOUND,"无法删除更新提示缓存");
    return true;
}
void record_offer(const fs::path& data_root,const Offer& o){
    if(data_root.empty())return;
    if(o.state!=Offer::State::available||o.manifest_bytes.empty()||o.signature.empty()){clear_offer(data_root);return;}
    std::string bytes;
    try{bytes=json{{"kind",kOfferKind},{"version",1},{"saved_utc",std::int64_t(std::time(nullptr))},{"tag",o.tag},{"page",o.page},
        {"setup",{{"name",o.setup.name},{"url",o.setup.url},{"size",o.setup.size}}},
        {"manifest",o.manifest_bytes},{"signature",o.signature}}.dump(2)+"\n";}
    catch(const std::exception&){bytes.clear();}
    // A stale offer must not outlive a newer check: what cannot be saved is removed.
    try{need(!bytes.empty()&&bytes.size()<=kOfferCacheLimit,"更新提示缓存无效或超出上限，未保存");
        save_setting(offer_cache_file(data_root),bytes);}
    catch(...){try{clear_offer(data_root);}catch(...){}throw;}
}
std::optional<Offer> cached_offer(const fs::path& data_root,const Endpoint& e,const Version& running,std::string* why){
    if(why)why->clear();
    if(data_root.empty())return std::nullopt;
    const auto file=offer_cache_file(data_root);
    if(GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES)return std::nullopt;
    auto drop=[&](const std::string& reason)->std::optional<Offer>{
        std::string removal;try{clear_offer(data_root);}catch(const std::exception& x){removal=std::string("；")+x.what();}
        if(why)*why="已丢弃缓存的更新提示："+reason+removal;return std::nullopt;};
    const auto bytes=read_small(file,kOfferCacheLimit);
    if(!bytes)return drop("缓存文件无效或超出上限");
    try{
        const auto j=json::parse(*bytes,nullptr,false);
        need(!j.is_discarded()&&j.is_object()&&j.contains("kind")&&j["kind"]==kOfferKind&&j.contains("version")&&j["version"]==1,"缓存格式未知");
        Release release;release.tag=cached_text(j,"tag");
        const auto tag=parse_tag(release.tag);need(tag.has_value(),"发布标签无效");release.version=*tag;
        need(compare(release.version,running)>0,"版本 "+release.version.text()+" 不比当前版本 "+running.text()+" 新");
        release.page=cached_text(j,"page");
        need(release.page==e.releases_page||(starts_with(release.page,e.page_prefix)&&release.page.size()<1024),"发布页地址无效");
        const auto setup=j.find("setup");need(setup!=j.end()&&setup->is_object(),"缺少字段 setup");
        Asset asset;asset.name=cached_text(*setup,"name");asset.url=cached_text(*setup,"url");
        const auto size=setup->find("size");need(size!=setup->end()&&size->is_number_unsigned(),"安装程序大小无效");asset.size=size->get<std::uint64_t>();
        need(starts_with(asset.url,e.asset_prefix)&&asset.url.size()<2048,"下载地址无效");
        release.assets.push_back(std::move(asset));
        auto o=signed_offer(e,running,release,cached_text(j,"manifest"),cached_text(j,"signature"));
        o.from_cache=true;return o;
    }catch(const std::exception& x){return drop(x.what());}
}
void OfferNotice::start(std::optional<Offer> cached){offer_=std::move(cached);hidden_=false;}
void OfferNotice::checked(Offer o,bool manual){
    if(o.state==Offer::State::up_to_date){offer_.reset();hidden_=false;return;}
    if(manual||!offer_||offer_->version!=o.version)hidden_=false;
    offer_=std::move(o);
}
const Offer* OfferNotice::shown()const{
    return offer_&&offer_->state!=Offer::State::up_to_date&&!hidden_?&*offer_:nullptr;}

// ---- download
fs::path updates_dir(const fs::path& data_root){return data_root/L"updates";}
std::uint64_t space_needed(std::uint64_t setup_size){return setup_size*5+(256ULL<<20);}
namespace {
// Removes our update folders whose version `remove` selects (see cleanup_downloads).
unsigned cleanup_if(const fs::path& data_root,const std::function<bool(const Version&)>& remove){
    const auto root=updates_dir(data_root);std::error_code ec;
    if(!fs::is_directory(root,ec)||reparse(root))return 0;
    std::vector<fs::path> folders;
    for(fs::directory_iterator it(root,ec),end;!ec&&it!=end;it.increment(ec))
        if(it->is_directory(ec)&&!reparse(it->path()))folders.push_back(it->path());
    unsigned removed=0;
    for(const auto& folder:folders){
        const auto mine=marker(folder);if(!mine)continue; // never touch a folder we did not create
        if(!remove(*parse_version(mine->first)))continue;
        remove_own_files(folder,mine->second);
        // Anything else inside (put there by someone else) keeps the folder and its marker.
        bool only_marker=true;
        for(fs::directory_iterator it(folder,ec),end;!ec&&it!=end;it.increment(ec))if(it->path().filename()!=kMarker)only_marker=false;
        if(ec||!only_marker)continue;
        DeleteFileW((folder/kMarker).c_str());
        if(RemoveDirectoryW(folder.c_str()))++removed;
    }
    return removed;
}
}
unsigned cleanup_downloads(const fs::path& data_root,const std::string& keep){
    return cleanup_if(data_root,[&](const Version& v){return v.text()!=keep;});}
unsigned cleanup_installed(const fs::path& data_root,const Version& running){
    return cleanup_if(data_root,[&](const Version& v){return compare(v,running)<=0;});}
fs::path download_setup(const Endpoint& e,const Offer& o,const fs::path& data_root,const Cancel* cancel,Progress* progress){
    need(o.state==Offer::State::available,"没有可安装的签名更新");
    const auto& file=o.manifest.setup;need(plain_name(file.name,".exe"),"安装程序文件名无效");
    need(o.setup.name==file.name&&o.setup.size==file.size,"发布附件与签名清单不一致");
    const auto root=updates_dir(data_root);std::error_code ec;
    need(data_root.is_absolute()&&fs::is_directory(data_root,ec),"数据目录不存在");
    if(!fs::exists(root,ec))need(CreateDirectoryW(root.c_str(),nullptr)!=FALSE||GetLastError()==ERROR_ALREADY_EXISTS,"无法创建更新目录");
    winpath::require_no_reparse(root);need(fs::is_directory(root,ec),"更新目录无效");
    const auto folder=root/wide(o.manifest.version);
    if(fs::exists(folder,ec)){need(!reparse(folder)&&marker(folder).has_value(),"更新目录 "+utf8(folder.wstring())+" 已存在且不是釉光创建的，未改动");}
    else{need(CreateDirectoryW(folder.c_str(),nullptr)!=FALSE,"无法创建更新目录");
        write_atomic(folder/kMarker,json{{"kind",kMarkerKind},{"version",o.manifest.version},{"setup",file.name}}.dump());}
    {const auto mine=marker(folder);need(mine.has_value(),"更新目录记录无效，未改动");
     if(mine->second!=file.name){ // the same version re-signed with another Setup name: replace only our own files
        remove_own_files(folder,mine->second);
        write_atomic(folder/kMarker,json{{"kind",kMarkerKind},{"version",o.manifest.version},{"setup",file.name}}.dump());}}
    cleanup_downloads(data_root,o.manifest.version);
    const auto available=free_space(folder);
    need(available>=space_needed(file.size),"磁盘空间不足：需要约 "+mib(space_needed(file.size))+"，剩余 "+mib(available));
    if(progress){progress->total=file.size;progress->received=0;}
    const auto target=folder/wide(file.name);
    if(fs::exists(target,ec)){
        try{need(!reparse(target),"安装程序路径无效");verify_file(target,file);if(progress)progress->received=file.size;return target;}
        catch(const Error&){DeleteFileW(target.c_str());}}
    const auto partial=fs::path(target.wstring()+L".partial");DeleteFileW(partial.c_str());
    Handle out(CreateFileW(partial.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    need(out.valid(),"无法写入下载文件");
    try{
        Sha256 hash;std::uint64_t written=0;bool local=false;
        // What the address delivered (or failed to) is FetchFailed; writing it here is not.
        try{get(e,o.setup.url,L"application/octet-stream",file.size,cancel,[&](const char* p,std::size_t n){
                try{DWORD count=0;need(WriteFile(out.value,p,DWORD(n),&count,nullptr)&&count==n,"写入下载文件失败");
                    hash.add(p,n);written+=n;if(progress)progress->received=written;}
                catch(...){local=true;throw;}});}
        catch(const Cancelled&){throw;}
        catch(const Error& x){if(local)throw;throw FetchFailed(x.what());}
        if(written!=file.size)throw FetchFailed("下载不完整（"+std::to_string(written)+" / "+std::to_string(file.size)+" 字节）");
        need(FlushFileBuffers(out.value)!=FALSE,"写入下载文件失败");
        if(hash.finish()!=file.sha256)throw FetchFailed("下载的安装程序 SHA-256 与签名清单不符，已删除");
        CloseHandle(out.value);out.value=INVALID_HANDLE_VALUE;
        need(MoveFileExW(partial.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"无法保存下载的安装程序");
    }catch(...){if(out.valid()){CloseHandle(out.value);out.value=INVALID_HANDLE_VALUE;}DeleteFileW(partial.c_str());throw;}
    verify_file(target,file);
    return target;
}

// ---- installation
Installation detect(const root::Location& where,const UninstallLocation& reader){
    Installation out;out.root=where.root;out.data=where.data;
    auto portable=[&](std::string why){out.why=std::move(why);return out;};
    if(where.source!=root::Source::app_layout)return portable("不是安装版的目录布局");
    const auto bytes=read_small(where.data/L"settings"/L"application-install.json",64*1024);
    if(!bytes)return portable("没有安装程序的安装记录");
    const auto j=json::parse(*bytes,nullptr,false);
    if(j.is_discarded()||!j.is_object()||!j.contains("schema")||j["schema"]!="overglaze-application-install-v1"||!j.contains("product")||j["product"]!="overglaze"||
       !j.contains("root")||!j["root"].is_string()||!winpath::same_spelling(wide(j["root"].get<std::string>()),where.root))return portable("安装记录指向另一个目录");
    std::error_code ec;if(!fs::is_regular_file(where.root/L"app"/L"unins000.exe",ec))return portable("没有卸载程序");
    const auto lookup=reader?reader():registry_install_location();
    if(!lookup.location||lookup.location->empty())return portable(uninstall_reason(lookup));
    auto location=*lookup.location;
    while(location.size()>3&&(location.back()==L'\\'||location.back()==L'/'))location.pop_back();
    if(!winpath::same_spelling(location,where.root))return portable("Windows 卸载登记指向另一个目录");
    out.by_setup=true;return out;
}
Installation detect_self(){
    try{return detect(root::resolve_self());}
    catch(const std::exception& e){Installation out;out.why=std::string("无法确定程序目录：")+e.what();return out;}}
std::string install_refusal(const Installation& at,std::uint64_t setup_size){
    if(!at.by_setup)return "这是便携版（"+at.why+"），不会自动安装；请从下载页获取新版本。";
    const auto check=games::app_maintenance_check(at.root,"update");
    if(!check.allowed)return check.message;
    try{const auto available=free_space(at.root);
        if(available<space_needed(setup_size))return "磁盘空间不足：需要约 "+mib(space_needed(setup_size))+"，剩余 "+mib(available);}
    catch(const std::exception& e){return e.what();}
    return {};
}
std::wstring setup_command_line(const fs::path& setup,const fs::path& root,DWORD pid){
    return L"\""+setup.native()+L"\" /SILENT /SUPPRESSMSGBOXES /NORESTART /DIR=\""+root.native()+L"\" /WAITPID="+std::to_wstring(pid)+L" /RELAUNCH";}
void launch_setup(const fs::path& setup,const FileRef& expected,const fs::path& root,DWORD pid){
    winpath::require_no_reparse(setup);
    // Held against writes from the last verification until Windows has opened it.
    Handle hold(CreateFileW(setup.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    need(hold.valid(),"无法读取下载的安装程序");
    verify_handle(hold.value,expected);
    auto command=setup_command_line(setup,root,pid);std::vector<wchar_t> line(command.begin(),command.end());line.push_back(L'\0');
    STARTUPINFOW startup{};startup.cb=sizeof startup;PROCESS_INFORMATION process{};
    const auto directory=setup.parent_path();
    if(!CreateProcessW(setup.c_str(),line.data(),nullptr,nullptr,FALSE,0,nullptr,directory.c_str(),&startup,&process))
        fail("无法启动安装程序（Windows "+std::to_string(GetLastError())+"）");
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
}

// ---- running version
std::optional<std::string> product_version(const fs::path& exe){
    DWORD ignored=0;const auto size=GetFileVersionInfoSizeW(exe.c_str(),&ignored);if(!size)return std::nullopt;
    std::vector<unsigned char> data(size);if(!GetFileVersionInfoW(exe.c_str(),0,size,data.data()))return std::nullopt;
    std::vector<std::wstring> keys;
    struct Language {WORD language,codepage;};Language* languages=nullptr;UINT bytes=0;
    if(VerQueryValueW(data.data(),L"\\VarFileInfo\\Translation",reinterpret_cast<void**>(&languages),&bytes))
        for(UINT i=0;i<bytes/sizeof(Language);++i){wchar_t key[64];swprintf_s(key,L"\\StringFileInfo\\%04x%04x\\ProductVersion",languages[i].language,languages[i].codepage);keys.push_back(key);}
    keys.push_back(L"\\StringFileInfo\\040904b0\\ProductVersion");
    for(const auto& key:keys){wchar_t* value=nullptr;UINT length=0;
        if(VerQueryValueW(data.data(),key.c_str(),reinterpret_cast<void**>(&value),&length)&&value&&length){
            std::wstring text(value,length);while(!text.empty()&&text.back()==L'\0')text.pop_back();if(!text.empty())return utf8(text);}}
    return std::nullopt;
}
std::string running_version(){
    const auto viewer=root::self_executable().parent_path()/L"overglaze_viewer.exe";
    const auto version=product_version(viewer);
    need(version&&parse_version(*version),"无法读取当前程序版本");return *version;}
json offer_json(const Offer& o,const std::string& running,const Installation& at){
    const char* state=o.state==Offer::State::available?"available":o.state==Offer::State::manual_only?"manual-only":"up-to-date";
    json out={{"schema","overglaze-self-update-v1"},{"running",running},{"state",state},
        {"installed_by_setup",at.by_setup},{"program_root",utf8(at.root.wstring())}};
    if(!at.by_setup)out["portable_reason"]=at.why;
    if(o.state!=Offer::State::up_to_date){out["version"]=o.version;out["page"]=o.page;}
    if(o.state==Offer::State::available){
        out["manifest"]={{"version",o.manifest.version},{"channel",o.manifest.channel},{"published",o.manifest.published},{"source_commit",o.manifest.source_commit},
            {"notes_zh",o.manifest.notes_zh},{"setup",{{"name",o.manifest.setup.name},{"size",o.manifest.setup.size},{"sha256",o.manifest.setup.sha256}}},
            {"portable",{{"name",o.manifest.portable.name},{"size",o.manifest.portable.size},{"sha256",o.manifest.portable.sha256}}}};
        out["signature"]="verified with the release key "+std::string(kReleaseKeySha256).substr(0,16)+"…";}
    out["scope"]="Reads public GitHub release data only; sends nothing about the user, games or files. An update is installed only from a manifest signed with the release key, by a copy installed by Setup.";
    return out;
}

// ---- settings
json Settings::document()const{
    json d={{"kind","overglaze-update-settings"},{"version",1},{"check_on_start",check_on_start},{"last_check_utc",last_check}};
    if(!last_result.empty())d["last_result"]=last_result;
    if(!pending_to.empty())d["pending"]={{"from",pending_from},{"to",pending_to}};
    return d;}
Settings Settings::parse(const json& j){
    Settings s;if(!j.is_object()||!j.contains("kind")||j["kind"]!="overglaze-update-settings")return s;
    if(j.contains("check_on_start")&&j["check_on_start"].is_boolean())s.check_on_start=j["check_on_start"].get<bool>();
    if(j.contains("last_check_utc")&&j["last_check_utc"].is_number_integer())s.last_check=j["last_check_utc"].get<std::int64_t>();
    if(j.contains("last_result")&&j["last_result"].is_string()&&j["last_result"].get<std::string>().size()<=1024)s.last_result=j["last_result"].get<std::string>();
    if(j.contains("pending")&&j["pending"].is_object()){const auto& p=j["pending"];
        if(p.contains("from")&&p["from"].is_string()&&p.contains("to")&&p["to"].is_string()&&parse_version(p["to"].get<std::string>())){
            s.pending_from=p["from"].get<std::string>();s.pending_to=p["to"].get<std::string>();}}
    return s;}
bool automatic_check_due(const Settings& s,std::int64_t now){
    return s.check_on_start&&(s.last_check<=0||now<s.last_check||now-s.last_check>=kAutomaticInterval);}
AfterUpdate after_update(const Settings& s,const Version& running){
    if(s.pending_to.empty())return AfterUpdate::none;
    const auto to=parse_version(s.pending_to);if(!to)return AfterUpdate::none;
    if(compare(running,*to)>=0)return AfterUpdate::updated;
    const auto from=parse_version(s.pending_from);
    if(from&&compare(running,*from)==0)return AfterUpdate::failed;
    return AfterUpdate::none;}
SettingsStore::SettingsStore(fs::path data_root){
    if(data_root.empty())return;
    file_=data_root/L"settings"/L"update-v1.json";
    if(const auto bytes=read_small(file_,16*1024)){const auto j=json::parse(*bytes,nullptr,false);if(!j.is_discarded())settings_=Settings::parse(j);}
}
Settings SettingsStore::get()const{std::lock_guard g(lock_);return settings_;}
std::string SettingsStore::error()const{std::lock_guard g(lock_);return error_;}
void SettingsStore::update(const std::function<void(Settings&)>& change){std::lock_guard g(lock_);change(settings_);save_locked();}
void SettingsStore::save_locked(){
    if(file_.empty())return;
    try{save_setting(file_,settings_.document().dump(2)+"\n");error_.clear();}
    catch(const std::exception& e){error_=e.what();}
}
}
