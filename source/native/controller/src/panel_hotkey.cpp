// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_panel_hotkey_file.hpp"
#include "lab_platform.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace lab::hotkey {
namespace fs=std::filesystem;
namespace {
std::string win32(const char* what,DWORD code){return std::string(what)+"（错误 "+std::to_string(code)+"）";}
// What a look at the file found, for "did it change since the last read".
struct Stamp {
    bool exists=false;std::uint64_t size=0,written=0;
    bool operator==(const Stamp&) const=default;
};
std::optional<Stamp> stamp(const fs::path& file){
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if(!GetFileAttributesExW(file.c_str(),GetFileExInfoStandard,&a)){
        const auto e=GetLastError();
        if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)return Stamp{};
        return std::nullopt; // cannot tell now; look again later
    }
    return Stamp{true,(std::uint64_t(a.nFileSizeHigh)<<32)|a.nFileSizeLow,(std::uint64_t(a.ftLastWriteTime.dwHighDateTime)<<32)|a.ftLastWriteTime.dwLowDateTime};
}
}
const char* source_name(Source s) noexcept {
    switch(s){case Source::defaulted:return "default";case Source::file:return "file";case Source::malformed:return "malformed";case Source::memory:return "memory";}
    return "default";
}
fs::path settings_file(const fs::path& data_root){return data_root/L"settings"/kFileName;}
std::string name(const Binding& b){
    char text[kMaxNameBytes+1]{};
    if(!format(b,text,sizeof text))(void)format(kDefault,text,sizeof text);
    return text;
}
ReadResult read(const fs::path& data_root){
    ReadResult r;
    try{
        const auto file=settings_file(data_root);
        const HANDLE h=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(h==INVALID_HANDLE_VALUE){
            const auto e=GetLastError();
            if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)return r; // no file: the default
            r.note=win32("无法读取面板快捷键设置",e);r.transient=true;return r;
        }
        Handle owned(h);
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(h,&info)){r.note=win32("无法读取面板快捷键设置",GetLastError());r.transient=true;return r;}
        if(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)){r.source=Source::malformed;r.note="面板快捷键设置不是普通文件，已使用默认的 Insert。";return r;}
        if(info.nFileSizeHigh||info.nFileSizeLow>kMaxFileBytes){r.source=Source::malformed;r.note="面板快捷键设置文件过大，已使用默认的 Insert。";return r;}
        char text[kMaxFileBytes]{};DWORD got=0;
        if(!ReadFile(h,text,DWORD(sizeof text),&got,nullptr)){r.note=win32("无法读取面板快捷键设置",GetLastError());r.transient=true;return r;}
        Binding b;
        if(!parse_document(text,got,b)){r.source=Source::malformed;r.note="面板快捷键设置无效，已使用默认的 Insert。";return r;}
        r.binding=b;r.source=Source::file;return r;
    }catch(const std::exception& e){r.note=std::string("无法读取面板快捷键设置：")+e.what();r.transient=true;return r;}
}
void write(const fs::path& data_root,const Binding& b){
    if(const auto why=check(b);why!=Refusal::none)throw std::runtime_error(refusal_text(why));
    char text[512]{};const auto bytes=write_document(b,text,sizeof text);
    if(!bytes)throw std::runtime_error("面板快捷键无法写成设置文件");
    const auto folder=data_root/L"settings";
    if(!CreateDirectoryW(folder.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)throw std::runtime_error(win32("无法创建设置目录",GetLastError()));
    const auto file=settings_file(data_root);
    const auto temp=file.wstring()+L".tmp-"+wide(uuid());
    {
        Handle out(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!out.valid())throw std::runtime_error(win32("无法写入面板快捷键设置",GetLastError()));
        DWORD written=0;
        const bool ok=WriteFile(out.value,text,DWORD(bytes),&written,nullptr)&&written==bytes&&FlushFileBuffers(out.value);
        const auto error=GetLastError();
        if(!ok){CloseHandle(out.value);out.value=INVALID_HANDLE_VALUE;DeleteFileW(temp.c_str());throw std::runtime_error(win32("无法写入面板快捷键设置",error));}
    }
    // Another program may be reading the file at this instant (the host, the
    // launcher and the desktop program all do); a reader that does not share
    // deletion would hold the rename off for a moment.
    for(unsigned attempt=0;;++attempt){
        if(MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return;
        const auto e=GetLastError();
        if(attempt<20&&(e==ERROR_ACCESS_DENIED||e==ERROR_SHARING_VIOLATION||e==ERROR_LOCK_VIOLATION)){Sleep(25);continue;}
        DeleteFileW(temp.c_str());throw std::runtime_error(win32("无法保存面板快捷键设置",e));
    }
}

struct Settings::Impl {
    fs::path root,file;
    std::atomic<std::uint32_t> packed{pack(kDefault)};
    std::atomic<std::uint64_t> revision{0};
    std::atomic<int> source{int(Source::defaulted)};
    mutable std::mutex mutex;std::condition_variable wake;std::thread worker;
    std::optional<Binding> pending;bool closing=false,writing=false;std::string error;
    std::optional<Stamp> seen; // what the last successful read or write left
    void publish(const Binding& b,Source s){
        const auto v=pack(b);
        if(packed.exchange(v)!=v)++revision;
        source=int(s);
    }
    void apply(const ReadResult& r){
        if(r.transient){std::lock_guard lock(mutex);error=r.note;return;} // keep the last value; look again
        publish(r.binding,r.source);std::lock_guard lock(mutex);error=r.note;
    }
    void poll(){
        const auto now=stamp(file);
        if(!now)return;
        if(seen&&*seen==*now)return;
        const auto r=read(root);apply(r);
        if(!r.transient)seen=now;
    }
    void run(){
        std::unique_lock lock(mutex);
        for(;;){
            wake.wait_for(lock,std::chrono::milliseconds(500),[&]{return closing||pending.has_value();});
            if(pending){
                const auto b=*pending;pending.reset();writing=true;lock.unlock();
                std::string failure;
                try{write(root,b);seen=stamp(file);}catch(const std::exception& e){failure=e.what();}
                lock.lock();writing=false;
                if(!failure.empty())error="面板快捷键未保存："+failure;else error.clear();
                wake.notify_all();
                continue; // a write queued meanwhile goes first
            }
            if(closing)break;
            lock.unlock();
            try{poll();}catch(...){}
            lock.lock();
        }
    }
};
Settings::Settings(fs::path data_root):p_(std::make_unique<Impl>()){
    auto& s=*p_;
    if(data_root.empty()){s.source=int(Source::memory);return;}
    s.root=std::move(data_root);s.file=settings_file(s.root);
    s.seen=stamp(s.file);
    const auto r=read(s.root);
    s.publish(r.binding,r.source);s.error=r.note;
    if(r.transient)s.seen.reset(); // read again on the first look
    s.revision=0;
    s.worker=std::thread([&s]{s.run();});
}
Settings::~Settings(){close();}
Binding Settings::current() const noexcept {return unpack(p_->packed.load());}
std::string Settings::current_name() const {return name(current());}
Source Settings::source() const noexcept {return Source(p_->source.load());}
std::uint64_t Settings::revision() const noexcept {return p_->revision.load();}
Refusal Settings::set(const Binding& b) noexcept {
    const auto why=check(b);if(why!=Refusal::none)return why;
    auto& s=*p_;
    if(s.root.empty()){s.publish(b,Source::memory);return Refusal::none;}
    s.publish(b,Source::file);
    try{std::lock_guard lock(s.mutex);if(!s.closing){s.pending=b;s.wake.notify_all();}}catch(...){}
    return Refusal::none;
}
std::string Settings::error() const {std::lock_guard lock(p_->mutex);return p_->error;}
void Settings::close() noexcept {
    auto& s=*p_;
    try{
        {std::unique_lock lock(s.mutex);
         s.closing=true;s.wake.notify_all();
         // A write already asked for is finished before the worker stops.
         s.wake.wait_for(lock,std::chrono::seconds(5),[&]{return !s.pending&&!s.writing;});}
        if(s.worker.joinable())s.worker.join();
    }catch(...){}
}
}
