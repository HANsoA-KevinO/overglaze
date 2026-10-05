// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include <iostream>
namespace {
void need(bool v,const char* why){if(!v)throw std::runtime_error(why);}
lab::json mode(lab::Controller& c,bool on,const char* client="daily"){const auto s=c.status();return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id",client},
    {"session_id",s["session_id"]},{"expected_revision",s["revision"]},{"method","SetNrMode"},{"params",{{"mode",on?"on":"off"}}}};}
lab::json runtime(const char* state="ready",unsigned serial=0){return {{"profile","cyberpunk-rr-experimental-v1"},{"state",state},{"rebuild_serial",serial},{"rebuild_frame",70u},{"bypass_frame",70u}};}
void ready(lab::Controller& c){c.enable_nr_frame_control("game-rr-experimental-nr",true,true);c.publish_nr_runtime(runtime());}
void ack(lab::Controller& c,unsigned time,unsigned frame,bool on){auto a=c.take_nr_mode_request(time);need(a&&a->at("mode")== (on?"on":"off"),"Expected current intent");c.acknowledge_nr_mode(a->at("revision"),frame,true,on,on);}
}
int main(){try{
    lab::Controller c;c.enable_nr_preparation(true);
    need(c.status()["capabilities"]["nr_intent_control"]==true&&!c.status()["capabilities"]["nr_control"].get<bool>(),"Intent control is not claimed rendering readiness");
    for(unsigned i=0;i<300;++i){c.frame_boundary(i,i);need(!c.take_nr_preparation(i),"Startup never loads NR");}
    auto on=mode(c,true);auto accepted=c.handle(on,301);need(accepted["ok"]==true&&c.handle(on,302)==accepted,"One switch is idempotent and needs no scene checkbox");
    need(c.take_nr_preparation(303)&&!c.take_nr_preparation(304)&&!c.take_nr_mode_request(305),"Prepare once; never dispatch NR before readiness");
    ready(c);ack(c,306,10,true);need(c.status()["nr_frame_control"]["observed_mode"]=="on","Automatic first ON gets actual acknowledgement");
    c.interrupt_nr_for_rebuild(1,70);c.publish_nr_runtime(runtime("draining",1));
    need(c.status()["nr_lifecycle"]["desired_mode"]=="on"&&!c.take_nr_mode_request(307),"Preserve intent while GPU drains");
    c.publish_nr_runtime(runtime("ready",1));ack(c,308,71,true);
    c.interrupt_nr_for_rebuild(2,80);c.publish_nr_runtime(runtime("draining",2));
    need(c.handle(mode(c,false),309)["ok"]==true,"OFF can cancel automatic resume during rebuild");
    c.publish_nr_runtime(runtime("ready",2));ack(c,310,81,false);
    need(!c.take_nr_mode_request(311),"No stale ON remains");
    need(c.handle(mode(c,true),312)["ok"]==true,"New ON");auto old=c.take_nr_mode_request(313);need(old.has_value(),"Old command in flight");
    need(c.handle(mode(c,false),314)["ok"]==true,"OFF accepted while prior ON is in flight");
    c.acknowledge_nr_mode(old->at("revision"),82,true,true,true);ack(c,315,83,false);
    c.handle(mode(c,true),316);c.interrupt_nr_for_rebuild(3,90);c.publish_nr_runtime(runtime("draining",3));c.frame_boundary(91,11000);
    c.publish_nr_runtime(runtime("ready",3));ack(c,11001,92,false);need(c.status()["nr_lifecycle"]["desired_mode"]=="off","Disconnect cannot resume ON");
    c.publish_host({{"state","failed"},{"error","device-fault"}});c.publish_nr_runtime(runtime());
    need(c.handle(mode(c,true),11002)["error"]["code"]=="backend_failed"&&!c.take_nr_mode_request(11003),"Failure is sticky");
    for(bool consumed:{false,true}){lab::Controller a;a.enable_nr_preparation(true);a.handle(mode(a,true),1);
        if(consumed)need(a.take_nr_preparation(2),"Begin preparation");
        need(a.handle(mode(a,false),3)["ok"]==true,"Cancel startup ON");
        if(consumed){ready(a);ack(a,4,1,false);}else need(!a.take_nr_preparation(4),"Unstarted prepare cancelled without loading DLL");}
    lab::Controller expired;expired.enable_nr_preparation(true);expired.handle(mode(expired,true),1);
    need(!expired.take_nr_preparation(11002)&&expired.status()["nr_lifecycle"]["desired_mode"]=="off","Lease expiry cancels startup ON");
    lab::Controller owner;owner.enable_nr_preparation(true);owner.handle(mode(owner,true),1);
    need(owner.handle(mode(owner,false,"other"),2)["error"]["code"]=="control_busy","Automatic lifecycle still has one writer");
    auto stale=mode(owner,false);stale["expected_revision"]=0u;need(owner.handle(stale,3)["error"]["code"]=="stale_revision","Stale intent rejected");
    std::cout<<"PASS automatic standby, lazy prepare, current intent, rebuild, cancellation, lease and terminal veto\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
