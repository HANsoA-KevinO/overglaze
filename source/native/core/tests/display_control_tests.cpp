// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include <iostream>
namespace {
void need(bool x,const char* why){if(!x)throw std::runtime_error(why);}
lab::json confirmed(){return {{"scene_confirmed",true},{"fg_disabled_confirmed",true}};}
lab::json request(lab::Controller& c,const char* method,lab::json params,const char* owner="display-test"){
    const auto s=c.status();return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id",owner},{"session_id",s["session_id"]},
        {"expected_revision",s["revision"]},{"method",method},{"params",params}};
}
lab::json command(lab::Controller& c,const char* method,lab::json params,unsigned now=1){return c.handle(request(c,method,params),now);}
void standby(lab::Controller& c){c.enable_nr_preparation(true);c.enable_display_capture();}
void ready(lab::Controller& c){c.enable_nr_frame_control("game-rr-experimental-nr",true,true);
    c.publish_nr_runtime({{"profile","cyberpunk2077-rr-v1"},{"state","ready"}});
    auto off=c.take_nr_mode_request(3);need(off&&off->at("mode")=="off","Preparing display never requests NR ON");
    c.acknowledge_nr_mode(off->at("revision"),20,true,false,false);
}
}
int main(){try{
    lab::Controller unsupported;need(command(unsupported,"CaptureDisplayPair",confirmed())["error"]["code"]=="unsupported","Not exposed without backend");
    lab::Controller c;standby(c);
    need(!c.take_nr_preparation(1)&&!c.take_display_capture_request(1),"Standby does not collect or load NR");
    for(const auto& p:{lab::json::object(),lab::json{{"scene_confirmed",false},{"fg_disabled_confirmed",true}},lab::json{{"scene_confirmed",true},{"fg_disabled_confirmed",1}}})
        need(command(c,"CaptureDisplayPair",p)["error"]["code"]=="scene_required","Strict manual confirmation types");
    auto req=request(c,"CaptureDisplayPair",confirmed());const auto first=c.handle(req,1);need(first["ok"]==true&&c.handle(req,2)==first,"Idempotent capture");
    need(c.take_nr_preparation(2)&&!c.take_nr_preparation(2)&&!c.take_display_capture_request(2),"Wait for prepare and actual OFF");
    need(command(c,"SetNrMode",{{"mode","on"}},2)["error"]["code"]=="display_capture_active","Reject ON during diagnostic");
    need(command(c,"CaptureDisplayPair",confirmed(),2)["error"]["code"]=="pending","One pending capture");
    auto stale=request(c,"CancelDisplayPair",lab::json::object());stale["expected_revision"]=999u;
    need(c.handle(stale,2)["error"]["code"]=="stale_revision","Reject stale configuration");
    need(c.handle(request(c,"CancelDisplayPair",lab::json::object(),"other"),2)["error"]["code"]=="control_busy","One controller");
    ready(c);auto capture=c.take_display_capture_request(4);need(capture&&!capture->value("cancel",true)&&!c.take_display_capture_request(4),"Exactly once dispatch after OFF");
    const auto rev=capture->at("revision");c.publish_display_pair({{"state","idle"},{"revision",0u}});
    need(c.status()["display_pair"]["state"]=="preparing","Older receipt cannot erase request");
    c.publish_display_pair({{"state","pending_gpu"},{"revision",rev}});
    need(command(c,"CancelDisplayPair",lab::json::object(),5)["ok"]==true,"Cancel consumed action");
    auto cancel=c.take_display_capture_request(6);need(cancel&&cancel->at("cancel")==true,"Cancel forwarded to real worker");
    c.publish_display_pair({{"state","cancelled"},{"revision",rev}});
    need(command(c,"SetNrMode",{{"mode","on"}},7)["ok"]==true,"Normal control resumes after retirement receipt");
    need(command(c,"CaptureDisplayPair",confirmed(),8)["error"]["code"]=="nr_not_off","NR ON not display baseline");
    lab::Controller queued;standby(queued);command(queued,"CaptureDisplayPair",confirmed());command(queued,"CancelDisplayPair",lab::json::object(),2);
    need(!queued.take_display_capture_request(3)&&!queued.take_nr_preparation(3)&&queued.status()["display_pair"]["state"]=="cancelled","Unconsumed cancellation schedules no copies or preparation");
    lab::Controller expired;standby(expired);command(expired,"CaptureDisplayPair",confirmed());
    need(!expired.take_display_capture_request(11002)&&!expired.take_nr_preparation(11002),"Lease expiry cancels unconsumed preparation and capture");
    lab::Controller lost;standby(lost);command(lost,"CaptureDisplayPair",confirmed());lost.take_nr_preparation(2);ready(lost);lost.take_display_capture_request(4);
    auto cancelled=lost.take_display_capture_request(11002);need(cancelled&&cancelled->at("cancel")==true,"Lease expiry cancels consumed capture");
    lab::Controller rebuilt;standby(rebuilt);command(rebuilt,"CaptureDisplayPair",confirmed());rebuilt.take_nr_preparation(2);ready(rebuilt);
    rebuilt.interrupt_nr_for_rebuild(1,25);need(!rebuilt.take_display_capture_request(4),"Rebuild never replays old scene request");
    lab::Controller failed;standby(failed);command(failed,"CaptureDisplayPair",confirmed());failed.publish_host({{"state","failed"},{"error","test"}});
    need(!failed.take_display_capture_request(3)&&command(failed,"CaptureDisplayPair",confirmed(),4)["error"]["code"]=="backend_failed","Host failure blocks further sampling");
    std::cout<<"PASS display protocol: standby, manual consent, OFF, dedup, revision, ownership, cancel, lease, rebuild, fault\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
