// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Cursor-clip ownership of the panel input block. Runs in the test process:
// every user32 call below goes through the same detours the game would hit.
// The real clip is changed for a few milliseconds and always released.
#include "lab_input_block.hpp"
#include <cstdio>
#include <stdexcept>
using namespace lab;
namespace {
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
bool same(const RECT& a,const RECT& b){return a.left==b.left&&a.top==b.top&&a.right==b.right&&a.bottom==b.bottom;}
bool sink(HWND,UINT,WPARAM,LPARAM){return false;}
struct Cleanup{HWND w;~Cleanup(){input_block::activate(w,false);ClipCursor(nullptr);if(w)DestroyWindow(w);}};
}
int main(){try{
    need(input_block::install(&sink),"user32 detours install");
    // A hidden window that never owns the foreground: deactivation must release
    // the clip instead of trapping the cursor for another application.
    HWND w=CreateWindowExW(0,L"STATIC",L"lab-input-block-test",WS_POPUP,0,0,8,8,nullptr,nullptr,nullptr,nullptr);
    need(w!=nullptr,"test window");Cleanup cleanup{w};
    const RECT lock{100,100,101,101},lock2{200,200,201,201};RECT r{};
    // Inactive: pass-through, the clip is applied for real.
    need(ClipCursor(&lock)!=FALSE,"inactive ClipCursor passes through");
    need(input_block::real_clip_rect(&r)!=FALSE&&same(r,lock),"inactive clip applied for real");
    // Active: the real clip is released; the game's requests are remembered, reported back, never applied.
    input_block::activate(w,true);need(input_block::active(),"active");
    need(input_block::real_clip_rect(&r)!=FALSE&&!same(r,lock),"activation releases the real clip");
    need(GetClipCursor(&r)!=FALSE&&same(r,lock),"game still reads the clip it had at activation");
    need(ClipCursor(&lock2)!=FALSE,"blocked ClipCursor reports success");
    need(input_block::real_clip_rect(&r)!=FALSE&&!same(r,lock2),"blocked ClipCursor does not clip for real");
    need(GetClipCursor(&r)!=FALSE&&same(r,lock2),"game reads its latest requested clip");
    need(SetCursorPos(0,0)!=FALSE,"blocked SetCursorPos reports success");
    POINT p{};need(GetCursorPos(&p)!=FALSE,"frozen cursor readable");
    input_block::activate(w,true); // re-activation for the same window keeps state
    need(GetClipCursor(&r)!=FALSE&&same(r,lock2),"re-activation keeps the remembered clip");
    // Deactivate while the window is not foreground: release, do not re-apply.
    input_block::activate(w,false);need(!input_block::active(),"inactive again");
    need(input_block::real_clip_rect(&r)!=FALSE&&!same(r,lock2)&&!same(r,lock),"deactivation without foreground releases the clip");
    need(ClipCursor(&lock)!=FALSE&&input_block::real_clip_rect(&r)!=FALSE&&same(r,lock),"pass-through restored after deactivation");
    need(ClipCursor(nullptr)!=FALSE,"cleanup");
    std::printf("PASS input block owns the cursor clip while the panel is shown\n");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
