// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_game_presentation.hpp"
#include <algorithm>

namespace lab::games {
namespace {
const std::vector<BackendState> kStates{
    {"needs-package","not-installed"},{"package-stale","not-installed"},{"available","not-installed"},
    {"installed","installed"},{"installed","update-available"},{"installed","needs-update"},{"installed","legacy"},
    {"existing","external"},{"existing","research-managed"},
    {"changed","game-changed"},{"incomplete","incomplete"},
    {"blocked","modified"},{"blocked","other-copy"},{"blocked","not-installed"},{"blocked","unknown"},
    {"denuvo-blocked","not-installed"},{"anticheat-blocked","not-installed"},{"no-dlss","not-installed"},
    {"unsupported-store","not-installed"},{"loader-conflict","not-installed"},
};
// A refusal that means "this tool never offers it for this package": the
// button is not shown at all; the reason is in the status text.
bool structural(const std::string& code){return code=="repin-research-track"||code=="repin-compiled-row"||code=="repin-no-game-root";}
// Blocked by something in the game directory the user has to sort out.
bool conflict(const std::string& code){return code=="loader-conflict"||code=="foreign-loader"||code=="foreign-bridge"||code=="model-mismatch";}
}
const char* tone_name(Tone t) noexcept{
    switch(t){case Tone::success:return "success";case Tone::accent:return "accent";case Tone::warning:return "warning";
        case Tone::error:return "error";case Tone::neutral:return "neutral";case Tone::info:return "info";}
    return "neutral";}
const std::vector<BackendState>& backend_states(){return kStates;}
bool known_backend_state(const std::string& state,const std::string& install){
    return std::any_of(kStates.begin(),kStates.end(),[&](const BackendState& b){return state==b.state&&install==b.install;});}
Presentation present(const Status& s,bool busy){
    Presentation p;p.running=s.running;
    auto action=[&](const char* name,bool primary,bool possible,bool writes,const char* label=nullptr){
        Action a;a.name=name;a.label=label?label:std::string("action.")+name;a.primary=primary;a.writes=writes;
        // Opening the folder changes nothing, so it stays usable while busy.
        if(busy&&a.name!="open-folder")a.why_not={"busy"};
        else if(writes&&s.running)a.why_not={"game-running"};
        else if(!possible){const auto it=s.refusals.find(name);a.why_not=it!=s.refusals.end()?it->second:Reason{"not-possible-now"};}
        a.enabled=a.why_not.code.empty();p.actions.push_back(std::move(a));};
    auto folder=[&]{action("open-folder",false,true,false);};
    auto forget=[&]{if(!s.installed)action("forget",false,!s.can_uninstall,false);};
    const auto& st=s.state;const auto& in=s.install_state;
    const std::string first=s.reasons.empty()?std::string():s.reasons.front().code;
    if(st=="needs-package"||st=="package-stale"||st=="available"){
        p.label="label.available";p.tone=Tone::accent;
        if(st=="needs-package")action("make-package",true,s.can_make_package,false);
        else if(st=="package-stale")action("refresh-package",true,s.can_refresh_package,false);
        else action("install",true,s.can_install,true);
        folder();forget();}
    else if(st=="installed"){
        // Installed by the pre-rename build: the one action is the migration,
        // which is update() with the package rewritten under the new names.
        if(in=="legacy"){p.label="label.legacy";p.tone=Tone::warning;action("update",true,s.can_update,true,"action.migrate");
            action("uninstall",false,s.can_uninstall,true);folder();return p;}
        if(in=="needs-update"){p.label="label.needs-update";p.tone=Tone::warning;}
        else if(in=="update-available"){p.label="label.update-available";p.tone=Tone::accent;p.dot=true;}
        else{p.label="label.installed";p.tone=Tone::success;}
        if(in=="needs-update"||in=="update-available")action("update",true,s.can_update,true);
        action("uninstall",false,s.can_uninstall,true);folder();}
    else if(st=="existing"){
        // A research install is read-only here: the research tools
        // own it, so this page offers neither update nor uninstall.
        if(in=="research-managed"){p.label="label.research-managed";p.tone=Tone::info;folder();}
        else{p.label="label.external";p.tone=Tone::success;action("uninstall",false,s.can_uninstall,true);folder();}}
    else if(st=="changed"){
        p.label="label.game-changed";p.tone=Tone::warning;
        const auto r=s.refusals.find("repin");
        if(r==s.refusals.end()||!structural(r->second.code))action("repin",true,s.can_repin,true);
        if(s.installed)action("uninstall",false,s.can_uninstall,true);
        folder();forget();}
    else if(st=="incomplete"){p.label="label.incomplete";p.tone=Tone::error;action("uninstall",true,s.can_uninstall,true,"action.cleanup");folder();}
    else if(st=="blocked"){
        if(in=="other-copy"){p.label="label.other-copy";p.tone=Tone::info;}
        else if(in=="modified"){p.label="label.modified";p.tone=Tone::warning;}
        else if(conflict(first)){p.label="label.loader-conflict";p.tone=Tone::warning;}
        else{p.label="label.blocked";p.tone=Tone::error;}
        folder();forget();}
    else if(st=="denuvo-blocked"||st=="anticheat-blocked"||st=="no-dlss"||st=="unsupported-store"){
        p.label="label."+st;p.tone=Tone::neutral;folder();forget();}
    else if(st=="loader-conflict"){p.label="label.loader-conflict";p.tone=Tone::warning;folder();forget();}
    else{p.label="label.blocked";p.tone=Tone::error;folder();} // not a backend state; the test forbids reaching it
    return p;}
json presentation_json(const Presentation& p){json actions=json::array();
    for(const auto& a:p.actions)actions.push_back({{"name",a.name},{"label",a.label},{"primary",a.primary},{"enabled",a.enabled},{"writes",a.writes},
        {"why_not",a.why_not.code.empty()?json(nullptr):reason_json(a.why_not)}});
    return {{"label",p.label},{"tone",tone_name(p.tone)},{"dot",p.dot},{"running",p.running},{"actions",actions}};}
}
