// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Backend reports published into Controller are clipped to their byte bound instead of throwing into the
// publishing host worker. State/identity keys are protected; identity refusals
// (unknown live profile, capability-enabling adapter snapshots) still throw.
#include "lab_control.hpp"
#include <iostream>
namespace {
unsigned checks=0;
void need(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
std::string filler(std::size_t n){return std::string(n,'x');}
lab::json names(std::initializer_list<const char*> keys){lab::json a=lab::json::array();for(const char* k:keys)a.push_back(k);return a;}
void frame_control(lab::Controller& c,const char* profile){
    c.enable_nr_frame_control("game-rr-experimental-nr",true,true);
    c.publish_nr_runtime({{"profile",profile},{"state","ready"}});
    // Only an integrated host (enable_nr_preparation(true)) queues an initial OFF intent.
    if(auto off=c.take_nr_mode_request(1))c.acknowledge_nr_mode(off->at("revision"),1,true,false,false);
}
}
int main(){try{
    // Diagnostic probes: oversize reports are clipped largest-key-first, the state key survives, nothing throws.
    {lab::Controller c;
     c.publish_binding_probe({{"state","observed"},{"frame",42u},{"detail",filler(3000)}});
     const auto probe=c.status()["binding_restore_probe"];
     need(probe["state"]=="observed"&&probe["frame"]==42u&&probe["clipped"]==names({"detail"})&&probe.dump().size()<=2048,"Binding probe clipped; state and frame kept");
     c.publish_preparation_probe({{"state","done"},{"rows",filler(4000)}});
     need(c.status()["input_preparation_probe"]["state"]=="done"&&c.status()["input_preparation_probe"]["clipped"]==names({"rows"}),"Preparation probe clipped");
     c.publish_post_inspection({{"state","running"},{"a",filler(5000)},{"b",filler(6000)}});
     const auto post=c.status()["post_inspection"];
     need(post["state"]=="running"&&post["clipped"]==names({"b"})&&post.contains("a")&&post.dump().size()<=8192,"Largest key dropped first; the smaller one that fits is kept");
     c.publish_access_inspection({{"state","idle"},{"blob",filler(40000)}});
     need(c.status()["nr_access_inspection"]["state"]=="idle"&&c.status()["nr_access_inspection"]["clipped"]==names({"blob"}),"Access inspection clipped");
     c.publish_binding_probe(lab::json::array({1,2,3}));
     need(c.status()["binding_restore_probe"]["state"]=="failed"&&c.status()["binding_restore_probe"]["error"]=="binding-probe-report-shape","Malformed report becomes a bounded failure record");
     c.publish_post_inspection({{"state",filler(9000)}});
     need(c.status()["post_inspection"]["state"]=="failed"&&c.status()["post_inspection"]["error"]=="post-inspection-report-size","Protected keys alone above the bound become a failure record");
     c.publish_post_inspection({{"state","idle"}});
     need(c.status()["post_inspection"]==lab::json{{"state","idle"}},"In-bound report is stored verbatim without a clipped marker");
    }
    // Host: clipping keeps state/error, so a failed host still fails NR closed.
    {lab::Controller c;frame_control(c,"cyberpunk2077-rr-v1");
     c.publish_host({{"backend","standalone-d3d12"},{"state","ready"},{"modules",filler(9000)}});
     need(c.status()["host"]["state"]=="ready"&&c.status()["host"]["backend"]=="standalone-d3d12"&&c.status()["host"]["clipped"]==names({"modules"}),"Host status clipped; backend and state kept");
     need(c.status()["capabilities"]["capture_pair"]==true,"Clipping is not a host failure");
     c.publish_host({{"state","failed"},{"error","test failure"},{"trace",filler(9000)}});
     need(c.status()["host"]["state"]=="failed"&&c.status()["host"]["error"]=="test failure"&&c.status()["nr_frame_control"]["state"]=="failed"&&c.status()["capabilities"]["capture_pair"]==false,"Clipped failed host still fails closed with its error");}
    // Live NR runtime: size clipped with profile/state protected; unknown profile is still an identity refusal; malformed fails closed.
    {lab::Controller c;frame_control(c,"synthetic-standalone-fixture");
     c.publish_nr_runtime({{"profile","synthetic-standalone-fixture"},{"state","ready"},{"evaluates",3u},{"diagnostics",filler(9000)}});
     const auto runtime=c.status()["nr_runtime"];
     need(runtime["state"]=="ready"&&runtime["profile"]=="synthetic-standalone-fixture"&&runtime["evaluates"]==3u&&runtime["clipped"]==names({"diagnostics"}),"Runtime clipped; identity, state and small keys kept");
     need(c.status()["nr_frame_control"]["state"]!="failed"&&c.status()["nr_lifecycle"].is_null(),"Clipping is not a backend failure");
     bool refused=false;try{c.publish_nr_runtime({{"profile","unknown-profile"},{"state","ready"}});}catch(const std::logic_error&){refused=true;}
     need(refused&&c.status()["nr_runtime"]["profile"]=="synthetic-standalone-fixture","Unknown profile is an identity refusal and does not mutate the last good status");
     c.publish_nr_runtime(lab::json::array());
     need(c.status()["nr_runtime"]["state"]=="failed"&&c.status()["nr_runtime"]["error"]=="nr-runtime-report-shape"&&c.status()["nr_frame_control"]["state"]=="failed"&&c.status()["capabilities"]["nr_control"]==false,"Malformed runtime report fails NR closed instead of throwing");}
    // Frame pair / display pair mailboxes: revision/files are protected, so ordering rules keep working on clipped receipts.
    {lab::Controller c;c.enable_nr_preparation(true);c.enable_embedded_control();c.enable_display_capture();frame_control(c,"cyberpunk2077-rr-v1");
     need(c.handle_embedded("CaptureDisplayPair",{{"scene_confirmed",true},{"fg_disabled_confirmed",true}},2)["ok"]==true,"Display request accepted");
     const auto revision=c.status()["display_pair"]["revision"];
     c.publish_display_pair({{"state","complete"},{"revision",revision},{"files",6u},{"paths",filler(5000)}});
     const auto display=c.status()["display_pair"];
     need(display["state"]=="complete"&&display["revision"]==revision&&display["files"]==6u&&display["clipped"]==names({"paths"}),"Display receipt clipped with revision and files kept");
     c.publish_display_pair({{"state","pending_gpu"},{"revision",revision},{"note",filler(5000)}});
     need(c.status()["display_pair"]["state"]=="complete","Clipped late active receipt still cannot resurrect a completed request");
     c.publish_pair({{"state","idle"},{"revision",0u},{"junk",filler(5000)}});
     need(c.status()["frame_pair"]["state"]=="idle"&&c.status()["frame_pair"]["clipped"]==names({"junk"}),"Frame pair receipt clipped");}
    // Observer: the 48 KiB bound is clipped and the whole status stays under the 64 KiB pipe limit.
    {lab::Controller c;c.publish_observer({{"origin","d3d12-harness"},{"written",7u},{"queues",filler(60*1024)}});
     need(c.status()["observer"]["written"]==7u&&c.status()["origin"]=="d3d12-harness"&&c.status()["observer"]["clipped"]==names({"queues"})&&c.status().dump().size()<64*1024,"Observer clipped under the pipe limit");}
    // Admission diagnostics: size clipped; the capability-safety shape check is still a refusal.
    {lab::Controller c;
     c.publish_nr_adapter({{"mode","admission-only"},{"game_control_available",false},{"render_admission",false},{"nr_executed",false},{"enabled",true},{"trace",filler(9000)}});
     need(c.status()["nr_adapter"]["mode"]=="admission-only"&&c.status()["nr_adapter"]["enabled"]==true&&c.status()["nr_adapter"]["clipped"]==names({"trace"})&&c.status()["capabilities"]["nr_adapter_diagnostics"]==true,"Adapter diagnostics clipped with capability keys kept");
     bool refused=false;try{c.publish_nr_adapter({{"mode","admission-only"},{"game_control_available",true},{"render_admission",false},{"nr_executed",false}});}catch(const std::runtime_error&){refused=true;}
     need(refused,"Capability-enabling adapter snapshot is still refused");}
    std::cout<<"PASS "<<checks<<" publish bound checks; oversize/malformed backend reports clip to bounded records instead of throwing into the host worker\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
