// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include <iostream>
namespace {
void need(bool v,const char* why){if(!v)throw std::runtime_error(why);}
lab::json request(lab::Controller& c,bool scene=true){auto s=c.status();return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id","preparation-test"},
    {"session_id",s["session_id"]},{"expected_revision",s["revision"]},{"method","PrepareNr"},{"params",{{"scene_confirmed",scene}}}};}
}
int main(){try{
    lab::Controller integrated;integrated.enable_nr_preparation(true);
    need(integrated.handle(request(integrated,false),1)["error"]["code"]=="scene_required","Integrated prepare still needs current manual confirmation");
    auto prepare_only=request(integrated);const auto receipt=integrated.handle(prepare_only,2);
    need(receipt["ok"]==true&&integrated.handle(prepare_only,3)==receipt,"Integrated OFF prepare idempotent");
    need(integrated.take_nr_preparation(4)&&!integrated.take_nr_preparation(5),"Integrated OFF prepare consumed once");
    integrated.enable_nr_frame_control("synthetic-input-real-nr");integrated.publish_nr_runtime({{"profile","synthetic-standalone-fixture"},{"state","ready"}});
    const auto off_action=integrated.take_nr_mode_request(6);need(off_action&&(*off_action)["mode"]=="off","Prepared runtime must request OFF, never auto-Evaluate");
    need(integrated.handle(request(integrated),7)["error"]["code"]=="already_prepared","Do not reprepare ready runtime");
    lab::Controller expired_off;expired_off.enable_nr_preparation(true);expired_off.handle(request(expired_off),1);
    need(!expired_off.take_nr_preparation(11002)&&expired_off.status()["nr_lifecycle"]["desired_mode"]=="off","Expired diagnostic prepare does not load model");
    lab::Controller c;need(c.handle(request(c),1)["error"]["code"]=="unsupported","Bare backend must not prepare NR");
    c.enable_nr_preparation();
    for(unsigned i=0;i<300;++i){c.frame_boundary(i,i);need(!c.take_nr_preparation(i),"CPU frames must not auto-prepare NR");}
    need(c.status()["capabilities"]["nr_control"]==false&&c.status()["nr_runtime"].is_null(),"Preparation is not rendering capability");
    need(c.handle(request(c,false),300)["error"]["code"]=="scene_required","Explicit manual readiness required");
    auto p=request(c);auto accepted=c.handle(p,301);need(accepted["ok"]==true&&c.handle(p,302)==accepted,"Preparation request is idempotent");
    auto other=request(c);other["client_id"]="other";need(c.handle(other,303)["error"]["code"]=="control_busy","Single preparation writer");
    need(c.handle(request(c),304)["error"]["code"]=="already_prepared","Cannot schedule repeated preparation");
    need(c.take_nr_preparation(305)&&!c.take_nr_preparation(306),"Prepare consumed once");
    need(c.status()["nr_preparation"]["automatically_enables_nr"]==false&&c.status()["capabilities"]["nr_control"]==false,"Prepare must not enable NR");
    c.enable_nr_frame_control("synthetic-input-real-nr");need(c.status()["nr_preparation"]["state"]=="ready","Real backend supplies readiness");
    lab::Controller cancelled;cancelled.enable_nr_preparation();need(cancelled.handle(request(cancelled),1)["ok"]==true,"Schedule preparation");
    need(!cancelled.take_nr_preparation(11002)&&cancelled.status()["nr_preparation"]["state"]=="waiting_for_user","Expired unconsumed preparation is cancelled");
    auto stale=request(cancelled);stale["expected_revision"]=0u;need(cancelled.handle(stale,11003)["error"]["code"]=="stale_revision","Stale preparation rejected");
    need(cancelled.handle(request(cancelled),11004)["ok"]==true&&cancelled.take_nr_preparation(11005),"Can explicitly prepare after cancelled wait");
    for(const auto* terminal:{"failed","stopped"}){
        for(bool queued:{false,true}){
            lab::Controller dead;dead.enable_nr_preparation();
            if(queued)need(dead.handle(request(dead),1)["ok"]==true,"Queue before host failure");
            dead.publish_host({{"backend","standalone-d3d12"},{"state",terminal},{"error","test host fault"}});
            need(!dead.status()["capabilities"]["nr_preparation"].get<bool>(),"Terminal host must revoke preparation capability");
            need(!dead.take_nr_preparation(2),"Terminal host must cancel unconsumed preparation");
            need(dead.handle(request(dead),3)["error"]["code"]=="backend_failed","Terminal host must reject new preparation");
            dead.publish_host({{"backend","standalone-d3d12"},{"state","connected"}});
            need(!dead.status()["capabilities"]["nr_preparation"].get<bool>(),"Late host status cannot revive preparation");
            bool refused=false;try{dead.enable_nr_frame_control("game-rr-experimental-nr");}catch(const std::logic_error&){refused=true;}
            need(refused,"Late runtime readiness cannot revive failed host");
        }
        for(bool inflight:{false,true}){
            lab::Controller dead;dead.enable_nr_preparation();dead.handle(request(dead),1);dead.take_nr_preparation(2);
            dead.enable_nr_frame_control("game-rr-experimental-nr");
            auto on=request(dead);on["method"]="SetNrMode";on["params"]={{"mode","on"}};
            need(dead.handle(on,3)["ok"]==true,"Queue ON before failure");
            auto action=inflight?dead.take_nr_mode_request(4):std::optional<lab::json>{};
            dead.publish_host({{"backend","standalone-d3d12"},{"state",terminal},{"error","test host fault"}});
            if(inflight)dead.acknowledge_nr_mode(action->at("revision"),42,true,true,true);
            const auto s=dead.status();
            need(!s["capabilities"]["nr_control"].get<bool>()&&!s["capabilities"]["nr_frame_control"].get<bool>(),"Host failure revokes all NR controls including after late ack");
            need(s["nr_frame_control"]["observed_mode"]=="unknown","Host failure must not claim OFF or successful display");
            need(!dead.take_nr_mode_request(5),"Host failure cancels pending ON");
            auto again=request(dead);again["method"]="SetNrMode";again["params"]={{"mode","on"}};
            need(dead.handle(again,6)["error"]["code"]=="backend_failed","Host failure refuses new ON");
        }
    }
    std::cout<<"PASS explicit NR preparation, idle startup, leases, idempotency and terminal host veto\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
