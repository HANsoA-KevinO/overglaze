// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The game manager's codes and the one table that turns them into words. The
// manager reports WHAT happened as a code
// with parameters; only this table says it in Chinese, so the backend composes
// no sentences and a later front end (or a translation) reads the same table.
// Out-of-game only: nothing compiled into a game, the bridge, the proxy or the
// installation checker includes this.
#include "lab_platform.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace lab::games {
// Three-state result of one check. "unknown" means the check
// could not run -- an unreadable EXE, a truncated scan -- and is never shown or
// counted as a pass.
enum class Outcome {pass,fail,unknown};
const char* outcome_name(Outcome) noexcept; // "pass" | "fail" | "unknown"
struct Check {
    std::string name;        // check.<name> in the table
    Outcome outcome=Outcome::unknown;
    json value=nullptr;      // what was measured, when anything was
    std::string reason;      // a code, set whenever the outcome is not pass
};
struct Reason {std::string code;json params=json::object();};
// {"name","ok":true|false|null,"value","reason"}: ok is null for "cannot check".
json check_json(const Check&);
json reason_json(const Reason&); // {"code","params"}

// Families, by prefix:
//   <bare code>          status, refusal and operation reasons
//   label.<x>            state labels          action.<x>     buttons
//   check.<x>            check names           outcome.<x>    pass / fail / unknown
//   stage.<op>.<x>       progress stages       operation.<x>  install / update / ...
//   parts.<x> route.<x> store.<x> load-mode.<strategy> verdict.<x>  parameter values
struct CodeText {std::string_view code,text;};
const std::vector<CodeText>& code_table();
// nullptr when the code is not in the table.
const char* code_text(std::string_view code) noexcept;
// The text with {name} placeholders filled from params. A string value is first
// looked up as "<name with _ as ->.<value>" (route, store, parts, load-mode,
// operation, verdict, check), then used verbatim; "stage" is looked up under its "operation";
// numbers print in decimal; arrays are joined with "、". An unknown code renders
// as "[code]" so a missing entry is visible, never silent.
std::string render(std::string_view code,const json& params=json::object());
std::string render(const Reason&);
std::string render_all(const std::vector<Reason>&); // joined with a space
// The placeholders a table entry names, in order of first use.
std::vector<std::string> placeholders(std::string_view text);
json code_table_json(); // {"schema":"overglaze-game-codes-v1","codes":{code:text}}
}
