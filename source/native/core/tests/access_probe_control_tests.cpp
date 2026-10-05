// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include <iostream>
namespace {
void need(bool b,const char* m){if(!b)throw std::runtime_error(m);}
lab::json req(lab::Controller& c,const char* method,lab::json params,const char* client="test"){
    const auto s=c.status();return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id",client},
        {"session_id",s.at("session_id")},{"expected_revision",s.at("revision")},{"method",method},{"params",params}};
}
auto confirmed(){return lab::json{{"scene_confirmed",true},{"fg_disabled_confirmed",true}};}
}
int main(){try{
    lab::Controller c;c.enable_nr_preparation(true);c.enable_embedded_control();
    unsigned starts=0,cancels=0;bool consumed=false;
    c.enable_access_inspection([&](bool cancel,std::uint64_t){if(cancel){++cancels;return true;}if(consumed)return false;consumed=true;++starts;return true;});
    need(c.status()["capabilities"]["nr_access_inspection"]==true,"Explicit capability");
    need(c.handle(req(c,"InspectNrAccess",confirmed()),1)["error"]["code"]=="takeover_required","Explicit ownership");
    need(c.handle(req(c,"TakeControl",lab::json::object()),2)["ok"]==true,"Take control");
    for(const auto* method:{"PrepareNr","CapturePair","CaptureDisplayPair"})
        need(c.handle(req(c,method,lab::json::object()),3)["ok"]==false,"No preparation or captures in diagnostic process");
    need(!c.take_nr_preparation(3),"No backend preparation job");
    for(const auto* mode:{"on","compute-only"})need(c.handle(req(c,"SetNrMode",{{"mode",mode}}),3)["ok"]==false,"No NR computation");
    auto bad=confirmed();bad["scene_confirmed"]=1;
    need(c.handle(req(c,"InspectNrAccess",bad),3)["ok"]==false&&!starts,"Only boolean scene confirmation");
    bad=confirmed();bad["extra"]=true;
    need(c.handle(req(c,"InspectNrAccess",bad),3)["ok"]==false&&!starts,"No hidden options");
    auto stale=req(c,"InspectNrAccess",confirmed());stale["session_id"]="another-process";
    need(c.handle(stale,3)["ok"]==false&&!starts,"No stale process request");
    auto r=req(c,"InspectNrAccess",confirmed());const auto ack=c.handle(r,4);
    need(ack["ok"]==true&&c.handle(r,5)==ack&&starts==1,"Idempotent request");
    need(c.handle(req(c,"InspectNrAccess",confirmed()),5)["ok"]==false&&starts==1,"No second attempt");
    stale=req(c,"CancelNrAccess",lab::json::object());stale["expected_revision"]=999u;
    need(c.handle(stale,6)["error"]["code"]=="stale_revision"&&!cancels,"Stale cancel refused");
    need(c.handle(req(c,"CancelNrAccess",lab::json::object(),"other"),6)["error"]["code"]=="control_busy","Other writer refused");
    c.frame_boundary(0,11007);need(cancels==1&&c.status()["control_owner"]=="@embedded","Disconnect lease cancels");
    need(c.handle(req(c,"TakeControl",lab::json::object()),11008)["ok"]==true,"Take control after expiry");
    need(c.handle(req(c,"CancelNrAccess",lab::json::object()),11009)["ok"]==true&&cancels==2,"Explicit cancel");
    need(c.handle(req(c,"ReleaseControl",lab::json::object()),11010)["ok"]==true&&cancels==3,"Release cancels");
    c.publish_host({{"state","stopped"}});need(cancels==4,"Backend stop cancels");
    need(c.status()["capabilities"]["nr_access_inspection"]==false&&
        c.handle(req(c,"InspectNrAccess",confirmed()),12000)["error"]["code"]=="unsupported","Stopped backend revokes capability");
    lab::Controller absent;need(absent.handle(req(absent,"InspectNrAccess",confirmed()),1)["error"]["code"]=="unsupported","Absent backend not advertised");
    lab::Controller exclusive;exclusive.enable_nr_preparation(true);exclusive.enable_access_inspection([](bool,std::uint64_t){return false;});
    bool refused=false;try{exclusive.enable_post_inspection([](bool,std::uint64_t){return true;});}catch(const std::logic_error&){refused=true;}
    need(refused,"Never combine heavy post and access probes");
    std::cout<<"PASS access protocol: explicit, NR-disabled, bounded, deduplicated, lease/cancel/stop; no GPU\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
