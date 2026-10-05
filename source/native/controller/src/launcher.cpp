// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// overglaze_launch.exe -- the Steam launch-option launcher for games that refuse
// any Lab file in their own directory (RE9: even a DLL that does
// nothing, loaded from the game directory, crashes it at start).
//
// Steam launch options:  "<root>\app\overglaze_launch.exe" %command%
//
// It starts the game with exactly the command Steam gave, then waits OUTSIDE the
// game for the player's first Insert press while the game window is in front,
// and only then injects the late-loading controller from the game's overglaze\
// payload -- the same injection, the same gates, the same moment as a manual
// `overglaze_games attach` once in play. The panel opens by itself.
//
// Safety order: the game always starts. A game that is not a registered,
// installed late-loading package is launched and left alone; any refusal is
// written to the log and the game keeps running. The launcher waits for the game
// to exit so Steam sees the game as running for as long as it is.
#include "lab_attach.hpp"
#include "lab_game_manager.hpp"
#include "lab_launch_command.hpp"
#include "lab_brand.hpp"
#include "lab_root_locator.hpp"
#include <windows.h>
#include <chrono>
#include <fstream>
#include <optional>

namespace {
namespace fs=std::filesystem;

struct Log {
    std::ofstream out;
    // No Lab root, no log: never a directory relative to wherever Steam started us.
    explicit Log(const fs::path& root){
        if(root.empty())return;
        std::error_code ec;const auto dir=root/"data"/"launcher";fs::create_directories(dir,ec);
        SYSTEMTIME t{};GetLocalTime(&t);wchar_t name[40];std::swprintf(name,std::size(name),L"launch-%04u%02u%02u.log",t.wYear,t.wMonth,t.wDay);
        out.open(dir/name,std::ios::app|std::ios::binary);
    }
    void line(const std::string& s){
        if(!out)return;SYSTEMTIME t{};GetLocalTime(&t);char stamp[32];
        std::snprintf(stamp,sizeof(stamp),"%02u:%02u:%02u.%03u ",t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
        out<<stamp<<s<<'\n';out.flush();
    }
};

bool same_file(const fs::path& a,const fs::path& b){
    return _wcsicmp(a.lexically_normal().wstring().c_str(),b.lexically_normal().wstring().c_str())==0;
}
fs::path image_of(DWORD pid){
    const HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!h)return {};
    wchar_t b[MAX_PATH*2];DWORD n=static_cast<DWORD>(std::size(b));const BOOL ok=QueryFullProcessImageNameW(h,0,b,&n);CloseHandle(h);
    return ok?fs::path(std::wstring(b,n)):fs::path();
}

// The same target resolution `overglaze_games watch` uses: a registered entry,
// installed, whose package loads late. Gates come from the package, never from us.
struct Target {std::string id,package;fs::path exe,loader;lab::games::AttachGates gates;};
std::optional<Target> resolve(const fs::path& root,const fs::path& exe,std::string& why){
    lab::games::Manager m(root);
    for(const auto& e:m.list()){
        if(!same_file(e.exe,exe))continue;
        const auto s=m.inspect(e);
        if(!s.installed){why="registered but not installed";return std::nullopt;}
        const lab::games::Policy* policy=nullptr;
        for(const auto& c:m.policies())if(c.name==s.package)policy=&c;
        if(!policy){why="no package for this game";return std::nullopt;}
        if(policy->legacy){why="installed by the pre-rename build (DLSS Lab); migrate it in the game manager first";return std::nullopt;}
        if(policy->loader.strategy!="late_d3d12"){why="package "+policy->name+" does not load late ("+policy->loader.strategy+"); the game loads Lab itself";return std::nullopt;}
        Target t;t.id=e.id;t.package=s.package;t.exe=e.exe;
        t.loader=(e.exe.parent_path()/policy->loader.subdir/lab::wide(policy->loader.basename)).lexically_normal();
        t.gates.expected_executable=e.exe;
        t.gates.expected_executable_sha256=policy->pins.at(policy->executable);
        t.gates.expected_loader_sha256=policy->payload.at(policy->loader.basename);
        return t;
    }
    why="not a registered game";return std::nullopt;
}
// Any live process of this game: the one we started, or one it handed over to.
DWORD game_process(const Target& t,HANDLE started){
    DWORD code=0;if(started&&GetExitCodeProcess(started,&code)&&code==STILL_ACTIVE)return GetProcessId(started);
    return lab::games::find_process(t.exe);
}
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    // <root>\app\overglaze_launch.exe, or overglaze-root.json beside it. A root that
    // cannot be resolved only means no log and no attach: the game still starts.
    fs::path root;std::string root_refused;
    try{root=lab::root::resolve_self().root;}catch(const std::exception& e){root_refused=e.what();}
    Log log(root);
    const auto command=lab::launch::rest_of_command_line(GetCommandLineW());
    if(command.empty()){
        MessageBoxW(nullptr,lab::brand::kLauncherUsage,lab::brand::kLauncherTitle,MB_OK|MB_ICONINFORMATION);
        return 2;
    }
    const fs::path exe=lab::launch::program_of(command);
    log.line("command: "+lab::utf8(command));
    std::optional<Target> target;std::string why=root_refused;
    if(!root.empty())try{target=resolve(root,exe,why);}catch(const std::exception& e){why=e.what();}
    // The game starts whatever Lab thinks of it.
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    std::wstring line=command;
    if(!CreateProcessW(nullptr,line.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){
        const auto error=GetLastError();log.line("CreateProcess failed, Win32="+std::to_string(error));
        MessageBoxW(nullptr,(L"无法启动游戏（Win32 错误 "+std::to_wstring(error)+L"）：\n"+command).c_str(),lab::brand::kLauncherTitle,MB_OK|MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    log.line("started pid="+std::to_string(pi.dwProcessId)+(target?" target="+target->package:" passthrough: "+why));
    if(!target){WaitForSingleObject(pi.hProcess,INFINITE);DWORD code=0;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hProcess);log.line("game exited, passthrough");return static_cast<int>(code);}

    // Wait for the player's Insert with the game in front, then inject once.
    bool attached=false,was_down=true;HANDLE open_panel=nullptr;unsigned misses=0;
    for(;;){
        const DWORD pid=game_process(*target,pi.hProcess);
        if(!pid){
            // Allow a short handover (a game that restarts itself) before giving up.
            if(++misses>20)break;
            Sleep(250);continue;
        }
        misses=0;
        if(attached){Sleep(1000);continue;}
        Sleep(50);
        const bool down=(GetAsyncKeyState(VK_INSERT)&0x8000)!=0;
        const bool pressed=down&&!was_down;was_down=down;
        if(!pressed)continue;
        DWORD owner=0;const HWND window=GetForegroundWindow();
        if(!window||!GetWindowThreadProcessId(window,&owner)||!same_file(image_of(owner),target->exe)){log.line("Insert pressed, game not in front; ignored");continue;}
        // Ask the host to open the panel: this press was for it.
        if(!open_panel)open_panel=CreateEventW(nullptr,TRUE,TRUE,lab::launch::open_panel_event_name(owner).c_str());
        try{
            const auto r=lab::games::attach(owner,target->loader,target->gates);
            attached=true;
            log.line("attached pid="+std::to_string(r.pid)+" loader="+r.loader_sha256+" loaded="+(r.loaded?"true":"false")+" note="+r.note);
        }catch(const std::exception& e){
            // Most refusals mean "not yet" (renderer not created); the next Insert retries.
            log.line(std::string("attach refused: ")+e.what());
        }
    }
    DWORD code=0;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hProcess);
    if(open_panel)CloseHandle(open_panel);
    log.line(std::string("game exited")+(attached?"":" without an attach"));
    return static_cast<int>(code);
}
