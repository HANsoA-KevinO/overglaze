// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// overglaze_controller.dll — the late-loading controller, injected into a game
// that is already running.
//
// The whole file is shaped by one rule: DllMain runs under the loader lock, so
// it may not load a library, create a thread that does real work, hook anything,
// or touch COM. It does exactly two things — pin this module so the worker can
// never outlive it, and hand off to a thread. Every decision, including whether
// this process is ours to touch at all, happens on that thread.
//
// There is no fixture branch and no environment switch. start_late_attach()
// admits only a process with a validated installation beside this DLL, so an
// injected copy without one does nothing at all.
#include "lab_standalone.hpp"
#include "lab_launch_command.hpp"
#include <windows.h>

namespace {
// Were we loaded by our own root proxy, or injected? The proxy exports a marker
// no system dxgi.dll has. If it is there, the proxy owns the bring-up and will
// hand us the game's factory at renderer time, so this worker must not start a
// second, blinder one of its own. An injected copy finds no marker.
bool proxy_owns_bringup() noexcept {
    const auto dxgi=GetModuleHandleW(L"dxgi.dll");
    return dxgi&&GetProcAddress(dxgi,"LabDXGIProxyMarker")!=nullptr;
}
// Did overglaze_launch.exe inject us on the player's Insert? It leaves a named
// event for this PID; that press was for the panel, so the panel opens itself.
bool launcher_asked_for_panel() noexcept {
    const auto event=OpenEventW(SYNCHRONIZE,FALSE,lab::launch::open_panel_event_name(GetCurrentProcessId()).c_str());
    if(!event)return false;
    CloseHandle(event);return true;
}
DWORD WINAPI worker(LPVOID){
    // Outside the loader lock from here on.
    if(proxy_owns_bringup())return 0;
    lab::standalone::start_late_attach(launcher_asked_for_panel()?lab::standalone::late_open_panel:0u);
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){
    if(reason!=DLL_PROCESS_ATTACH)return TRUE;
    DisableThreadLibraryCalls(module);
    // Pin before the worker exists: the host it starts keeps running for the life
    // of the process and holds pointers into this image.
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&worker),&pinned))return TRUE; // stay inert rather than run unpinned
    if(const auto thread=CreateThread(nullptr,0,&worker,nullptr,0,nullptr))CloseHandle(thread);
    return TRUE;
}
