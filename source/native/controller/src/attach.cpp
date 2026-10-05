// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Injecting the late-loading controller into a game the user already started.
//
// This is a deliberately boring injector: CreateRemoteThread(LoadLibraryW) with
// an absolute path, into a process the user owns, in the same session, with no
// anti-cheat anywhere near it. There is no manual mapping, no hollowing and no
// attempt to hide: the DLL appears in the module list exactly as ReShade's would.
//
// Every gate is here rather than in the injected DLL, because a refusal is only
// useful if it happens before anything is written into another process. The DLL
// itself refuses again on its own terms (it needs a validated installation
// beside it), so neither side trusts the other.
#include "lab_attach.hpp"
#include "lab_windows_path.hpp"
#include <tlhelp32.h>
#include <psapi.h>

namespace lab::games {
namespace {
void need(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}

struct Handle {
    HANDLE value=nullptr;
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle()=default;explicit Handle(HANDLE h):value(h){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};

// Is this module already in the target? Injecting a second copy would start a
// second host in one process.
bool module_present(DWORD pid,const std::filesystem::path& dll){
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));
    if(snapshot.value==INVALID_HANDLE_VALUE)return false;
    MODULEENTRY32W entry{sizeof(entry)};
    if(!Module32FirstW(snapshot.value,&entry))return false;
    do{
        std::error_code ec;
        if(winpath::same_spelling(std::filesystem::path(entry.szExePath).filename(),dll.filename())&&
           std::filesystem::exists(entry.szExePath,ec))return true;
    }while(Module32NextW(snapshot.value,&entry));
    return false;
}

bool has_module(DWORD pid,const wchar_t* name){
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));
    if(snapshot.value==INVALID_HANDLE_VALUE)return false;
    MODULEENTRY32W entry{sizeof(entry)};
    if(!Module32FirstW(snapshot.value,&entry))return false;
    do{if(winpath::same_spelling(std::filesystem::path(entry.szModule),std::filesystem::path(name)))return true;}
    while(Module32NextW(snapshot.value,&entry));
    return false;
}

struct WindowSearch {DWORD pid=0;bool found=false;};
BOOL CALLBACK visible_window(HWND window,LPARAM param){
    auto* search=reinterpret_cast<WindowSearch*>(param);
    DWORD owner=0;GetWindowThreadProcessId(window,&owner);
    if(owner==search->pid&&IsWindowVisible(window)&&GetWindow(window,GW_OWNER)==nullptr){search->found=true;return FALSE;}
    return TRUE;
}
}

DWORD find_process(const std::filesystem::path& executable){
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if(!snapshot.value||snapshot.value==INVALID_HANDLE_VALUE)return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    if(!Process32FirstW(snapshot.value,&entry))return 0;
    const auto self=GetCurrentProcessId();
    do{
        if(entry.th32ProcessID==self)continue;
        if(!winpath::same_spelling(entry.szExeFile,executable.filename()))continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID));
        if(!process.value)continue; // a name match we cannot verify is not a match
        std::wstring image(32768,0);DWORD size=static_cast<DWORD>(image.size());
        if(!QueryFullProcessImageNameW(process.value,0,image.data(),&size))continue;
        image.resize(size);
        if(winpath::same_spelling(image,executable.wstring()))return entry.th32ProcessID;
    }while(Process32NextW(snapshot.value,&entry));
    return 0;
}
AttachResult attach(DWORD pid,const std::filesystem::path& loader,const AttachGates& gates){
    AttachResult out;out.pid=pid;
    need(pid!=0&&pid!=GetCurrentProcessId(),"Attach needs another process");
    need(loader.is_absolute()&&loader==loader.lexically_normal(),"Loader path must be absolute and normalized");
    winpath::require_no_reparse(loader);
    need(std::filesystem::is_regular_file(loader),"Loader DLL is not where the installation says it is");
    need(loader.filename()==L"overglaze_controller.dll","Only the late-loading controller may be injected");
    out.loader_sha256=sha256(loader);
    need(out.loader_sha256==gates.expected_loader_sha256,"Loader identity differs from the adapter package");

    // The process must be the game we registered, and it must already be a
    // running D3D12 game with a window: injecting before the renderer exists
    // would make discovery refuse anyway, and there would be no swapchain.
    Handle process(OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid));
    need(process.value!=nullptr,"Cannot open the game process; it may need the same elevation as this tool");
    std::wstring image(32768,L'\0');DWORD size=DWORD(image.size());
    need(QueryFullProcessImageNameW(process.value,0,image.data(),&size)!=0,"Cannot read the game's image path");
    image.resize(size);
    out.executable=image;
    need(winpath::same_spelling(std::filesystem::path(image),gates.expected_executable),"That PID is not the registered game");
    need(sha256(image)==gates.expected_executable_sha256,"The running game's identity differs from the adapter package");

    need(has_module(pid,L"d3d12.dll")&&has_module(pid,L"dxgi.dll"),"The game has not created its D3D12 renderer yet; start a scene first");
    WindowSearch search{pid,false};EnumWindows(&visible_window,reinterpret_cast<LPARAM>(&search));
    need(search.found,"The game has no visible top-level window yet");
    need(!module_present(pid,loader),"The controller is already loaded in that process");

    // Write the absolute path into the target and call LoadLibraryW on it.
    // kernel32 is at the same address in every process of a session, so the
    // address resolved here is the one the target will call.
    const auto kernel32=GetModuleHandleW(L"kernel32.dll");
    const auto load=reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(GetProcAddress(kernel32,"LoadLibraryW")));
    need(load!=nullptr,"LoadLibraryW unavailable");
    const auto text=loader.wstring();
    const auto bytes=(text.size()+1)*sizeof(wchar_t);
    auto* remote=VirtualAllocEx(process.value,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    need(remote!=nullptr,"Cannot reserve the path buffer in the game");
    SIZE_T written=0;
    const bool wrote=WriteProcessMemory(process.value,remote,text.c_str(),bytes,&written)!=0&&written==bytes;
    if(!wrote){VirtualFreeEx(process.value,remote,0,MEM_RELEASE);need(false,"Cannot write the loader path into the game");}
    Handle thread(CreateRemoteThread(process.value,nullptr,0,load,remote,0,nullptr));
    if(!thread.value){VirtualFreeEx(process.value,remote,0,MEM_RELEASE);need(false,"Cannot start the loader thread in the game");}
    const auto waited=WaitForSingleObject(thread.value,gates.wait_ms);
    DWORD result=0;GetExitCodeThread(thread.value,&result);
    VirtualFreeEx(process.value,remote,0,MEM_RELEASE);
    need(waited==WAIT_OBJECT_0,"The loader thread did not finish in time; inspect the game, do not inject again");
    // LoadLibraryW returns the module handle, truncated to 32 bits here. Zero is
    // the only unambiguous failure; a non-zero value means the image loaded.
    need(result!=0,"LoadLibraryW refused the loader inside the game");
    out.loaded=true;
    out.note="Loaded. The controller decides for itself whether this process has a validated installation; "
             "a handshake over the control pipe is what confirms it started.";
    return out;
}
}
