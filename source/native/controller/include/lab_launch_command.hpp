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
#include <filesystem>
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
// A per-game selector lets Steam start a bootstrap while attachment remains
// bound to the registered real EXE. Only our prefix is consumed: the game tail
// is forwarded verbatim, including all its own quoting and option names.
struct SteamCommand {std::wstring command,game_id,error;bool explicit_target=false;};
inline bool valid_game_id(const std::wstring& id){
    if(id.size()!=38||id.front()!=L'{'||id.back()!=L'}')return false;
    for(std::size_t i=1;i<37;++i){const auto c=id[i];
        if(i==9||i==14||i==19||i==24){if(c!=L'-')return false;}
        else if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')||(c>=L'A'&&c<=L'F')))return false;
    }return true;
}
inline SteamCommand steam_command(const std::wstring& rest){
    SteamCommand r;r.command=rest;
    if(program_of(rest)!=L"--game-id")return r;
    r.explicit_target=true;const auto tail=rest_of_command_line(rest.c_str());
    r.game_id=program_of(tail);const auto after_id=rest_of_command_line(tail.c_str());
    if(program_of(after_id)!=L"--"){r.command.clear();r.error=L"Missing -- after the game id";return r;}
    r.command=rest_of_command_line(after_id.c_str());
    if(!valid_game_id(r.game_id))r.error=L"Invalid game id";
    if(r.command.empty())r.error=L"Missing Steam game command";
    return r;
}
inline bool target_matches(const SteamCommand& r,const std::wstring& id,const std::wstring& exe){
    if(!r.error.empty())return false;
    if(r.explicit_target)return valid_game_id(r.game_id)&&_wcsicmp(r.game_id.c_str(),id.c_str())==0;
    const auto command_exe=std::filesystem::path(program_of(r.command)).lexically_normal().wstring();
    return _wcsicmp(command_exe.c_str(),std::filesystem::path(exe).lexically_normal().c_str())==0;
}
// Set by the launcher before it injects on the player's Insert; the injected
// loader opens the panel once attached if it exists. Session-local, per PID.
inline std::wstring open_panel_event_name(DWORD pid){
    wchar_t name[80];std::swprintf(name,std::size(name),L"Local\\Overglaze-OpenPanelOnAttach-%lu",static_cast<unsigned long>(pid));
    return name;
}
}
