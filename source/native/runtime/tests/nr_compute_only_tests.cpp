// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include "lab_nr_live_api.hpp"
#include <iostream>
namespace {
void need(bool v,const char* m){if(!v)throw std::runtime_error(m);}
lab::json runtime(){return {{"profile","007-first-light-rr-v1"},{"state","ready"}};}
void ack(lab::Controller& c,const lab::json& a,std::uint64_t f){
    auto r=runtime();if(a.contains("settings"))r["settings"]={{"observed_revision",a.at("revision")},{"frame",f},{"read_mask",3u},{"style_read",true},{"observed",a.at("settings")}};
    c.publish_nr_runtime(r);c.acknowledge_nr_mode(a.at("revision"),f,true,lab::nr::executes(a.at("mode").get<std::string>()),a.at("mode")=="on");
}
lab::json request(lab::Controller& c,const char* method,lab::json params={}){
    const auto s=c.status();return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id","diagnosis"},{"session_id",s.at("session_id")},
        {"expected_revision",s.at("revision")},{"method",method},{"params",params.is_null()?lab::json::object():params}};
}
}
int main(){try{
    lab::live::Status s;lab::nr::Settings settings{.25f,.75f,2};
    need(lab::live::apply_request(s,true,1,2,&settings)&&s.requested_on==2,"ABI compute-only mode");
    need(!lab::live::apply_request(s,true,1,1,nullptr)&&s.requested_on==2,"Duplicate cannot change selection");
    need(!lab::live::apply_request(s,true,2,3,nullptr)&&s.request_revision==1,"Unknown mode refused atomically");
    lab::Controller c;c.enable_nr_preparation(true);c.enable_embedded_control();
    need(c.handle_embedded("SetNrMode",{{"mode","compute-only"}},1)["error"]["code"]=="unsupported","No guessed backend capability");
    c.enable_nr_compute_only();
    need(c.handle_embedded("SetNrMode",{{"mode","compute-only"}},2)["ok"]==true&&c.take_nr_preparation(3),"First diagnostic intent prepares");
    c.enable_nr_frame_control("game-rr-experimental-nr",true,true);c.enable_display_capture();c.publish_nr_runtime(runtime());
    auto a=c.take_nr_mode_request(4);need(a&&a->at("mode")=="compute-only","Prepared intent preserves mode");
    bool rejected=false;try{c.acknowledge_nr_mode(a->at("revision"),10,true,true,true);}catch(...){rejected=true;}
    need(rejected,"Diagnostic cannot acknowledge game output selected");ack(c,*a,10);
    auto status=c.status();need(status["nr_frame_control"]["nr_evaluated"]==true&&status["nr_frame_control"]["nr_output_selected"]==false,"Executed != selected");
    need(status["nr_lifecycle"]["desired_mode"]=="compute-only","Truthful lifecycle label");
    need(c.handle_embedded("SetNrSettings",{{"tone",.25},{"structure",.75},{"style",2}},5)["ok"]==true,"Settings while compute-only");
    a=c.take_nr_mode_request(6);need(a&&a->at("mode")=="compute-only","Settings cannot silently select output");ack(c,*a,11);
    need(c.handle_embedded("CaptureDisplayPair",{{"scene_confirmed",true},{"fg_disabled_confirmed",true}},7)["error"]["code"]=="nr_not_off","Compute-only is not an OFF baseline");
    need(c.handle_embedded("CapturePair",lab::json::object(),8)["ok"]==true,"Explicit diagnostic color capture");
    unsigned delivered=0;need(c.dispatch_pair_request(9,[&](const lab::json&){++delivered;return true;})&&delivered==1,"One delivery in compute-only");
    need(!c.dispatch_pair_request(9,[&](const lab::json&){++delivered;return true;})&&delivered==1,"No automatic recapture");
    c.publish_pair({{"state","complete"},{"revision",0u},{"files",4}});
    c.interrupt_nr_for_rebuild(1,12);c.publish_nr_runtime({{"profile","007-first-light-rr-v1"},{"state","draining"},{"rebuild_serial",1u}});
    need(!c.take_nr_mode_request(10),"No dispatch during rebuild");c.publish_nr_runtime(runtime());
    a=c.take_nr_mode_request(11);need(a&&a->at("mode")=="compute-only","Rebuild preserves current mode, not ON");ack(c,*a,13);
    const auto take=request(c,"TakeControl");need(c.handle(take,12)["ok"]==true,"External takeover");
    c.frame_boundary(14,12000);a=c.take_nr_mode_request(12001);
    need(a&&a->at("mode")=="off","Lease expiry stops diagnostic computation");ack(c,*a,14);
    need(c.status()["nr_lifecycle"]["desired_mode"]=="off","Lease does not resume diagnostic");
    std::cout<<"PASS compute-only: capability, ABI, execution/selection, settings, capture, rebuild, lease OFF\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
