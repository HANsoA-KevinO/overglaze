// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include "lab_nr_live_api.hpp"
#include <iostream>
namespace {
void need(bool v,const char* why){if(!v)throw std::runtime_error(why);}
lab::json external(lab::Controller& c,const char* method,lab::json params=lab::json::object(),const char* client="cli"){
    const auto s=c.status();return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id",client},{"session_id",s["session_id"]},
        {"expected_revision",s["revision"]},{"method",method},{"params",params}};
}
lab::json runtime(){return {{"profile","cyberpunk-rr-experimental-v1"},{"state","ready"}};}
void ack(lab::Controller& c,const lab::json& action,std::uint64_t frame){
    const bool on=action.at("mode")=="on";auto r=runtime();
    if(action.contains("settings"))r["settings"]={{"observed_revision",action.at("revision")},{"frame",frame},{"read_mask",3u},{"style_read",true},{"observed",action.at("settings")}};
    c.publish_nr_runtime(r);c.acknowledge_nr_mode(action.at("revision"),frame,true,on,on);
}
// The settings the runtime client hands the bridge for a mailbox action.
lab::nr::Settings to_settings(const lab::json& v){
    return {v.at("tone").get<float>(),v.at("structure").get<float>(),v.at("style").get<unsigned>(),v.value("exposure_stops",0.f),v.value("exposure_auto",0u),v.value("compare_split",0u),
        v.value("skin",1.f),v.value("automask",0u),v.value("extrapolate",0u),v.value("extrapolate_factor",2.f),v.value("network",0u),
        v.value("compare_split_position",lab::nr::Settings::default_compare_split_position)};
}
}
int main(){try{
    lab::live::Status runtime_status;lab::nr::Settings staged{.2f,.8f};
    need(lab::live::apply_request(runtime_status,true,1,1,&staged)&&runtime_status.requested_on==1&&runtime_status.settings_requested_revision==1&&runtime_status.requested_settings.tone==.2f,"First ON atomically carries staged settings to runtime");
    need(!lab::live::apply_request(runtime_status,true,1,1,&staged),"Repeated bridge revision rejected");
    need(lab::live::apply_request(runtime_status,true,2,0,nullptr)&&runtime_status.requested_on==0,"Atomic OFF");
    need(lab::live::apply_request(runtime_status,true,3,1,&staged)&&runtime_status.settings_requested_revision==3,"Re-enable with same values still produces a new actual read revision");
    need(!lab::live::apply_request(runtime_status,false,4,1,&staged)&&runtime_status.request_revision==3,"Not-ready transaction makes no partial change");
    lab::Controller c;c.enable_nr_preparation(true);c.enable_embedded_control();
    need(c.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"style",2}},1)["ok"]==true,"Stage verified Style while OFF");
    need(c.status()["nr_settings_request"]["values"]["exposure_stops"]==0.0,"Omitted exposure keeps the default");
    need(c.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"exposure_stops",4.5}},1)["ok"]==true,"Stage host exposure while OFF");
    need(c.status()["nr_settings_request"]["values"]["exposure_stops"]==4.5&&c.status()["nr_settings_request"]["values"]["style"]==2,"Exposure staged, Style preserved");
    need(c.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"exposure_stops",-13}},1)["error"]["code"]=="bad_config","Reject out-of-range exposure");
    need(c.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"style",2}},1)["ok"]==true&&c.status()["nr_settings_request"]["values"]["exposure_stops"]==4.5,"Omitted exposure keeps the staged value");
    need(c.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"exposure_stops",0}},1)["ok"]==true,"Restore default exposure for the remaining checks");
    // ABI25 edit extrapolation: staged while OFF like every panel setting.
    need(c.status()["nr_settings_request"]["values"]["extrapolate"]==0&&c.status()["nr_settings_request"]["values"]["extrapolate_factor"]==2.0,"Extrapolation off by default at factor 2");
    need(c.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"extrapolate",true},{"extrapolate_factor",3.5}},1)["ok"]==true,"Stage extrapolation while OFF");
    for(const auto& bad:{lab::json{{"extrapolate_factor",4.5}},lab::json{{"extrapolate_factor",.5}},lab::json{{"extrapolate",3}}}){
        auto p=bad;p["tone"]=1;p["structure"]=1;const auto revision=c.status()["revision"];
        need(c.handle_embedded("SetNrSettings",p,1)["error"]["code"]=="bad_config"&&c.status()["revision"]==revision,"Out-of-range extrapolation rejected without mutation");}
    need(c.status()["nr_settings_request"]["values"]["extrapolate"]==1&&c.status()["nr_settings_request"]["values"]["extrapolate_factor"]==3.5,"Extrapolation staged");
    // ABI27 compare split position: staged while OFF, refused out of range without mutation.
    need(c.status()["nr_settings_request"]["values"]["compare_split_position"]==.5,"Split in the middle by default");
    need(c.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"compare_split",true},{"compare_split_position",.7}},1)["ok"]==true,"Stage the split position while OFF");
    for(const auto& bad:{lab::json{{"compare_split_position",1.5}},lab::json{{"compare_split_position",-.5}},lab::json{{"compare_split_position",false}}}){
        auto p=bad;p["tone"]=1;p["structure"]=1;const auto revision=c.status()["revision"];
        need(c.handle_embedded("SetNrSettings",p,1)["error"]["code"]=="bad_config"&&c.status()["revision"]==revision,"Out-of-range split position rejected without mutation");}
    need(c.status()["nr_settings_request"]["values"]["compare_split_position"]==.7f&&c.status()["nr_settings_request"]["values"]["compare_split"]==1,"Split position staged");
    for(const auto& style:{lab::json(-1),lab::json(3),lab::json(.5),lab::json(1.0),lab::json(true),lab::json("A"),lab::json(4294967296ULL)}){
        const auto revision=c.status()["revision"];
        need(c.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"style",style}},1)["error"]["code"]=="bad_config","Reject guessed, fractional or out-of-range Style");
        need(c.status()["revision"]==revision&&c.status()["nr_settings_request"]["values"]["style"]==2,"Rejected Style does not mutate intent");}
    need(c.status()["control"]["source"]=="in-game","Local default owner");
    need(c.handle(external(c,"SetNrMode",{{"mode","on"}}),1)["error"]["code"]=="takeover_required","Legacy client cannot steal writer");
    need(c.handle(external(c,"Hello",lab::json::object(),"@embedded"),2)["error"]["code"]=="reserved_client","Pipe cannot spoof local writer");
    for(unsigned i=0;i<5000;++i)need(c.handle_embedded("SetNrSettings",{{"tone",.25f},{"structure",.75f}},3+i)["ok"]==true,"Local editing is bounded, not a lifetime id cache");
    need(c.status()["nr_settings_request"]["values"]["style"]==2,"Legacy two-slider editing preserves chosen Style");
    need(!c.take_nr_preparation(6000)&&!c.take_nr_mode_request(6000),"Editing OFF never starts NR");
    need(c.handle_embedded("SetNrMode",{{"mode","on"}},6001)["ok"]==true&&c.take_nr_preparation(6002),"Local first ON prepares once");
    c.enable_nr_frame_control("game-rr-experimental-nr",true,true);c.publish_nr_runtime(runtime());
    auto action=c.take_nr_mode_request(6003);need(action&&action->at("settings")["tone"]==.25f,"First ON carries staged values");
    need(action->at("settings")["extrapolate"]==1&&action->at("settings")["extrapolate_factor"]==3.5,"First ON carries the staged extrapolation");
    need(action->at("settings")["compare_split_position"]==.7f&&to_settings(action->at("settings")).compare_split_position==.7f,"First ON carries the staged split position to the bridge");ack(c,*action,10);
    c.frame_boundary(11,600000);need(c.status()["nr_lifecycle"]["desired_mode"]=="on"&&!c.take_nr_mode_request(600001),"Hidden panel needs no heartbeat");
    auto claim=external(c,"TakeControl");auto accepted=c.handle(claim,600002);
    need(accepted["ok"]==true&&c.handle(claim,600003)==accepted,"External takeover idempotent");
    need(c.handle_embedded("SetNrMode",{{"mode","off"}},600004)["error"]["code"]=="control_busy","Single writer");
    need(c.handle(external(c,"TakeControl",lab::json::object(),"other"),600005)["error"]["code"]=="control_busy","Second external cannot steal");
    c.frame_boundary(12,620000);need(c.status()["control"]["source"]=="in-game"&&c.status()["nr_lifecycle"]["desired_mode"]=="off","Disconnect safe OFF and return");
    action=c.take_nr_mode_request(620001);need(action&&action->at("mode")=="off","OFF not merely UI state");ack(c,*action,12);
    need(c.handle_embedded("SetNrMode",{{"mode","on"}},620002)["ok"]==true,"Local resumes only on new intent");
    action=c.take_nr_mode_request(620003);ack(c,*action,13);
    need(c.handle(external(c,"TakeControl"),620004)["ok"]==true,"Retake");
    c.publish_pair({{"state","complete"},{"revision",0u},{"files",4},{"frame",13u}});
    need(c.handle(external(c,"ReleaseControl"),620005)["ok"]==true,"Explicit release");
    need(!c.take_pair_request(620005)&&c.status()["frame_pair"]["state"]=="complete","Release must not cancel a completed capture or hide the overlay indefinitely");
    need(c.handle_embedded("CancelCapturePair",lab::json::object(),620005)["ok"]==true&&!c.take_pair_request(620005),"Cancel on a completed capture is a harmless no-op");
    action=c.take_nr_mode_request(620006);need(action&&action->at("mode")=="off","Release requests actual OFF");ack(c,*action,14);
    need(c.status()["control"]["source"]=="in-game","Release returns writer");
    c.interrupt_nr_for_rebuild(1,15);c.publish_nr_runtime({{"profile","cyberpunk-rr-experimental-v1"},{"state","draining"},{"rebuild_serial",1u}});
    need(c.handle_embedded("SetNrSettings",{{"tone",.8},{"structure",.2}},620007)["ok"]==true,"Rebuilding allows staged edit without Evaluate");
    c.publish_host({{"state","failed"},{"error","device removed"}});
    need(c.handle_embedded("SetNrMode",{{"mode","on"}},620008)["error"]["code"]=="backend_failed","No unsafe recovery");
    // ABI26 network preset, end to end through the controller and the runtime's
    // own decision (lab::live::network_rebuild_needed). No GPU: the bridge's
    // Feature creation is modelled by Status::applied_network, and its rebuild
    // by what begin_rebuild does to Status (rebuild_serial+1, requested_on=0).
    // The real bridge is exercised by the real-NR controller fixture.
    {lab::Controller n;n.enable_nr_preparation(true);n.enable_embedded_control();
     need(n.status()["nr_settings_request"]["values"]["network"]==0,"Network 0 (the DLL's default) by default");
     need(n.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"network",1}},1)["ok"]==true,"Stage network 1 while OFF");
     for(const auto& bad:{lab::json(3),lab::json(-1),lab::json(1.5),lab::json(true),lab::json("2")}){const auto revision=n.status()["revision"];
         need(n.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"network",bad}},1)["error"]["code"]=="bad_config"&&n.status()["revision"]==revision,
              "Out-of-range or non-integer network rejected without mutation");}
     need(n.status()["nr_settings_request"]["values"]["network"]==1,"Network staged");
     need(n.handle_embedded("SetNrMode",{{"mode","on"}},2)["ok"]==true&&n.take_nr_preparation(3),"First ON prepares");
     n.enable_nr_frame_control("game-rr-experimental-nr",true,true);
     // The first Feature is created before any settings reach the bridge: network 0.
     lab::live::Status bridge;bridge.feature_generation=1;bridge.applied_network=0;
     unsigned rebuilds=0,acknowledged=0;std::uint64_t frame=20;
     const auto ready=[&]{auto r=runtime();r["rebuild_serial"]=bridge.rebuild_serial;n.publish_nr_runtime(r);};
     // One mailbox action into the bridge, then one frame: either evaluated
     // (acknowledged), or the bridge rebuilt and the controller will resume the
     // user's intent with a new action once the runtime is ready again.
     const auto deliver=[&](const lab::json& action){
         const auto mode=action.at("mode")=="on"?1u:0u;
         if(action.contains("settings")){const auto s=to_settings(action.at("settings"));need(lab::live::apply_request(bridge,true,action.at("revision").get<std::uint64_t>(),mode,&s),"Bridge accepts the request");}
         else need(lab::live::apply_request(bridge,true,action.at("revision").get<std::uint64_t>(),mode,nullptr),"Bridge accepts the mode");
         ++frame;
         if(lab::live::network_rebuild_needed(bridge)){
             ++rebuilds;++bridge.rebuild_serial;bridge.requested_on=0; // begin_rebuild: nothing recorded on this frame
             n.interrupt_nr_for_rebuild(bridge.rebuild_serial,frame);
             auto draining=runtime();draining["state"]="draining";draining["rebuild_serial"]=bridge.rebuild_serial;n.publish_nr_runtime(draining);
             need(!n.take_nr_mode_request(frame),"Nothing is dispatched while the Feature is rebuilt");
             bridge.applied_network=bridge.requested_settings.network;++bridge.feature_generation; // prepare(): created with the requested network
             ready();return false;}
         ++acknowledged;ack(n,action,frame);return true;};
     ready();
     auto action=n.take_nr_mode_request(4);need(action&&action->at("mode")=="on"&&action->at("settings")["network"]==1,"First ON carries the staged network");
     need(!deliver(*action)&&rebuilds==1&&bridge.applied_network==1,"A network the Feature was not created with is never evaluated: one rebuild");
     action=n.take_nr_mode_request(5);need(action&&action->at("mode")=="on"&&action->at("settings")["network"]==1,"The controller resumes ON with the same network after the rebuild");
     need(deliver(*action)&&rebuilds==1&&!n.take_nr_mode_request(6),"The recreated Feature evaluates it; no second rebuild");
     // Switching while ON: exactly one request, one rebuild, one resumed ON.
     need(n.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"network",2}},7)["ok"]==true,"Switch to network 2 while ON");
     action=n.take_nr_mode_request(8);need(action&&action->at("settings")["network"]==2,"One request carries the new network");
     need(!deliver(*action)&&rebuilds==2&&bridge.applied_network==2,"The switch rebuilds the Feature exactly once");
     action=n.take_nr_mode_request(9);need(action&&action->at("mode")=="on"&&action->at("settings")["network"]==2,"ON resumes with network 2");
     need(deliver(*action)&&rebuilds==2&&!n.take_nr_mode_request(10),"Network 2 evaluated on its own Feature");
     // Other settings never rebuild; neither does OFF, nor ON again with an unchanged network.
     need(n.handle_embedded("SetNrSettings",{{"tone",.5},{"structure",1}},11)["ok"]==true,"Tone change");
     action=n.take_nr_mode_request(12);need(action&&action->at("settings")["network"]==2&&deliver(*action)&&rebuilds==2,"A Tone change keeps the network and the Feature");
     // ABI27: moving the compare split is composite-time only; it never rebuilds.
     need(n.handle_embedded("SetNrSettings",{{"tone",.5},{"structure",1},{"compare_split",true},{"compare_split_position",.2}},12)["ok"]==true,"Split position change while ON");
     action=n.take_nr_mode_request(12);need(action&&action->at("settings")["compare_split_position"]==.2f&&deliver(*action)&&rebuilds==2&&bridge.requested_settings.compare_split_position==.2f,
          "A split position change reaches the bridge and keeps the Feature");
     need(n.handle_embedded("SetNrMode",{{"mode","off"}},13)["ok"]==true,"OFF");
     action=n.take_nr_mode_request(14);need(action&&action->at("mode")=="off"&&deliver(*action)&&rebuilds==2,"OFF never rebuilds");
     need(n.handle_embedded("SetNrMode",{{"mode","on"}},15)["ok"]==true,"ON again");
     action=n.take_nr_mode_request(16);need(action&&deliver(*action)&&rebuilds==2&&acknowledged==6,"ON again with the same network reuses the Feature");}
    std::cout<<"PASS embedded ownership, 5000 staged edits, OFF staging, request/read distinction, explicit takeover/release, lease expiry, network switch = one rebuild, split position never rebuilds\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
