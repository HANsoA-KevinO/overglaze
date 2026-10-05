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
auto confirmed(){return lab::json{{"scene_confirmed",true},{"fg_disabled_confirmed",true},{"sdr_confirmed",true}};}
}
int main(){try{
    lab::Controller c;c.enable_nr_preparation(true);c.enable_embedded_control();
    unsigned starts=0,cancels=0;bool consumed=false;
    c.enable_post_inspection([&](bool cancel,std::uint64_t){if(cancel){++cancels;return true;}if(consumed)return false;consumed=true;++starts;return true;});
    need(c.handle(req(c,"InspectPostBindings",confirmed()),1)["error"]["code"]=="takeover_required","Explicit writer ownership");
    need(c.handle(req(c,"TakeControl",lab::json::object()),2)["ok"]==true,"Take control");
    need(c.handle(req(c,"PrepareNr",lab::json::object()),3)["ok"]==false&&!c.take_nr_preparation(3),"No NR preparation in diagnostic process");
    need(c.handle(req(c,"SetNrMode",{{"mode","on"}}),3)["ok"]==false,"No NR ON");
    auto bad=confirmed();bad["scene_confirmed"]=1;
    need(c.handle(req(c,"InspectPostBindings",bad),3)["ok"]==false&&!starts,"Boolean confirmation is not integer");
    auto r=req(c,"InspectPostBindings",confirmed());const auto ack=c.handle(r,4);
    need(ack["ok"]==true&&c.handle(r,5)==ack&&starts==1,"Idempotent request");
    need(c.handle(req(c,"InspectPostBindings",confirmed()),5)["ok"]==false&&starts==1,"Second request refused");
    auto stale=req(c,"CancelPostInspection",lab::json::object());stale["expected_revision"]=999u;
    need(c.handle(stale,6)["error"]["code"]=="stale_revision"&&!cancels,"Stale cancel refused");
    need(c.handle(req(c,"CancelPostInspection",lab::json::object(),"other"),6)["error"]["code"]=="control_busy","Other writer refused");
    c.frame_boundary(0,11007);need(cancels==1,"Lease expiry cancels observation");
    need(c.status()["control_owner"]=="@embedded","Lease returns embedded control");
    c.publish_host({{"state","stopped"}});
    need(!c.status()["capabilities"]["post_binding_inspection"].get<bool>()&&
        c.handle(req(c,"InspectPostBindings",confirmed()),12000)["error"]["code"]=="unsupported","Stopped host revokes inspection");
    lab::Controller unsupported;need(unsupported.handle(req(unsupported,"InspectPostBindings",confirmed()),1)["error"]["code"]=="unsupported","Absent backend not advertised");
    std::cout<<"PASS post protocol: OFF-only, consent, dedup, ownership, lease, no retry\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
