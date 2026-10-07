// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The game page's state mapping and the code table, as pure functions: every
// backend state the manager can produce maps to a label, a colour meaning and
// the buttons that state offers; every code renders; nothing falls back.
#include "lab_game_presentation.hpp"
#include <iostream>
#include <set>
using lab::json;
using namespace lab::games;
namespace {
unsigned checks=0;
void need(bool ok,const std::string& what){++checks;if(!ok)throw std::runtime_error(what);}
bool installed_axis(const std::string& in){return in=="installed"||in=="update-available"||in=="needs-update"||in=="legacy"||in=="external"||in=="research-managed"||in=="incomplete";}
// A Status shaped the way inspect() shapes it for this pair.
Status shaped(const std::string& state,const std::string& install,bool running){
    Status s;s.state=state;s.install_state=install;s.running=running;s.installed=installed_axis(install)||(state=="changed");
    s.can_uninstall=s.installed&&!running&&state!="blocked";
    s.can_install=state=="available"&&!running;s.can_make_package=state=="needs-package"&&!running;s.can_refresh_package=state=="package-stale";
    s.update_available=install=="update-available"||install=="needs-update"||install=="legacy";s.can_update=s.update_available&&!running;
    s.can_repin=state=="changed"&&!running;
    if(state=="blocked")s.reasons.push_back({install=="modified"?"installed-files-modified":install=="other-copy"?"installed-by-other-copy":install=="not-installed"?"loader-conflict":"internal-error"});
    return s;}
const Action* find(const Presentation& p,const std::string& name){for(const auto& a:p.actions)if(a.name==name)return &a;return nullptr;}
const Action* primary(const Presentation& p){for(const auto& a:p.actions)if(a.primary)return &a;return nullptr;}
struct Expect {const char* state;const char* install;const char* label;Tone tone;const char* primary;};
}
int main(){try{
    // ---- the table: unique codes, sane placeholders, everything renders
    std::set<std::string> codes;
    for(const auto& e:code_table()){const std::string code(e.code);
        need(codes.insert(code).second,"duplicate code "+code);need(!e.text.empty(),"empty text for "+code);
        need(code.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-._")==std::string::npos,"code spelling "+code);
        json params=json::object();
        for(const auto& name:placeholders(e.text)){need(!name.empty()&&name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_")==std::string::npos,"placeholder name in "+code);params[name]="x";}
        const auto text=render(code,params);need(text.find('{')==std::string::npos&&text.find('}')==std::string::npos,"every placeholder filled for "+code);
        need(code_text(code)!=nullptr,"lookup "+code);}
    need(codes.size()>=150,"the table covers the manager");
    need(render("no-such-code")=="[no-such-code]","a missing code is visible");
    need(render("update-available",json{{"parts",json::array({"host","bridge"})}}).find("宿主、桥")!=std::string::npos,"parts render through part.*");
    {const auto t=render("package-missing",json{{"route","ngx-rr"},{"store","gdk"}});
     need(t.find("引擎直连 · 光线重建")!=std::string::npos&&t.find("Xbox app")!=std::string::npos,"route and store render through their families");}
    {const auto t=render("operation-failed",json{{"operation","update"},{"stage","refresh-check"},{"message","boom"}});
     need(t.find("更新")!=std::string::npos&&t.find("重新预检")!=std::string::npos&&t.find("boom")!=std::string::npos,"a stage renders under its operation");}
    need(render("preflight-incomplete",json{{"count",2},{"check",json::array({"denuvo","anticheat"})}}).find("Denuvo 反篡改、反作弊标记")!=std::string::npos,"check names render");
    {const auto t=render("package-ready");
     need(std::count(t.begin(),t.end(),'?')==2&&t.find('{')==std::string::npos&&t.find('}')==std::string::npos,
          "missing parameters show as ?, independent of UI wording");}
    for(const auto* o:{"pass","fail","unknown"})need(code_text(std::string("outcome.")+o)!=nullptr,"three outcomes named");
    need(outcome_name(Outcome::unknown)==std::string("unknown")&&check_json({"denuvo",Outcome::unknown,nullptr,"exe-unreadable"})["ok"].is_null(),"cannot-check is ok:null, never true");
    need(check_json({"denuvo",Outcome::pass,6,{}})["ok"]==true&&check_json({"denuvo",Outcome::fail,nullptr,"denuvo-sections"})["ok"]==false,"pass and fail are booleans");
    // ---- operation stages: one table entry per stage, no duplicates
    for(const auto* op:{"install","uninstall","update","repin"}){const auto& stages=operation_stages(op);need(!stages.empty(),"stages for each operation");
        std::set<std::string> seen;for(const auto& st:stages){need(seen.insert(st).second,"stage listed once");need(code_text(std::string("stage.")+op+"."+st)!=nullptr,"stage text "+std::string(op)+"."+st);}
        need(code_text(std::string("operation.")+op)!=nullptr,"operation name");}
    need(operation_stages("install").front()=="check"&&operation_stages("install").back()=="cleanup-staging","install stages in transaction order");
    need(operation_stages("nonsense").empty(),"unknown operation has no stages");
    // ---- every backend state maps without the fallback
    need(!backend_states().empty(),"backend states listed");
    std::set<std::string> states;
    for(const auto& b:backend_states()){states.insert(b.state);need(known_backend_state(b.state,b.install),"listed pair known");
        for(const bool running:{false,true}){const auto s=shaped(b.state,b.install,running);const auto p=present(s);
            const std::string where=std::string(b.state)+"/"+b.install+(running?" running":"");
            need(code_text(p.label)!=nullptr,"label in table: "+where);
            need(p.label!="label.blocked"||std::string(b.state)=="blocked","only blocked uses the blocked label: "+where);
            need(p.running==running,"running mark: "+where);
            unsigned primaries=0;for(const auto& a:p.actions){
                need(code_text(a.label)!=nullptr,"action label in table: "+where+" "+a.name);
                need(a.enabled==a.why_not.code.empty(),"a disabled action says why: "+where+" "+a.name);
                if(!a.enabled)need(code_text(a.why_not.code)!=nullptr,"why-not in table: "+where+" "+a.why_not.code);
                if(running&&a.writes)need(!a.enabled&&a.why_not.code=="game-running","writes disabled while running: "+where+" "+a.name);
                if(a.primary)++primaries;}
            need(primaries<=1,"at most one primary: "+where);
            if(primaries)need(p.actions.front().primary,"primary first: "+where);
            need(find(p,"open-folder")!=nullptr&&find(p,"open-folder")->enabled,"open folder always offered: "+where);
            const auto busy=present(s,true);for(const auto& a:busy.actions)need(a.name=="open-folder"?a.enabled:(!a.enabled&&a.why_not.code=="busy"),"busy disables everything but the folder: "+where);}}
    need(!states.contains("unsupported-route"),"the unreachable state is gone");
    need(!known_backend_state("unsupported-route","not-installed")&&!known_backend_state("installed","not-installed"),"unknown pairs are unknown");
    need(present(shaped("nonsense","unknown",false)).label=="label.blocked","an unlisted state falls back visibly");
    // ---- the state mapping, row by row
    const Expect rows[]{
        {"needs-package","not-installed","label.available",Tone::accent,"make-package"},
        {"package-stale","not-installed","label.available",Tone::accent,"refresh-package"},
        {"available","not-installed","label.available",Tone::accent,"install"},
        {"installed","installed","label.installed",Tone::success,nullptr},
        {"installed","update-available","label.update-available",Tone::accent,"update"},
        {"installed","needs-update","label.needs-update",Tone::warning,"update"},
        {"installed","legacy","label.legacy",Tone::warning,"update"},
        {"existing","external","label.external",Tone::success,nullptr},
        {"existing","research-managed","label.research-managed",Tone::info,nullptr},
        {"changed","game-changed","label.game-changed",Tone::warning,"repin"},
        {"incomplete","incomplete","label.incomplete",Tone::error,"uninstall"},
        {"blocked","modified","label.modified",Tone::warning,nullptr},
        {"blocked","other-copy","label.other-copy",Tone::info,nullptr},
        {"blocked","not-installed","label.loader-conflict",Tone::warning,nullptr},
        {"blocked","unknown","label.blocked",Tone::error,nullptr},
        {"denuvo-blocked","not-installed","label.denuvo-blocked",Tone::neutral,nullptr},
        {"anticheat-blocked","not-installed","label.anticheat-blocked",Tone::neutral,nullptr},
        {"no-dlss","not-installed","label.no-dlss",Tone::neutral,nullptr},
        {"unsupported-store","not-installed","label.unsupported-store",Tone::neutral,nullptr},
        {"loader-conflict","not-installed","label.loader-conflict",Tone::warning,nullptr},
    };
    need(std::size(rows)==backend_states().size(),"one expectation per backend state");
    for(const auto& r:rows){const auto p=present(shaped(r.state,r.install,false));const std::string where=std::string(r.state)+"/"+r.install;
        need(p.label==r.label,"label "+where+" got "+p.label);need(p.tone==r.tone,"tone "+where+" got "+tone_name(p.tone));
        const auto* first=primary(p);
        if(r.primary)need(first&&first->name==r.primary&&first->enabled,"primary "+where);else need(first==nullptr,"no primary "+where);}
    // ---- details the rows above do not show
    {const auto p=present(shaped("installed","update-available",false));need(p.dot,"有更新 carries the dot");need(find(p,"uninstall")&&find(p,"uninstall")->enabled,"uninstall offered with an update");}
    {const auto p=present(shaped("installed","installed",false));need(!find(p,"update")&&find(p,"uninstall"),"a current install offers no update");}
    {const auto p=present(shaped("existing","research-managed",false));need(!find(p,"uninstall")&&!find(p,"update"),"a research install is read-only here");}
    {const auto p=present(shaped("incomplete","incomplete",false));need(primary(p)->label=="action.cleanup","incomplete cleans up");}
    {const auto p=present(shaped("denuvo-blocked","not-installed",false));for(const auto& a:p.actions)need(!a.writes,"an unsupported game offers nothing that writes");}
    // A refusal decided by the backend: shown disabled, with its reason.
    {auto s=shaped("installed","update-available",false);s.can_update=false;s.refusals["update"]={"update-needs-denuvo-flag"};
     const auto p=present(s);const auto* u=find(p,"update");need(u&&!u->enabled&&u->why_not.code=="update-needs-denuvo-flag","a gated update says why");}
    {auto s=shaped("changed","game-changed",false);s.can_repin=false;s.refusals["repin"]={"repin-needs-denuvo-flag"};
     const auto p=present(s);const auto* r=find(p,"repin");need(r&&!r->enabled&&r->why_not.code=="repin-needs-denuvo-flag","a gated repin is shown disabled with its reason");}
    // A refusal by design (research track, compiled review row): not offered at all.
    for(const auto* code:{"repin-research-track","repin-compiled-row","repin-no-game-root"}){auto s=shaped("changed","game-changed",false);s.can_repin=false;s.refusals["repin"]={code};
        const auto p=present(s);need(!find(p,"repin"),std::string("a structural refusal hides repin: ")+code);}
    {auto s=shaped("changed","game-changed",false);s.installed=false;s.can_uninstall=false;const auto p=present(s);
     need(!find(p,"uninstall")&&find(p,"forget"),"a changed game without Lab files offers no uninstall");}
    {auto s=shaped("available","not-installed",false);s.can_uninstall=false;const auto p=present(s);need(find(p,"forget")&&find(p,"forget")->enabled,"forget a game with no Lab files");}
    {const auto j=presentation_json(present(shaped("installed","needs-update",true)));
     need(j["label"]=="label.needs-update"&&j["tone"]=="warning"&&j["running"]==true&&j["actions"].is_array()&&j["actions"][0]["why_not"]["code"]=="game-running","presentation JSON");}
    std::cout<<json{{"passed",true},{"checks",checks},{"codes",codes.size()},{"backend_states",backend_states().size()}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
