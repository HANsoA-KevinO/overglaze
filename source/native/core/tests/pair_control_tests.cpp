// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include <iostream>
namespace {
void need(bool x,const char* message){if(!x)throw std::runtime_error(message);}
lab::json req(lab::Controller& c,const char* method,lab::json params=lab::json::object()){
    const auto s=c.status();return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id","pair-test"},
        {"session_id",s["session_id"]},{"expected_revision",s["revision"]},{"method",method},{"params",params}};
}
void on(lab::Controller& c){need(c.handle(req(c,"SetNrMode",{{"mode","on"}}),1)["ok"]==true,"ON request");
    auto w=c.take_nr_mode_request(2);c.acknowledge_nr_mode(w->at("revision"),1,true,true,true);}
}
int main(){try{
    lab::Controller bare;need(bare.handle(req(bare,"CapturePair"),0)["error"]["code"]=="unsupported","No fabricated backend");
    lab::Controller c;c.enable_nr_frame_control("synthetic-input-real-nr",true,true);
    need(c.handle(req(c,"CapturePair"),0)["error"]["code"]=="nr_off","OFF cannot capture NR stages");on(c);
    need(c.handle(req(c,"CapturePair",{{"path","C:\\arbitrary"}}),3)["error"]["code"]=="bad_config","No arbitrary path");
    const auto request=req(c,"CapturePair");const auto accepted=c.handle(request,4);need(accepted["ok"]==true&&c.handle(request,5)==accepted,"Idempotency");
    c.publish_pair({{"state","idle"},{"revision",0u}});need(c.status()["frame_pair"]["state"]=="requested","No stale status over request");
    need(c.handle(req(c,"CapturePair"),6)["error"]["code"]=="pending","One active capture");
    need(c.handle(req(c,"CancelCapturePair"),7)["ok"]==true,"Cancel queued capture");
    need(!c.take_pair_request(8)&&c.status()["frame_pair"]["state"]=="cancelled","Queued cancel performs no GPU work");
    need(c.handle(req(c,"CapturePair"),9)["ok"]==true,"Rearm");const auto action=c.take_pair_request(10);
    need(action&&!action->at("cancel").get<bool>()&&!c.take_pair_request(11),"Consume once");
    c.publish_pair({{"state","pending-gpu"},{"revision",action->at("revision")},{"files",0}});
    need(c.handle(req(c,"CancelCapturePair"),12)["ok"]==true,"Cancel GPU capture");need(c.take_pair_request(13)->at("cancel")==true,"Cancel delivered");
    c.publish_pair({{"state","cancelled"},{"revision",action->at("revision")},{"files",0}});
    need(c.handle(req(c,"CapturePair"),14)["ok"]==true,"Rearm after GPU cancellation");
    c.frame_boundary(9,11015);need(!c.take_pair_request(11016)&&c.status()["frame_pair"]["state"]=="cancelled","Disconnect cancels unconsumed pair");
    c.publish_host({{"state","failed"},{"error","test failure"}});need(c.status()["capabilities"]["capture_pair"]==false,"Fail closed");
    need(c.handle(req(c,"CapturePair"),11017)["error"]["code"]=="backend_failed","No new failed-host action");
    lab::Controller rebuild;rebuild.enable_nr_frame_control("synthetic-input-real-nr",true,true);on(rebuild);
    need(rebuild.handle(req(rebuild,"CapturePair"),2)["ok"]==true,"Queue capture before resize");
    need(rebuild.handle(req(rebuild,"SetNrSettings",{{"tone",.5f},{"structure",.25f}}),3)["ok"]==true,"Queue settings before resize");
    need(rebuild.take_nr_mode_request(4).has_value(),"Dispatched setting exists");
    const auto stale=req(rebuild,"SetNrMode",{{"mode","on"}});
    rebuild.interrupt_nr_for_rebuild(1,70);
    const auto revision=rebuild.status()["revision"];
    rebuild.interrupt_nr_for_rebuild(1,70);need(rebuild.status()["revision"]==revision,"Interruption deduplicated");
    need(rebuild.status()["nr_frame_control"]["interrupted_requests"].size()==1&&!rebuild.take_nr_mode_request(5),"Old in-flight settings cancelled, not replayed");
    need(rebuild.status()["frame_pair"]["state"]=="cancelled"&&!rebuild.take_pair_request(5),"Unsent capture cancelled");
    need(rebuild.handle(stale,6)["error"]["code"]=="stale_revision","Pre-rebuild request rejected");
    need(rebuild.handle(req(rebuild,"SetNrMode",{{"mode","on"}}),7)["error"]["code"]=="rebuilding","No ON during rebuild");
    lab::json runtime={{"profile","cyberpunk-rr-experimental-v1"},{"state","draining"},{"rebuild_serial",1u},{"rebuild_frame",70u},{"bypass_frame",69u}};
    rebuild.publish_nr_runtime(runtime);
    need(!rebuild.status()["capabilities"]["nr_frame_control"].get<bool>()&&rebuild.status()["nr_frame_control"]["observed_mode"]=="unknown","No early OFF or ready claim");
    runtime["bypass_frame"]=70u;rebuild.publish_nr_runtime(runtime);
    need(rebuild.status()["nr_frame_control"]["observed_mode"]=="off","Matched bypass return is explicit OFF");
    rebuild.frame_boundary(71,11010);
    need(rebuild.status()["nr_frame_control"]["state"]=="rebuilding"&&!rebuild.take_nr_mode_request(11011),"Lease expiry cannot resurrect work during rebuild");
    runtime["state"]="ready";rebuild.publish_nr_runtime(runtime);
    need(rebuild.status()["capabilities"]["nr_frame_control"].get<bool>()&&rebuild.status()["nr_frame_control"]["state"]=="idle","Ready reopens control, never auto ON");
    need(rebuild.handle(req(rebuild,"SetNrSettings",{{"tone",.5f},{"structure",.25f}}),11012)["error"]["code"]=="nr_off","New Feature requires actual ON receipt before settings");
    need(rebuild.handle(req(rebuild,"SetNrMode",{{"mode","on"}}),11013)["ok"]==true,"Explicit ON resumes");
    rebuild.publish_host({{"state","failed"},{"error","device-fault"}});rebuild.interrupt_nr_for_rebuild(2,80);rebuild.publish_nr_runtime(runtime);
    need(!rebuild.status()["capabilities"]["nr_frame_control"].get<bool>(),"Rebuild never clears terminal device failure");
    // The optional chain-capture scope ("chain" = pair + reconstructor inputs + following Presents; "presents" = Presents alone).
    {lab::Controller s;s.enable_nr_frame_control("synthetic-input-real-nr",true,true);
     need(s.status()["capabilities"]["capture_chain"]==false,"No chain capability without a backend");
     need(s.handle(req(s,"CapturePair",{{"scope","chain"}}),1)["error"]["code"]=="unsupported","Scoped request without a chain backend is unsupported");
     s.enable_chain_capture();need(s.status()["capabilities"]["capture_chain"]==true,"Chain capability reported once attached");
     bool twice=false;try{s.enable_chain_capture();}catch(const std::logic_error&){twice=true;}need(twice,"Chain backend attaches once");
     need(s.handle(req(s,"CapturePair",{{"scope","frames"}}),2)["error"]["code"]=="bad_config","scope accepts only chain or presents");
     need(s.handle(req(s,"CapturePair",{{"scope","chain"},{"path","C:\\arbitrary"}}),3)["error"]["code"]=="bad_config","Unknown key beside scope refused");
     need(s.handle(req(s,"CapturePair",{{"scope","chain"},{"fg_declared","maybe"}}),4)["error"]["code"]=="bad_config","fg_declared accepts off, on or unknown");
     need(s.handle(req(s,"CapturePair",{{"fg_declared","off"}}),5)["error"]["code"]=="bad_config","fg_declared alone is not a scope");
     need(s.handle(req(s,"CapturePair"),6)["error"]["code"]=="nr_off","The unscoped pair keeps requiring NR evaluation");
     need(s.handle(req(s,"CancelCapturePair",{{"scope","chain"}}),7)["error"]["code"]=="bad_config","Cancel takes no scope");
     const auto scoped=req(s,"CapturePair",{{"scope","chain"},{"fg_declared","off"}});const auto accepted=s.handle(scoped,8);
     need(accepted["ok"]==true&&s.status()["frame_pair"]["scope"]=="chain"&&s.status()["frame_pair"]["fg_declared"]=="off","A scoped chain may be requested with NR OFF");
     need(s.handle(scoped,9)==accepted,"Same id and payload is idempotent");
     auto changed=scoped;changed["params"]["scope"]="presents";need(s.handle(changed,10)["error"]["code"]=="id_reused","Same id with another scope refused");
     need(s.handle(req(s,"CapturePair",{{"scope","presents"}}),11)["error"]["code"]=="pending","One capture of any scope at a time");
     lab::json delivered;need(s.dispatch_pair_request(12,[&](const lab::json& a){delivered=a;return true;}),"Scoped request delivered while NR is OFF");
     need(delivered.value("scope","")=="chain"&&delivered.value("fg_declared","")=="off"&&delivered.value("cancel",true)==false,"Scope and declaration reach the backend unchanged");
     s.publish_pair({{"state","complete"},{"revision",delivered["revision"]},{"files",0},{"scope","chain"}});
     const auto presents=s.handle(req(s,"CapturePair",{{"scope","presents"}}),13);
     need(presents["ok"]==true&&s.status()["frame_pair"]["fg_declared"]=="unknown","fg_declared defaults to unknown");
     need(s.dispatch_pair_request(14,[&](const lab::json& a){delivered=a;return true;})&&delivered["scope"]=="presents","Presents scope delivered");
     lab::Controller unscoped;unscoped.enable_nr_frame_control("synthetic-input-real-nr",true,true);unscoped.enable_chain_capture();on(unscoped);
     need(unscoped.handle(req(unscoped,"CapturePair"),3)["ok"]==true,"Legacy empty params unchanged with a chain backend attached");
     need(unscoped.dispatch_pair_request(4,[&](const lab::json& a){delivered=a;return true;})&&!delivered.contains("scope"),"Legacy request carries no scope");}
    std::cout<<"PASS: pair control, explicit arming, bounded ownership, cancellation, rebuild interruption and failure, chain scope\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
