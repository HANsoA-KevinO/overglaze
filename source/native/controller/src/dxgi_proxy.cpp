// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// dxgi.dll — the root proxy, and the only Lab file that sits in a game's own
// directory under the root strategy.
//
// This file exists because of what it does NOT do. The game imports dxgi.dll, so
// Windows maps this module and resolves ITS imports while the loader lock is
// held and before a single line of the game's code has run; the application
// compatibility shim engine also calls straight into these exports at that
// moment. The previous controller dxgi.dll linked the whole host and therefore
// dragged d3d12, the shader compiler, WinTrust, OLE32, Shell32 and the entire
// MSVC runtime into that window -- 25 modules, one of which was dxgi.dll itself.
// Alan Wake 2 died there.
//
// So: no host, no Lab library, no C++ runtime DLL, no allocation, no file I/O
// and no exceptions on any path that can run before the renderer exists. The
// real controller is a separate DLL in the payload subdirectory, loaded by a
// worker thread this DLL starts -- off the loader lock, on a thread we own.
// NOTHING reachable from an export may touch the loader: Alan Wake 2 reaches
// these exports through the compatibility shim engine while the loader lock is
// held, and a LoadLibrary there deadlocked the process.
//
// Safety property, in order of priority: if anything about Lab is broken,
// missing or refused, this file must still be a perfect DXGI forwarder. A game
// with a damaged payload loses NR, never its renderer.
//
// Two modes, chosen by the installation beside the host (root_proxy_on_insert,
// made for RE9). Normally the host is loaded at once and handed the game's
// first factory. On Insert, nothing Lab is loaded while the game starts: the
// worker only watches for the player's first press of the panel hotkey (Insert
// unless changed; <data root>\settings\panel-hotkey-v1.json, read with the
// shared header-only rules and looked at again every two seconds) in this
// process's own foreground window, and then loads the host, which comes up as
// a late attach.
// RE9 died with the host present from start-up (after a few hundred frames)
// and has lived through every attach made once in play.
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
// Header-only, allocation-free and file-free: the same parser and press rule
// as the host, the launcher and the desktop program, without linking any of them.
#include "lab_panel_hotkey.hpp"

namespace {
// 0x887A0022. Spelled out rather than included so this translation unit needs no
// DXGI header and can never acquire an import from one.
constexpr std::uint32_t kNotCurrentlyAvailable=0x887A0022u;

constexpr const wchar_t* kPayloadSubdirectory=L"overglaze";
constexpr const wchar_t* kHostName=L"overglaze_controller.dll";
constexpr const wchar_t* kInstallationName=L"overglaze.install.json";

INIT_ONCE resolve_once=INIT_ONCE_STATIC_INIT;
INIT_ONCE host_once=INIT_ONCE_STATIC_INIT;
std::array<FARPROC,20> exports{};
// Index order is the .def file's order, which is also the ASM thunk order.
constexpr const char* names[]{"ApplyCompatResolutionQuirking","CompatString","CompatValue","CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2",
    "DXGID3D10CreateDevice","DXGID3D10CreateLayeredDevice","DXGID3D10GetLayeredDeviceSize","DXGID3D10RegisterLayers","DXGIDeclareAdapterRemovalSupport",
    "DXGIDisableVBlankVirtualization","DXGIDumpJournal","DXGIGetDebugInterface1","DXGIReportAdapterConfiguration","PIXBeginCapture","PIXEndCapture","PIXGetCaptureState",
    "SetAppCompatStringPointer","UpdateHMDEmulationStatus"};

// Atomics only. This state is written from paths that can run under the loader
// lock, from the shim engine, on threads we do not own.
std::atomic<unsigned> attempts{0},failures{0},fallbacks{0},stage{0},host_stage{0};
std::atomic<unsigned long> error{0},host_error{0};
std::atomic<bool> resolved{false},host_loaded{false},compat_pending{false},compat_replayed{false};
std::atomic<std::uint64_t> compat_args[4]{};
// Declared with void* rather than IUnknown*: ABI-identical, and it keeps this
// translation unit free of the COM headers entirely.
using FactoryCreated=void(__cdecl*)(void*,HMODULE);
std::atomic<FactoryCreated> host_factory{nullptr};
using StartLate=void(__cdecl*)(HMODULE,unsigned);
// 0 undecided, 1 host at the first factory, 2 host on the first Insert.
std::atomic<unsigned> mode{0},insert_presses{0},insert_stage{0};
// The hotkey the on-Insert worker watches (lab::hotkey::pack) and where it came
// from: 0 not read, 1 no file (default), 2 the file, 3 a malformed file
// (default), 4 no data root in the installation (default), 5 unreadable now
// (the previous binding kept).
std::atomic<std::uint32_t> watched_key{lab::hotkey::pack(lab::hotkey::kDefault)};
std::atomic<unsigned> hotkey_source{0};
// Set once the worker has settled the host for now -- loaded, failed, or
// deferred to Insert -- so a factory never waits for a host that is not coming.
// Without it a failed host load would make every factory wait the full bound
// (about 5.5 s each); proxy_on_insert_tests guards this.
std::atomic<bool> host_settled{false};

HMODULE self_module() noexcept {
    HMODULE m{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&self_module),&m);
    return m;
}
bool system_copy(HMODULE m,const wchar_t* expected) noexcept {
    if(!m||m==self_module())return false;
    wchar_t actual[MAX_PATH*2];
    const auto n=GetModuleFileNameW(m,actual,static_cast<DWORD>(std::size(actual)));
    return n&&n<std::size(actual)&&_wcsicmp(expected,actual)==0;
}
void fail(unsigned where) noexcept {stage=where;error=GetLastError();++failures;}

BOOL CALLBACK resolve_system(PINIT_ONCE,void*,void**) noexcept {
    ++attempts;
    wchar_t path[MAX_PATH*2];
    const auto n=GetSystemDirectoryW(path,static_cast<UINT>(std::size(path)));
    if(!n||n+16>=std::size(path)){fail(1);return FALSE;}
    lstrcpyW(path+n,L"\\dxgi.dll");
    // Under an application-compatibility shim (AcGenral) the FIRST call into this
    // proxy can come from the shim engine while the loader is still resolving the
    // game's imports (Alan Wake 2). There the plain load may fail or
    // hand back this very module. Try the extended-length spelling and an already
    // mapped system copy before giving up for THIS call; the INIT_ONCE stays
    // unset, so the game's own later call retries with a quiescent loader.
    // Never fail-fast: that killed the game.
    HMODULE module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!system_copy(module,path)){
        const bool got_self=module&&module==self_module();
        wchar_t extended[MAX_PATH*2+8];
        lstrcpyW(extended,L"\\\\?\\");lstrcatW(extended,path);
        module=LoadLibraryExW(extended,nullptr,0);
        if(!system_copy(module,path)){
            HMODULE mapped{};
            if(!(GetModuleHandleExW(0,path,&mapped)&&system_copy(mapped,path))){fail(got_self?3:2);return FALSE;}
            module=mapped;
        }
    }
    std::array<FARPROC,20> found{};
    for(unsigned i=0;i<found.size();++i){found[i]=GetProcAddress(module,names[i]);if(!found[i]){fail(4);return FALSE;}}
    exports=found; // Kept for process lifetime. No renamed/copied system DLL is required.
    resolved.store(true,std::memory_order_release);
    return TRUE;
}
// Loader-SAFE resolution: adopts the system copy only if it is ALREADY mapped.
// GetModuleHandleExW never loads, so this is legal from any context, including a
// shim-engine callback made while the loader lock is held.
bool adopt_mapped() noexcept {
    if(resolved.load(std::memory_order_acquire))return true;
    wchar_t path[MAX_PATH*2];
    const auto n=GetSystemDirectoryW(path,static_cast<UINT>(std::size(path)));
    if(!n||n+16>=std::size(path))return false;
    lstrcpyW(path+n,L"\\dxgi.dll");
    HMODULE mapped{};
    if(!GetModuleHandleExW(0,path,&mapped)||!system_copy(mapped,path))return false;
    std::array<FARPROC,20> found{};
    for(unsigned i=0;i<found.size();++i){found[i]=GetProcAddress(mapped,names[i]);if(!found[i])return false;}
    exports=found;resolved.store(true,std::memory_order_release);return true;
}
// May LOAD. Legal only where the loader is known to be quiescent: our own worker
// thread, or a DXGI entry the game called from ordinary code. Never from an
// export the shim engine can reach during import resolution -- that is the
// deadlock this file was once hanging in on Alan Wake 2.
bool resolve_loading() noexcept {return resolved.load(std::memory_order_acquire)||InitOnceExecuteOnce(&resolve_once,resolve_system,nullptr,nullptr)!=FALSE;}

// <this module's directory>\overglaze\<name>. Worker thread only.
bool payload_path(wchar_t* path,unsigned capacity,const wchar_t* name) noexcept {
    const auto n=GetModuleFileNameW(self_module(),path,capacity);
    if(!n||n>=capacity)return false;
    auto* last=path;
    for(auto* p=path;*p;++p)if(*p==L'\\'||*p==L'/')last=p;
    if(last==path)return false;
    ++last;*last=L'\0';
    if(lstrlenW(path)+lstrlenW(kPayloadSubdirectory)+lstrlenW(name)+2>=static_cast<int>(capacity))return false;
    lstrcatW(path,kPayloadSubdirectory);lstrcatW(path,L"\\");lstrcatW(path,name);
    return true;
}
// The installation beside the host, as text. Worker thread only (static buffer).
char installation[64*1024];DWORD installation_size=0;
bool read_installation() noexcept {
    wchar_t path[MAX_PATH*2];installation_size=0;
    if(!payload_path(path,static_cast<unsigned>(std::size(path)),kInstallationName))return false;
    const HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD got=0;const BOOL read=ReadFile(file,installation,sizeof(installation),&got,nullptr);CloseHandle(file);
    if(!read)return false;
    installation_size=got;return true;
}
// Does the installation beside the host ask to be loaded on Insert? A plain
// search of our own installer's JSON for the strategy value; the host
// re-validates the whole installation once it is loaded. Anything unreadable
// keeps the original behaviour (the fixture has no installation at all).
bool installed_on_insert() noexcept {
    if(!read_installation())return false;
    constexpr char needle[]="\"root_proxy_on_insert\"";constexpr DWORD length=sizeof(needle)-1;
    for(DWORD i=0;i+length<=installation_size;++i){DWORD k=0;while(k<length&&installation[i+k]==needle[k])++k;if(k==length)return true;}
    return false;
}
// <output_root>\settings\panel-hotkey-v1.json, from the installation's own
// "output_root" (the data root the host itself uses).
bool hotkey_path(wchar_t* path,unsigned capacity) noexcept {
    static char root[MAX_PATH*4];
    if(!installation_size||!lab::hotkey::string_field(installation,installation_size,"output_root",root,sizeof(root)))return false;
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,root,-1,path,static_cast<int>(capacity));
    if(n<=1)return false;
    if(static_cast<unsigned>(n)+10+lstrlenW(lab::hotkey::kFileName)>=capacity)return false;
    lstrcatW(path,L"\\settings\\");lstrcatW(path,lab::hotkey::kFileName);
    return true;
}
// Missing or malformed: the default. Unreadable right now (another program is
// replacing it): the binding watched so far.
void refresh_hotkey(lab::hotkey::Binding& key) noexcept {
    static wchar_t path[MAX_PATH*2];static bool located=false,tried=false;
    if(!tried){tried=true;located=hotkey_path(path,static_cast<unsigned>(std::size(path)));}
    if(!located){key=lab::hotkey::kDefault;hotkey_source=4;watched_key=lab::hotkey::pack(key);return;}
    const HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE){
        const auto e=GetLastError();
        if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND){key=lab::hotkey::kDefault;hotkey_source=1;}else hotkey_source=5;
        watched_key=lab::hotkey::pack(key);return;
    }
    static char text[lab::hotkey::kMaxFileBytes];DWORD got=0;BY_HANDLE_FILE_INFORMATION info{};
    // 2 read, 3 not a plain small file (malformed), 5 unreadable now.
    unsigned outcome=5;
    if(GetFileInformationByHandle(file,&info)){
        if((info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nFileSizeHigh||info.nFileSizeLow>lab::hotkey::kMaxFileBytes)outcome=3;
        else if(ReadFile(file,text,static_cast<DWORD>(sizeof(text)),&got,nullptr))outcome=2;
    }
    CloseHandle(file);
    lab::hotkey::Binding parsed;
    if(outcome==2&&lab::hotkey::parse_document(text,got,parsed)){key=parsed;hotkey_source=2;}
    else if(outcome!=5){key=lab::hotkey::kDefault;hotkey_source=3;}
    else hotkey_source=5;
    watched_key=lab::hotkey::pack(key);
}
// Blocks the worker until the panel hotkey goes down while this process owns
// the foreground window. user32 is only looked up, never loaded or imported:
// any windowed game has it, and waiting for it keeps it out of the import table.
bool await_insert() noexcept {
    HMODULE user=nullptr;
    while(!(user=GetModuleHandleW(L"user32.dll")))Sleep(250);
    const auto key_state=reinterpret_cast<SHORT(WINAPI*)(int)>(GetProcAddress(user,"GetAsyncKeyState"));
    const auto foreground=reinterpret_cast<HWND(WINAPI*)()>(GetProcAddress(user,"GetForegroundWindow"));
    const auto owner=reinterpret_cast<DWORD(WINAPI*)(HWND,LPDWORD)>(GetProcAddress(user,"GetWindowThreadProcessId"));
    if(!key_state||!foreground||!owner){insert_stage=1;return false;}
    lab::hotkey::Binding key=lab::hotkey::kDefault;refresh_hotkey(key);
    insert_stage=2;
    lab::hotkey::PressWatch watch; // a binding already held when watching starts is not a press
    for(unsigned tick=1;;++tick){
        Sleep(50);
        if(tick%40==0)refresh_hotkey(key); // the desktop program may change it while the game runs
        if(watch.poll(key,key_state)){
            DWORD pid=0;const HWND window=foreground();
            if(window&&owner(window,&pid)&&pid==GetCurrentProcessId()){++insert_presses;insert_stage=3;return true;}
        }
    }
}

// The controller, loaded beside us in the payload subdirectory. Runs on the
// worker thread only, so the loader is quiescent and the calling thread is ours.
// A failure here costs NR and nothing else, so every exit is silent, records a
// stage for the host's status block, and the forwarder carries on regardless.
BOOL CALLBACK load_host(PINIT_ONCE,void*,void**) noexcept {
    wchar_t path[MAX_PATH*2];
    if(!payload_path(path,static_cast<unsigned>(std::size(path)),kHostName)){host_stage=3;host_error=GetLastError();return FALSE;}
    // The host and its own dependencies resolve from its directory, not ours.
    const auto module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DEFAULT_DIRS|LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if(!module){host_stage=4;host_error=GetLastError();return FALSE;}
    if(mode.load()==2){
        // On Insert: never the factory path. The host brings itself up late.
        const auto start=reinterpret_cast<StartLate>(GetProcAddress(module,"LabControllerStartLate"));
        if(!start){host_stage=6;host_error=GetLastError();return FALSE;}
        host_loaded.store(true,std::memory_order_release);
        start(self_module(),1u); // 1: open the panel once it attaches -- this Insert was for it
        return TRUE;
    }
    const auto entry=reinterpret_cast<FactoryCreated>(GetProcAddress(module,"LabControllerFactoryCreated"));
    if(!entry){host_stage=5;host_error=GetLastError();return FALSE;}
    host_factory.store(entry,std::memory_order_release);
    host_loaded.store(true,std::memory_order_release);
    return TRUE;
}
// Wait a bounded time for the worker to finish, then give up. Sleep touches no
// loader state, and the bound means a stuck worker costs NR, never the game.
template<class Ready> bool await(Ready ready,unsigned milliseconds) noexcept {
    for(unsigned waited=0;waited<milliseconds;++waited){if(ready())return true;Sleep(1);}
    return ready();
}
void offer_factory(void* factory) noexcept {
    if(!factory)return;
    // The worker normally published the entry long before the game built its
    // renderer. If it has not, wait briefly rather than loading on this thread.
    if(!host_loaded.load(std::memory_order_acquire))
        (void)await([]{return host_loaded.load(std::memory_order_acquire)||host_settled.load(std::memory_order_acquire);},3000);
    // Hand over our own module too. Once the system dxgi.dll is loaded there are
    // two modules with that base name, so the host must never look us up by name.
    if(auto* entry=host_factory.load(std::memory_order_acquire))entry(factory,self_module());
}
// The only thread allowed to touch the loader. Created by DllMain, which means
// its body does not start until the loader lock is released -- so everything it
// does happens on a quiescent loader, on a thread we own.
DWORD WINAPI worker(LPVOID) noexcept {
    resolve_loading();
    if(installed_on_insert()){
        mode=2;host_settled.store(true,std::memory_order_release);
        if(!await_insert())return 0; // no key API: NR stays unavailable, forwarding is untouched
        InitOnceExecuteOnce(&host_once,load_host,nullptr,nullptr);
        return 0;
    }
    mode=1;
    InitOnceExecuteOnce(&host_once,load_host,nullptr,nullptr);
    host_settled.store(true,std::memory_order_release);
    return 0;
}

// Once the system copy is loaded, hand it the compat string the shim engine
// tried to set while we could not forward. All four integer argument registers
// are replayed, so a longer real signature still receives what the shim passed.
void replay_compat_string() noexcept {
    if(!compat_pending.load(std::memory_order_acquire)||!compat_pending.exchange(false))return;
    if(auto fn=reinterpret_cast<void(WINAPI*)(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t)>(exports[18])){
        fn(compat_args[0].load(),compat_args[1].load(),compat_args[2].load(),compat_args[3].load());compat_replayed=true;
    }
}
// Exports the Windows shim engine calls INTO dxgi (compat setters and value
// queries): when the system copy is not resolvable yet, "no compat data" (0) is
// exactly what DXGI itself answers without an entry. Everything else fails
// cleanly with an HRESULT the caller already has to handle.
constexpr bool zero_default(unsigned i) noexcept {return i<=2||i==18||i==19;}
// A real DXGI entry, reached from ordinary game code. It may wait for the
// worker and, as a last resort, resolve on this thread.
template<class F> F factory_export(unsigned index) noexcept {
    if(!adopt_mapped()){
        (void)await([]{return resolved.load(std::memory_order_acquire);},3000);
        if(!adopt_mapped()&&!resolve_loading())return nullptr;
    }
    replay_compat_string();return reinterpret_cast<F>(exports[index]);
}
HRESULT unavailable(void** out) noexcept {
    if(out)*out=nullptr;
    return static_cast<HRESULT>(kNotCurrentlyAvailable);
}
}

extern "C" std::uint64_t LabFallbackZero(){++fallbacks;return 0;}
extern "C" std::uint64_t LabFallbackUnavailable(){++fallbacks;return kNotCurrentlyAvailable;}
// Reached by the shim engine during the game's import resolution, so it must
// NEVER load anything: it adopts an already-mapped system copy or answers with
// the safe default. Calling into the loader here deadlocked Alan Wake 2, three
// threads parked on the loader's keyed events.
extern "C" void* __cdecl LabDXGIResolve(unsigned index){
    if(index>=exports.size())return reinterpret_cast<void*>(&LabFallbackUnavailable);
    if(!adopt_mapped())return reinterpret_cast<void*>(zero_default(index)?&LabFallbackZero:&LabFallbackUnavailable);
    replay_compat_string();
    return reinterpret_cast<void*>(exports[index]);
}
// The shim engine's own setter, observed being called during import resolution
// on Alan Wake 2. Loader-safe path only; the arguments are kept and replayed
// once the worker has the system copy.
extern "C" void WINAPI LabSetAppCompatStringPointer(std::uint64_t a,std::uint64_t b,std::uint64_t c,std::uint64_t d){
    if(adopt_mapped()){
        replay_compat_string();
        if(auto fn=reinterpret_cast<void(WINAPI*)(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t)>(exports[18]))fn(a,b,c,d);
        return;
    }
    compat_args[0]=a;compat_args[1]=b;compat_args[2]=c;compat_args[3]=d;
    compat_pending.store(true,std::memory_order_release);++fallbacks;
}
extern "C" HRESULT WINAPI LabCreateDXGIFactory(const GUID& iid,void** out){
    auto fn=factory_export<HRESULT(WINAPI*)(const GUID&,void**)>(3);if(!fn)return unavailable(out);
    const auto result=fn(iid,out);
    if(SUCCEEDED(result)&&out&&*out)offer_factory(*out);return result;
}
extern "C" HRESULT WINAPI LabCreateDXGIFactory1(const GUID& iid,void** out){
    auto fn=factory_export<HRESULT(WINAPI*)(const GUID&,void**)>(4);if(!fn)return unavailable(out);
    const auto result=fn(iid,out);
    if(SUCCEEDED(result)&&out&&*out)offer_factory(*out);return result;
}
extern "C" HRESULT WINAPI LabCreateDXGIFactory2(UINT flags,const GUID& iid,void** out){
    auto fn=factory_export<HRESULT(WINAPI*)(UINT,const GUID&,void**)>(5);if(!fn)return unavailable(out);
    const auto result=fn(flags,iid,out);
    if(SUCCEEDED(result)&&out&&*out)offer_factory(*out);return result;
}

// How the host tells it was loaded by our own proxy rather than injected. An
// injected copy finds no such export on the process's dxgi.dll and brings itself
// up; a proxy-loaded copy waits to be handed the factory instead.
extern "C" void __cdecl LabDXGIProxyMarker(){}

// Read by the host for its status block. Plain out-parameter, no allocation, so
// it stays callable from anywhere.
struct LabProxyStatus {
    unsigned size,attempts,failures,fallback_calls,last_stage,host_stage;
    unsigned long last_error,host_last_error;
    unsigned resolved,host_loaded,compat_string_pending,compat_string_replayed;
    unsigned mode,insert_presses,insert_stage;
    unsigned watched_key,hotkey_source; // the on-Insert worker's binding (lab::hotkey::pack) and its source
};
extern "C" void __cdecl LabDXGIProxyStatus(LabProxyStatus* out){
    if(!out||out->size!=sizeof(LabProxyStatus))return;
    out->attempts=attempts.load();out->failures=failures.load();out->fallback_calls=fallbacks.load();
    out->last_stage=stage.load();out->host_stage=host_stage.load();
    out->last_error=error.load();out->host_last_error=host_error.load();
    out->resolved=resolved.load()?1u:0u;out->host_loaded=host_loaded.load()?1u:0u;
    out->compat_string_pending=compat_pending.load()?1u:0u;out->compat_string_replayed=compat_replayed.load()?1u:0u;
    out->mode=mode.load();out->insert_presses=insert_presses.load();out->insert_stage=insert_stage.load();
    out->watched_key=watched_key.load();out->hotkey_source=hotkey_source.load();
}

// Creating a thread is the one thing allowed here: the loader lets a thread be
// created under its lock and simply does not run its body until the lock is
// released. Waiting for it, loading a library, hooking or touching COM here are
// all forbidden, and none of them happen. Everything that needs the loader has
// moved onto that thread.
BOOL WINAPI DllMain(HMODULE module,DWORD reason,LPVOID){
    if(reason!=DLL_PROCESS_ATTACH)return TRUE;
    DisableThreadLibraryCalls(module);
    if(const auto thread=CreateThread(nullptr,0,&worker,nullptr,0,nullptr))CloseHandle(thread);
    return TRUE; // a failed thread creation costs NR; the forwarder still works
}
