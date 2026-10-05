// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The game page's state mapping as a pure function: backend status -> what the game page shows
// (label, colour meaning, which buttons, and why a shown button is disabled).
// Reads only the Status; no file, process or clock. Out-of-game only.
#include "lab_game_manager.hpp"
#include <vector>

namespace lab::games {
// Colour meaning, independent of any visual direction:
// success = working, accent = something to do, warning = the user must act,
// error = Lab itself failed, neutral = not supported by design (never red),
// info = neutral information.
enum class Tone {success,accent,warning,error,neutral,info};
const char* tone_name(Tone) noexcept;
struct Action {
    std::string name;   // install | update | uninstall | make-package | refresh-package | repin | forget | open-folder
    std::string label;  // action.<x> in the code table
    bool primary=false,enabled=false,writes=false;
    Reason why_not;     // code set whenever enabled is false
};
struct Presentation {
    std::string label;  // label.<x> in the code table
    Tone tone=Tone::neutral;
    bool dot=false;     // "有更新": accent plus a dot
    bool running=false; // the "游戏运行中" mark; every write action is then disabled
    std::vector<Action> actions; // only the actions this state offers, primary first
};
// Every write action is disabled while the game runs (game-running) and while
// another operation is running when busy is true (busy).
Presentation present(const Status&,bool busy=false);
json presentation_json(const Presentation&);
// Every (state, install_state) pair inspect() can produce. The state-mapping
// test walks all of them; inspect()'s own test checks it never leaves the list.
struct BackendState {const char* state;const char* install;};
const std::vector<BackendState>& backend_states();
bool known_backend_state(const std::string& state,const std::string& install);
}
