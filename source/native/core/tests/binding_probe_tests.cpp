// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include "lab_binding_probe.hpp"
#include <iostream>
namespace {
unsigned checks=0;
void need(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
lab::json request(lab::Controller& c,const char* method,lab::json p=lab::json::object()){
    const auto s=c.status();return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id","diagnostic"},
        {"session_id",s["session_id"]},{"expected_revision",s["revision"]},{"method",method},{"params",p}};
}
}
int main(){try{
    using namespace lab::diagnostic;
    BindingProbe b;need(!b.active()&&!b.admit(1,1,1,true),"No startup restore");
    need(b.arm(100)&&!b.arm(101),"One bounded arm");
    need(!b.admit(101,1,1,false)&&b.status().phase==BindingProbePhase::failed,"Missing bindings stops without work");
    need(!b.arm(102),"Failure does not rearm");
    BindingProbe deadline;need(deadline.arm(100),"Deadline arm");
    need(deadline.admit(60099,1,1,true),"Before deadline");deadline.recorded(1,1);
    need(!deadline.admit(60100,2,2,true)&&deadline.status().restores==1,"Hard callback deadline");
    BindingProbe backwards;need(backwards.arm(100)&&!backwards.admit(99,1,1,true),"Clock reversal stops");
    BindingProbe count;need(count.arm(100),"Count arm");
    for(unsigned i=1;i<=BindingProbe::max_restores;++i){if(!count.admit(101,i,i,true))throw std::runtime_error("Unexpected capacity refusal");count.recorded(i,i);}
    need(!count.active()&&!count.admit(102,16385,16385,true)&&count.status().restores==16384,"Fixed callback budget");
    BindingProbe cancel;need(cancel.arm(1),"Cancel arm");cancel.cancel();need(!cancel.admit(2,1,1,true)&&!cancel.arm(3),"Cancellation sticky");
    BindingProbe order;need(order.arm(1)&&order.admit(2,2,3,true),"Order arm");order.recorded(2,3);
    need(!order.admit(3,2,4,true)&&order.status().phase==BindingProbePhase::failed,"Repeated frame refused");
    BindingProbe overflow;need(!overflow.arm(UINT64_MAX),"No deadline overflow");
    lab::Controller c;c.enable_nr_preparation(true);c.enable_embedded_control();
    unsigned arms=0,cancels=0;bool accept=true;
    c.enable_binding_probe([&](bool stop){if(stop){++cancels;return true;}if(!accept)return false;++arms;return true;});
    const lab::json scene={{"scene_confirmed",true},{"fg_disabled_confirmed",true}};
    need(c.handle_embedded("ProbeBindingRestore",scene,1)["error"]["code"]=="unsupported","No accidental embedded probe");
    need(c.handle(request(c,"TakeControl"),2)["ok"]==true,"Explicit takeover");
    need(c.handle(request(c,"ProbeBindingRestore",scene),3)["error"]["code"]=="nr_not_off"&&arms==0,"Prepared OFF required");
    c.enable_nr_frame_control("game-rr-experimental-nr",true,true);
    c.publish_nr_runtime({{"profile","007-first-light-rr-v1"},{"state","ready"},{"pending_gpu",false},{"evaluates",0}});
    const auto a=c.take_nr_mode_request(4);need(bool(a),"Initial OFF mailbox");c.acknowledge_nr_mode(a->at("revision"),20,true,false,false);
    need(c.handle(request(c,"ProbeBindingRestore"),5)["error"]["code"]=="scene_required","Explicit scene requirement");
    accept=false;need(c.handle(request(c,"ProbeBindingRestore",scene),6)["error"]["code"]=="probe_not_ready"&&arms==0,"Busy dispatch schedules nothing");accept=true;
    auto start=request(c,"ProbeBindingRestore",scene);const auto result=c.handle(start,7);
    need(result["ok"]==true&&arms==1,"One accepted restore-only probe");
    need(c.handle(start,8)==result&&arms==1,"Duplicate request returns cached receipt, no rearm");
    need(c.handle(request(c,"ProbeBindingRestore",scene),9)["error"]["code"]=="consumed","New id cannot rearm same process");
    need(c.handle(request(c,"SetNrMode",{{"mode","on"}}),10)["error"]["code"]=="diagnostic_session","Probe never enables NR");
    c.frame_boundary(21,11011);need(cancels==1&&c.status()["control_owner"]=="@embedded","Disconnect cancels probe and returns ownership");
    need(c.handle_embedded("SetNrMode",{{"mode","on"}},11012)["error"]["code"]=="diagnostic_session","No accidental ON after probe expiry");
    lab::Controller prep;prep.enable_nr_preparation(true);prep.enable_embedded_control();
    unsigned prepares=0,stops=0,other_arms=0;
    prep.enable_preparation_probe([&](bool stop){if(stop){++stops;return true;}++prepares;return true;});
    prep.enable_binding_probe([&](bool stop){if(!stop)++other_arms;return true;});
    need(prep.handle_embedded("ProbeInputPreparation",scene,1)["error"]["code"]=="unsupported","Preparation external only");
    need(prep.handle(request(prep,"TakeControl"),2)["ok"]==true,"Preparation takeover");
    need(prep.handle(request(prep,"ProbeInputPreparation",scene),3)["error"]["code"]=="nr_not_off","Preparation rejects unprepared runtime");
    prep.enable_nr_frame_control("game-rr-experimental-nr",true,true);
    prep.publish_nr_runtime({{"profile","007-first-light-rr-v1"},{"state","ready"},{"pending_gpu",false},{"evaluates",0}});
    const auto initial=prep.take_nr_mode_request(4);need(bool(initial),"Preparation OFF mailbox");
    prep.acknowledge_nr_mode(initial->at("revision"),20,true,false,false);
    auto runtime=lab::json{{"profile","007-first-light-rr-v1"},{"state","ready"},{"pending_gpu",true},{"evaluates",0}};prep.publish_nr_runtime(runtime);
    need(prep.handle(request(prep,"ProbeInputPreparation",scene),5)["error"]["code"]=="nr_not_off"&&prepares==0,"Preparation rejects pending GPU");
    runtime["pending_gpu"]=false;runtime["evaluates"]=1;prep.publish_nr_runtime(runtime);
    need(prep.handle(request(prep,"ProbeInputPreparation",scene),6)["error"]["code"]=="nr_not_off"&&prepares==0,"Preparation rejects prior NR evaluation");
    runtime["evaluates"]=0;prep.publish_nr_runtime(runtime);
    need(prep.handle(request(prep,"ProbeInputPreparation"),7)["error"]["code"]=="scene_required","Preparation needs this scene confirmation");
    const auto prep_request=request(prep,"ProbeInputPreparation",scene);const auto accepted=prep.handle(prep_request,8);
    need(accepted["ok"]==true&&prepares==1&&other_arms==0,"Only preparation callback armed");
    need(prep.handle(prep_request,9)==accepted&&prepares==1,"Preparation replay idempotent");
    need(prep.handle(request(prep,"ProbeBindingRestore",scene),10)["error"]["code"]=="consumed"&&other_arms==0,"No mixed diagnostic in same process");
    need(prep.handle(request(prep,"SetNrMode",{{"mode","compute-only"}}),11)["error"]["code"]=="diagnostic_session","No Evaluate after preparation diagnostic");
    need(!prep.status()["capabilities"]["input_preparation_probe"].get<bool>(),"Preparation capability consumed");
    auto cancel_request=request(prep,"CancelInputPreparationProbe");auto cancelled=prep.handle(cancel_request,12);
    need(cancelled["ok"]==true&&stops==1,"Explicit preparation cancel");
    need(prep.handle(cancel_request,13)==cancelled&&stops==1,"Cancel retry idempotent");
    prep.publish_preparation_probe({{"state","complete"},{"recorded",3},{"retired",2},{"pending_gpu",true}});
    need(prep.status()["input_preparation_probe"]["pending_gpu"]==true&&prep.status()["nr_runtime"]["evaluates"]==0,"Preparation completion distinct from NR counters");
    prep.frame_boundary(21,11014);
    need(stops==2&&prep.status()["control_owner"]=="@embedded","Lease expiry cancels preparation");
    std::cout<<"PASS binding probe "<<checks<<" policy/protocol checks; no GPU or game access\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
