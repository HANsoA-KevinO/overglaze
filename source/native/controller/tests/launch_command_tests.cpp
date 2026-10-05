// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// overglaze_launch.exe's command-line handling: the game must receive exactly the
// command Steam put after our program name, and we must find the game's EXE in
// it the way Windows does. No process is started here.
#include "lab_launch_command.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
}

int wmain(){
    try{
        using lab::launch::rest_of_command_line;using lab::launch::program_of;
        // Steam's shape: our quoted path, then the game's quoted path and arguments.
        const std::wstring steam=LR"("C:\Lab Root\app\overglaze_launch.exe" "D:\Games\steamapps\common\RESIDENT EVIL requiem BIOHAZARD requiem\re9.exe" -dx12 "a b")";
        const auto rest=rest_of_command_line(steam.c_str());
        need(rest==LR"("D:\Games\steamapps\common\RESIDENT EVIL requiem BIOHAZARD requiem\re9.exe" -dx12 "a b")","the game's command is passed on verbatim");
        need(program_of(rest)==LR"(D:\Games\steamapps\common\RESIDENT EVIL requiem BIOHAZARD requiem\re9.exe)","the game's EXE, quotes removed");
        // Our own name unquoted, extra spacing, tabs.
        need(rest_of_command_line(L"overglaze_launch.exe \t  C:\\g\\game.exe -x")==L"C:\\g\\game.exe -x","unquoted program name and mixed whitespace");
        need(program_of(L"C:\\g\\game.exe -x")==L"C:\\g\\game.exe","unquoted game EXE");
        // Quotes inside the arguments are the game's business, left untouched.
        need(rest_of_command_line(LR"("l.exe" "C:\a b\g.exe" --opt="x y" z)")==LR"("C:\a b\g.exe" --opt="x y" z)","argument quoting preserved");
        // Nothing after us: nothing to launch.
        need(rest_of_command_line(L"\"C:\\l.exe\"").empty()&&rest_of_command_line(L"l.exe   ").empty()&&rest_of_command_line(nullptr).empty(),"no command after the launcher");
        need(program_of(L"").empty(),"empty command, no program");
        // An unterminated quote in our own name consumes the line rather than guessing.
        need(rest_of_command_line(L"\"C:\\l.exe game.exe").empty(),"unterminated quote in our own name");
        // The panel-request event is per PID and session-local.
        need(lab::launch::open_panel_event_name(1234)==L"Local\\Overglaze-OpenPanelOnAttach-1234","event name");
        need(lab::launch::open_panel_event_name(1)!=lab::launch::open_panel_event_name(2),"distinct per process");
        std::cout<<"PASS "<<checks<<" launcher command-line checks\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
