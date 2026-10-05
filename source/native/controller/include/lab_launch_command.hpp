// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The Steam-launch-option launcher's small, testable pieces (overglaze_launch.exe).
// Steam runs `"<launcher>" %command%`, so our own command line is the game's
// command line with our program name in front of it.
#include <windows.h>
#include <shellapi.h>
#include <cwchar>
#include <string>
namespace lab::launch {
// Everything after this program's own name, verbatim. The program name follows
// the Windows rule for argv[0]: a leading quote runs to the next quote, otherwise
// it ends at the first space or tab. Nothing after it is re-quoted or re-split,
// so the game receives exactly the arguments Steam gave.
inline std::wstring rest_of_command_line(const wchar_t* line){
    if(!line)return {};
    const wchar_t* p=line;
    if(*p==L'"'){++p;while(*p&&*p!=L'"')++p;if(*p==L'"')++p;}
    else while(*p&&*p!=L' '&&*p!=L'\t')++p;
    while(*p==L' '||*p==L'\t')++p;
    return p;
}
// The program a command line starts, with the same argv[0] rule; empty if none.
inline std::wstring program_of(const std::wstring& command){
    if(command.empty())return {};
    int count=0;auto** args=CommandLineToArgvW(command.c_str(),&count);
    if(!args)return {};
    std::wstring first=count>0?args[0]:L"";
    LocalFree(args);return first;
}
// Set by the launcher before it injects on the player's Insert; the injected
// loader opens the panel once attached if it exists. Session-local, per PID.
inline std::wstring open_panel_event_name(DWORD pid){
    wchar_t name[80];std::swprintf(name,std::size(name),L"Local\\Overglaze-OpenPanelOnAttach-%lu",static_cast<unsigned long>(pid));
    return name;
}
}
