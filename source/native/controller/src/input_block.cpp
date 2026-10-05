// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_input_block.hpp"
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
namespace lab::input_block {
namespace {
std::atomic<bool> g_installed{false},g_active{false};
std::atomic<HWND> g_window{nullptr};
std::atomic<MessageSink> g_sink{nullptr};
std::mutex g_install_mutex;
POINT g_frozen{};
// Cursor clip the game believes is in force while the panel owns input. Games
// with mouse-look (2077) re-clip the cursor to a point every frame; the real
// clip is released at activation, every request meanwhile is only remembered,
// and the last one is re-applied at deactivation if the game still owns the
// foreground window.
std::mutex g_clip_mutex;
RECT g_game_clip{};bool g_game_clip_valid=false;
using GetCursorPos_t=BOOL(WINAPI*)(LPPOINT);
using SetCursorPos_t=BOOL(WINAPI*)(int,int);
using ClipCursor_t=BOOL(WINAPI*)(const RECT*);
using GetClipCursor_t=BOOL(WINAPI*)(LPRECT);
using GetAsyncKeyState_t=SHORT(WINAPI*)(int);
using GetKeyState_t=SHORT(WINAPI*)(int);
using GetKeyboardState_t=BOOL(WINAPI*)(PBYTE);
using GetRawInputData_t=UINT(WINAPI*)(HRAWINPUT,UINT,LPVOID,PUINT,UINT);
using GetRawInputBuffer_t=UINT(WINAPI*)(PRAWINPUT,PUINT,UINT);
using PeekMessageW_t=BOOL(WINAPI*)(LPMSG,HWND,UINT,UINT,UINT);
using PeekMessageA_t=BOOL(WINAPI*)(LPMSG,HWND,UINT,UINT,UINT);
using GetMessageW_t=BOOL(WINAPI*)(LPMSG,HWND,UINT,UINT);
using GetMessageA_t=BOOL(WINAPI*)(LPMSG,HWND,UINT,UINT);
GetCursorPos_t o_GetCursorPos=nullptr;SetCursorPos_t o_SetCursorPos=nullptr;
ClipCursor_t o_ClipCursor=nullptr;GetClipCursor_t o_GetClipCursor=nullptr;
GetAsyncKeyState_t o_GetAsyncKeyState=nullptr;GetKeyState_t o_GetKeyState=nullptr;GetKeyboardState_t o_GetKeyboardState=nullptr;
GetRawInputData_t o_GetRawInputData=nullptr;GetRawInputBuffer_t o_GetRawInputBuffer=nullptr;
PeekMessageW_t o_PeekMessageW=nullptr;PeekMessageA_t o_PeekMessageA=nullptr;GetMessageW_t o_GetMessageW=nullptr;GetMessageA_t o_GetMessageA=nullptr;

bool blocking_for(HWND w) noexcept {return g_active.load(std::memory_order_acquire)&&w&&w==g_window.load(std::memory_order_acquire);}
bool input_message(UINT m) noexcept {return (m>=WM_MOUSEFIRST&&m<=WM_MOUSELAST)||(m>=WM_KEYFIRST&&m<=WM_KEYLAST);}
bool full_desktop(const RECT& r) noexcept {
    const int x=GetSystemMetrics(SM_XVIRTUALSCREEN),y=GetSystemMetrics(SM_YVIRTUALSCREEN);
    return r.left<=x&&r.top<=y&&r.right>=x+GetSystemMetrics(SM_CXVIRTUALSCREEN)&&r.bottom>=y+GetSystemMetrics(SM_CYVIRTUALSCREEN);
}
void blank(RAWINPUT* r) noexcept {
    if(r->header.dwType==RIM_TYPEMOUSE){r->data.mouse.lLastX=0;r->data.mouse.lLastY=0;r->data.mouse.usButtonFlags=0;r->data.mouse.usButtonData=0;r->data.mouse.ulRawButtons=0;}
    else if(r->header.dwType==RIM_TYPEKEYBOARD){r->data.keyboard.MakeCode=0;r->data.keyboard.Flags=RI_KEY_BREAK;r->data.keyboard.VKey=0;r->data.keyboard.Message=WM_KEYUP;}
}
// Consume panel input before the game's pump can inspect it. Alt+F4 is left
// alone so the user's close command still works; WM_INPUT stays in the queue
// (its payload is blanked in GetRawInputData) so DefWindowProc cleanup runs.
void filter(LPMSG msg,bool removing) noexcept {
    if(!msg||!removing||!blocking_for(msg->hwnd)||!input_message(msg->message))return;
    if((msg->message==WM_SYSKEYDOWN||msg->message==WM_SYSKEYUP)&&msg->wParam==VK_F4)return;
    if(auto sink=g_sink.load(std::memory_order_acquire))if(sink(msg->hwnd,msg->message,msg->wParam,msg->lParam)){msg->message=WM_NULL;msg->wParam=0;msg->lParam=0;}
}
BOOL WINAPI h_GetCursorPos(LPPOINT p){if(g_active.load(std::memory_order_acquire)&&p){*p=g_frozen;return TRUE;}return o_GetCursorPos(p);}
BOOL WINAPI h_SetCursorPos(int x,int y){if(g_active.load(std::memory_order_acquire))return TRUE;return o_SetCursorPos(x,y);}
BOOL WINAPI h_ClipCursor(const RECT* r){
    if(g_active.load(std::memory_order_acquire)){std::lock_guard lock(g_clip_mutex);if(r){g_game_clip=*r;g_game_clip_valid=true;}else g_game_clip_valid=false;return TRUE;}
    return o_ClipCursor(r);
}
BOOL WINAPI h_GetClipCursor(LPRECT out){
    if(g_active.load(std::memory_order_acquire)&&out){std::lock_guard lock(g_clip_mutex);if(g_game_clip_valid){*out=g_game_clip;return TRUE;}}
    return o_GetClipCursor(out);
}
SHORT WINAPI h_GetAsyncKeyState(int vk){if(g_active.load(std::memory_order_acquire))return 0;return o_GetAsyncKeyState(vk);}
SHORT WINAPI h_GetKeyState(int vk){if(g_active.load(std::memory_order_acquire))return 0;return o_GetKeyState(vk);}
BOOL WINAPI h_GetKeyboardState(PBYTE s){const auto r=o_GetKeyboardState(s);if(r&&s&&g_active.load(std::memory_order_acquire))std::memset(s,0,256);return r;}
UINT WINAPI h_GetRawInputData(HRAWINPUT h,UINT cmd,LPVOID data,PUINT size,UINT header){
    const auto r=o_GetRawInputData(h,cmd,data,size,header);
    if(g_active.load(std::memory_order_acquire)&&data&&cmd==RID_INPUT&&r!=UINT(-1)&&r>=sizeof(RAWINPUTHEADER))blank(static_cast<RAWINPUT*>(data));
    return r;
}
UINT WINAPI h_GetRawInputBuffer(PRAWINPUT data,PUINT size,UINT header){
    const auto r=o_GetRawInputBuffer(data,size,header);
    if(g_active.load(std::memory_order_acquire)&&data&&r&&r!=UINT(-1)){RAWINPUT* p=data;for(UINT i=0;i<r;++i){blank(p);p=reinterpret_cast<RAWINPUT*>((reinterpret_cast<std::uintptr_t>(p)+p->header.dwSize+7u)&~std::uintptr_t(7));}}
    return r;
}
BOOL WINAPI h_PeekMessageW(LPMSG m,HWND w,UINT lo,UINT hi,UINT flags){const auto r=o_PeekMessageW(m,w,lo,hi,flags);if(r)filter(m,(flags&PM_REMOVE)!=0);return r;}
BOOL WINAPI h_PeekMessageA(LPMSG m,HWND w,UINT lo,UINT hi,UINT flags){const auto r=o_PeekMessageA(m,w,lo,hi,flags);if(r)filter(m,(flags&PM_REMOVE)!=0);return r;}
BOOL WINAPI h_GetMessageW(LPMSG m,HWND w,UINT lo,UINT hi){const auto r=o_GetMessageW(m,w,lo,hi);if(r>0)filter(m,true);return r;}
BOOL WINAPI h_GetMessageA(LPMSG m,HWND w,UINT lo,UINT hi){const auto r=o_GetMessageA(m,w,lo,hi);if(r>0)filter(m,true);return r;}
template<class T> bool hook(HMODULE user32,const char* name,T detour,T* original) noexcept {
    auto* target=reinterpret_cast<void*>(GetProcAddress(user32,name));if(!target)return false;
    if(MH_CreateHook(target,reinterpret_cast<void*>(detour),reinterpret_cast<void**>(original))!=MH_OK)return false;
    return MH_EnableHook(target)==MH_OK;
}
}
bool install(MessageSink sink) noexcept {
    std::lock_guard lock(g_install_mutex);g_sink.store(sink,std::memory_order_release);
    if(g_installed.load())return true;
    const auto init=MH_Initialize();if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED)return false;
    HMODULE user32=GetModuleHandleW(L"user32.dll");if(!user32)return false;
    const bool ok=hook(user32,"GetCursorPos",&h_GetCursorPos,&o_GetCursorPos)&&hook(user32,"SetCursorPos",&h_SetCursorPos,&o_SetCursorPos)&&
        hook(user32,"ClipCursor",&h_ClipCursor,&o_ClipCursor)&&hook(user32,"GetClipCursor",&h_GetClipCursor,&o_GetClipCursor)&&
        hook(user32,"GetAsyncKeyState",&h_GetAsyncKeyState,&o_GetAsyncKeyState)&&hook(user32,"GetKeyState",&h_GetKeyState,&o_GetKeyState)&&
        hook(user32,"GetKeyboardState",&h_GetKeyboardState,&o_GetKeyboardState)&&hook(user32,"GetRawInputData",&h_GetRawInputData,&o_GetRawInputData)&&
        hook(user32,"GetRawInputBuffer",&h_GetRawInputBuffer,&o_GetRawInputBuffer)&&hook(user32,"PeekMessageW",&h_PeekMessageW,&o_PeekMessageW)&&
        hook(user32,"PeekMessageA",&h_PeekMessageA,&o_PeekMessageA)&&hook(user32,"GetMessageW",&h_GetMessageW,&o_GetMessageW)&&hook(user32,"GetMessageA",&h_GetMessageA,&o_GetMessageA);
    // A partial set is never activated: blocking stays disabled for the process.
    if(!ok){g_active=false;return false;}
    g_installed.store(true,std::memory_order_release);return true;
}
bool installed() noexcept {return g_installed.load(std::memory_order_acquire);}
void activate(HWND window,bool on) noexcept {
    if(!g_installed.load(std::memory_order_acquire)){g_active=false;return;}
    if(on){
        // Re-activation for the same window keeps the frozen point and the remembered clip.
        if(g_active.load(std::memory_order_acquire)&&g_window.load(std::memory_order_acquire)==window)return;
        POINT p{};if(o_GetCursorPos(&p))g_frozen=p;
        {std::lock_guard lock(g_clip_mutex);RECT r{};g_game_clip_valid=o_GetClipCursor(&r)!=FALSE&&!full_desktop(r);if(g_game_clip_valid)g_game_clip=r;}
        g_window.store(window,std::memory_order_release);g_active.store(true,std::memory_order_release);
        o_ClipCursor(nullptr); // the panel needs the whole screen; the game's clip returns at deactivation
    }else{
        if(!g_active.exchange(false,std::memory_order_acq_rel))return;
        std::lock_guard lock(g_clip_mutex);const HWND w=g_window.load(std::memory_order_acquire);
        // Never re-apply a clip when another application owns the foreground.
        if(g_game_clip_valid&&w&&GetForegroundWindow()==w)o_ClipCursor(&g_game_clip);else o_ClipCursor(nullptr);
    }
}
bool active() noexcept {return g_active.load(std::memory_order_acquire);}
BOOL real_cursor_position(POINT* out) noexcept {if(!out)return FALSE;if(o_GetCursorPos)return o_GetCursorPos(out);return GetCursorPos(out);}
BOOL real_clip_rect(RECT* out) noexcept {if(!out)return FALSE;if(o_GetClipCursor)return o_GetClipCursor(out);return GetClipCursor(out);}
}
