// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_binding_boundary_json.hpp"
#include "lab_control.hpp"
#include "lab_pipe.hpp"
#include <iostream>
using namespace lab::diagnostic;
namespace {
unsigned checks=0;
void need(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
BindingBoundaryEvent event(unsigned point=0,std::uint64_t call=1){BindingBoundaryEvent e;e.point=BindingPoint(point);e.valid=1;
    e.call=call;e.frame=call;e.command=100;e.thread=7;e.viewport=1;return e;}
BindingSnapshot snapshot(){BindingSnapshot b;b.tracked=1;b.generation=55;return b;}
lab::json request(lab::Controller& c,const char* method,lab::json params=lab::json::object()){
    const auto s=c.status();return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id","boundary-test"},
        {"session_id",s["session_id"]},{"expected_revision",s["revision"]},{"method",method},{"params",params}};}
}
int main(){try{
    BindingBoundaryAudit a;auto b=snapshot();a.append(event(),b,1);need(a.report().count==0,"No startup sampling");
    need(a.arm(100)&&!a.arm(101),"Explicit one-shot arm");
    for(unsigned call=1;call<=4;++call)for(unsigned p=0;p<4;++p)a.append(event(p,call),b,101+call);
    need(a.report().phase==BindingAuditPhase::complete&&a.report().complete_calls==4,"Four complete calls stop automatically");
    a.append(event(0,5),b,200);need(a.report().count==4&&!a.arm(300),"No fifth call or automatic retry");
    need(sizeof(BindingBoundaryReport)<32768,"Fixed metadata storage under 32 KiB");
    // Exercise worst-case JSON bound rather than assuming ordinary few-slot use.
    auto full=a.report();full.started_ms=full.deadline_ms=full.callback_losses=UINT64_MAX;
    for(auto& call:full.calls)for(auto& p:call.points){
        auto& s=p.bindings;auto& e=p.event;
        e.call=e.frame=e.command=UINT64_MAX;e.thread=e.viewport=UINT_MAX;e.resources.fill(UINT64_MAX);e.declared_states.fill(UINT_MAX);
        s.generation=s.pipeline=s.raytracing_pipeline=s.heap_permutations=UINT64_MAX;s.heaps.fill(UINT64_MAX);s.heap_count=2;
        std::fill_n(s.reason,95,'\1');
        for(auto* bank:{&s.compute,&s.graphics}){
            bank->root=bank->tables=bank->descriptors=bank->constants=bank->constant_hash=UINT64_MAX;bank->table_addresses.fill(UINT64_MAX);}}
    auto j=binding_boundary_json(full);need(j["raw_files"]==0&&j["visual_fix_verified"]==false,"Worst-case bounded report keeps evidence limits");
    for(const auto& row:j["rows"])need(row.dump().size()<24576,"Worst integers and escaped strings fit one detail page");
    auto changed_report=a.report();auto& entry=changed_report.calls[0].points[0].bindings;
    entry.heap_count=2;entry.heaps={100,200};entry.compute.tables=8;entry.compute.table_addresses[3]=321;
    auto& next=changed_report.calls[0].points[1].bindings;next=entry;next.heaps={200,100};
    auto& end=changed_report.calls[0].points[2].bindings;end=next;end.heaps={300,100};end.compute.tables=0;end.compute.table_addresses={};
    auto changes=binding_boundary_json(changed_report)["rows"][0]["adjacent_changes"];
    need(changes[0]["heap_order_only_changed"]==true&&changes[0]["heap_set_changed"]==false&&changes[0]["compute"]["lost_table_mask"]==0,"Heap order difference not misreported as new heap");
    need(changes[1]["heap_set_changed"]==true&&changes[1]["compute"]["lost_table_mask"]==8,"Missing table attributed to exact adjacent boundary, not declared a root cause");
    for(unsigned changed=0;changed<7;++changed){BindingBoundaryAudit bad;bad.arm(100);bad.append(event(),b,101);auto e=event(1);auto s=b;
        switch(changed){case 0:++e.command;break;case 1:++e.thread;break;case 2:++e.frame;break;case 3:++e.viewport;break;
        case 4:++s.generation;break;case 5:e.valid=0;break;case 6:s.tracked=0;break;}
        bad.append(e,s,102);bad.append(event(2),b,103);bad.append(event(3),b,104);
        need(bad.report().complete_calls==0&&bad.report().calls[0].invalid,"Mismatch cannot form complete evidence");}
    BindingBoundaryAudit duplicate;duplicate.arm(1);duplicate.append(event(),b,2);duplicate.append(event(),b,3);
    need(duplicate.report().phase==BindingAuditPhase::failed,"Duplicate entry not overwritten");
    BindingBoundaryAudit missing;missing.arm(1);missing.append(event(),b,2);missing.append(event(2),b,3);missing.append(event(3),b,4);
    need(missing.report().complete_calls==0,"Missing callback cannot be padded");
    BindingBoundaryAudit partial;partial.arm(1);partial.append(event(),b,2);partial.expire(10001);
    need(partial.report().phase==BindingAuditPhase::expired&&partial.report().complete_calls==0,"Hard deadline includes partial call");
    BindingBoundaryAudit overlap;overlap.arm(1);overlap.append(event(),b,2);overlap.append(event(0,2),b,3);
    need(overlap.report().phase==BindingAuditPhase::failed,"Overlapping calls stop without joining samples");
    BindingBoundaryAudit tail;tail.arm(1);for(unsigned p=1;p<4;++p)tail.append(event(p,99),b,2);
    need(tail.report().phase==BindingAuditPhase::armed&&tail.report().count==0&&tail.report().pre_entry_callbacks==3,"Mid-call arm waits for next complete entry");
    for(unsigned call=1;call<=4;++call)for(unsigned p=0;p<4;++p)tail.append(event(p,call),b,3);
    need(tail.report().complete_calls==4&&tail.report().callback_losses==0,"Skipped pre-entry tail does not poison selected calls");
    BindingBoundaryAudit orphan;orphan.arm(1);orphan.append(event(0),b,2);orphan.append(event(3,99),b,3);need(orphan.report().phase==BindingAuditPhase::failed,"Orphan return inside selected window rejected");
    BindingBoundaryAudit cancelled;cancelled.arm(1);cancelled.cancel();cancelled.append(event(),b,2);
    need(cancelled.report().count==0&&!cancelled.arm(3),"Cancellation sticky");
    BindingBoundaryAudit clock;clock.arm(100);clock.append(event(),b,99);need(clock.report().phase==BindingAuditPhase::failed,"Clock reversal fails");
    BindingBoundaryAudit overflow;need(!overflow.arm(UINT64_MAX),"Deadline arithmetic bounded");
    BindingBoundaryAudit abi;abi.arm(1);auto invalid=event();invalid.abi=2;abi.append(invalid,b,2);need(abi.report().phase==BindingAuditPhase::failed,"Unsupported ABI rejected");
    BindingBoundaryAudit late_loss=a;late_loss.loss();need(late_loss.report().phase==BindingAuditPhase::failed,"Late reported callback loss invalidates completed window");
    lab::Controller c;c.enable_nr_preparation(true);c.enable_embedded_control();unsigned arms=0,cancels=0;
    c.enable_binding_boundary_inspection([&](bool cancel){if(cancel)++cancels;else ++arms;return true;});
    const lab::json scene={{"scene_confirmed",true},{"fg_disabled_confirmed",true}};
    need(c.handle_embedded("InspectBindingBoundaries",scene,1)["error"]["code"]=="unsupported","No embedded diagnostic trigger");
    need(c.handle(request(c,"TakeControl"),2)["ok"]==true,"Explicit takeover");
    need(c.handle(request(c,"InspectBindingBoundaries",scene),3)["error"]["code"]=="nr_not_off"&&arms==0,"Unprepared state cannot arm");
    c.enable_nr_frame_control("game-rr-experimental-nr",true,true);
    c.publish_nr_runtime({{"profile","007-first-light-rr-v1"},{"state","ready"},{"pending_gpu",false},{"evaluates",0}});
    auto off=c.take_nr_mode_request(4);need(bool(off),"Initial OFF acknowledgement pending");c.acknowledge_nr_mode(off->at("revision"),1,true,false,false);
    need(c.handle(request(c,"InspectBindingBoundaries"),5)["error"]["code"]=="scene_required","Fresh scene confirmation required");
    const auto start=request(c,"InspectBindingBoundaries",scene),accepted=c.handle(start,6);
    need(accepted["ok"]==true&&arms==1,"Bounded diagnostic accepted");
    need(c.handle(start,7)==accepted&&arms==1,"Duplicate request id does not repeat");
    need(c.handle(request(c,"InspectBindingBoundaries",scene),8)["error"]["code"]=="consumed","New id cannot repeat inspection");
    need(c.handle(request(c,"SetNrMode",{{"mode","on"}}),9)["error"]["code"]=="diagnostic_session","Readonly inspection cannot become NR experiment");
    c.publish_binding_boundaries(j);
    need(!c.status()["binding_boundaries"].contains("rows")&&c.status()["binding_boundaries"]["row_count"]==4,"No rows in normal status");
    auto detail=request(c,"GetBindingBoundaries",{{"call_index",0}});detail["request_id"]=std::string(128,'\1');
    auto page=c.handle(detail,9);need(page["ok"]==true&&page["row"]==j["rows"][0]&&page.dump().size()<32768,"Full escaped request plus worst detail fits pipe");
    detail["params"]["call_index"]=4;need(c.handle(detail,9)["error"]["code"]=="bad_request","Out of range detail refused");
    detail["params"]["call_index"]=0;detail["session_id"]="old";need(c.handle(detail,9)["error"]["code"]=="stale_session","Stale detail session refused");
    c.frame_boundary(2,11010);need(cancels==1&&c.status()["control_owner"]=="@embedded","Disconnect cancels and returns control");
    // Exercise actual local transport without granting a writer lease.
    lab::PipeServer server(c);server.start();const auto until=GetTickCount64()+3000;
    while(!server.ready()&&GetTickCount64()<until)Sleep(5);
    need(server.ready(),"Bounded pipe startup");
    for(unsigned i=0;i<4;++i){auto r=request(c,"GetBindingBoundaries",{{"call_index",i}});
        const auto reply=lab::pipe_request(GetCurrentProcessId(),r);
        need(reply["ok"]==true&&reply["row"]==j["rows"][i]&&reply.dump().size()<32768,"Worst detail survives actual pipe without clipping");}
    const auto normal=lab::pipe_request(GetCurrentProcessId(),request(c,"GetStatus"));
    need(normal["ok"]==true&&normal.dump().size()<16384&&c.status()["control_owner"]=="@embedded","Detail reads leave status and ownership usable");server.stop();
    c.publish_binding_boundaries({{"rows",lab::json::array({{{"oversize",std::string(30000,'x')}}})},{"state","complete"}});
    need(c.status()["binding_boundaries"]["state"]=="failed"&&c.status()["binding_boundaries"]["row_count"]==0,"Oversized diagnostic fails explicitly without breaking control");
    std::cout<<"PASS "<<checks<<" boundary policy/protocol checks; fixed bytes="<<sizeof(BindingBoundaryReport)<<"; worst JSON="<<j.dump().size()<<"; no GPU or game\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
