// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_workbench_adapter.hpp"
#include "lab_streamline.hpp"
#include "lab_pipe.hpp"
#include <iostream>
namespace {
unsigned checks=0,originals=0,virtuals=0;
bool change_options_in_evaluate=false;
bool emit_inner=false;unsigned fixture_frame=0;
struct BoundaryReceiver:lab::SlAdmissionSink,lab::IResearchObserver {
    bool armed=false;unsigned count=0,renders=0;std::array<lab::diagnostic::BindingBoundaryEvent,8> events{};
    unsigned game_entries=0,game_exits=0;std::uint64_t game_call=0;bool game_order_error=false;
    void game_binding_enter(const lab::slboundary::Resolution& r,void*)noexcept override{
        if(game_call)game_order_error=true;game_call=r.binding.call;++game_entries;
    }
    void game_binding_exit(std::uint64_t call)noexcept override{
        if(game_call&&game_call!=call)game_order_error=true;game_call=0;++game_exits;
    }
    bool wants_binding_boundaries()const noexcept override{return armed;}
    void binding_boundary(const lab::diagnostic::BindingBoundaryEvent& e)noexcept override{if(count<events.size())events[count++]=e;}
    void enter(const lab::slboundary::Resolution& r,const lab::rr::Packet&,void*)noexcept override{if(game_call!=r.binding.call)game_order_error=true;++renders;}
    // The read-only call observation the chain-capture tag mirror uses.
    // Only POD is copied out; the Call itself is never retained.
    struct Observed {unsigned entering=0,returned=0,aborted=0;std::array<unsigned,4> apis{};std::uint64_t open=0,last_evaluate=0;
        unsigned last_tag_count=0;std::uint32_t last_tag_type=UINT32_MAX;sl::Feature last_feature=0;bool order_error=false;} observed;
    void observe_call(const lab::slboundary::Call& c,lab::ObservedPoint p)noexcept override{
        using lab::slboundary::Api;const auto api=static_cast<unsigned>(c.api);if(api<4&&p==lab::ObservedPoint::entering)++observed.apis[api];
        if(p==lab::ObservedPoint::entering){++observed.entering;if(c.api==Api::evaluate){if(observed.open)observed.order_error=true;observed.open=c.id;}}
        else if(p==lab::ObservedPoint::returned){++observed.returned;
            if(c.api==Api::evaluate){if(observed.open!=c.id)observed.order_error=true;observed.open=0;observed.last_evaluate=c.id;observed.last_feature=c.feature;}
            if(c.api==Api::tags&&c.result==sl::Result::eOk){observed.last_tag_count=c.inputs.tag_count;observed.last_tag_type=c.inputs.tags[0].type;}}
        else ++observed.aborted;
    }
    void boundary_returned(std::uint64_t,bool)noexcept override{}
    void aborted(std::uint64_t)noexcept override{}
    void unavailable()noexcept override{}
};
lab::rr::SetOptions* volatile options_fn=nullptr;
lab::rr::OptionsV3 rr_options;
void need(bool ok,const char* what){++checks;if(!ok)throw std::runtime_error(what);}
struct Token:sl::FrameToken {operator uint32_t()const override{++virtuals;return 0;}} token;
__declspec(noinline) sl::Result get(sl::FrameToken*& out,const uint32_t*){++originals;out=&token;return sl::Result::eOk;}
__declspec(noinline) sl::Result set(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&){++originals;return sl::Result::eOk;}
__declspec(noinline) sl::Result tag(const sl::ViewportHandle&,const sl::ResourceTag*,uint32_t,sl::CommandBuffer*){++originals;return sl::Result::eOk;}
__declspec(noinline) sl::Result options_set(const sl::ViewportHandle&,const lab::rr::OptionsV3&){return sl::Result::eOk;}
__declspec(noinline) sl::Result eval(sl::Feature,const sl::FrameToken&,const sl::BaseStructure**,uint32_t,sl::CommandBuffer* cmd){
    ++originals;if(change_options_in_evaluate){rr_options.pre_exposure=.75f;options_fn(sl::ViewportHandle(0u),rr_options);}
    if(emit_inner){lab::slboundary::InnerCall e;e.command=cmd;e.frame=fixture_frame;e.viewport=0;e.metadata_valid=true;e.result=sl::Result::eOk;
        lab::notify_streamline_inner(e);e.phase=lab::slboundary::InnerCall::Phase::end;lab::notify_streamline_inner(e);}return sl::Result::eOk;}
}
int main(){bool installed=false;try {
    lab::WorkbenchAdapterHost disabled(false);disabled.poll();need(disabled.snapshot()["host_state"]=="disabled","Default does not attach");
    lab::WorkbenchAdapterHost absent(true);absent.poll();need(absent.snapshot()["host_state"]=="awaiting-loaded-interposer","No SL loaded by discovery");
    auto* options_watch=new lab::rr::Watch;lab::json options_hooks;
    need(options_watch->install(reinterpret_cast<void*>(&options_set),options_hooks),"RR option hook installed");
    options_fn=&options_set;rr_options.mode=1;rr_options.width=1280;rr_options.height=720;
    rr_options.pre_exposure=.25f;rr_options.exposure_scale=2;rr_options.hdr=sl::eTrue;
    rr_options.flip_x=rr_options.flip_y=rr_options.alpha=sl::eFalse;
    BoundaryReceiver receiver;lab::WorkbenchAdapter adapter(options_watch,false,&receiver,&receiver);lab::json hooks;
    std::array<void*,4> targets{reinterpret_cast<void*>(get),reinterpret_cast<void*>(eval),reinterpret_cast<void*>(set),reinterpret_cast<void*>(tag)};
    need(lab::install_streamline_boundary(targets,&adapter,hooks),"Real lightweight hook install");installed=true;
    auto volatile get_fn=reinterpret_cast<PFun_slGetNewFrameToken*>(targets[0]);
    auto volatile eval_fn=reinterpret_cast<PFun_slEvaluateFeature*>(targets[1]);
    auto volatile set_fn=reinterpret_cast<PFun_slSetConstants*>(targets[2]);
    auto volatile tag_fn=reinterpret_cast<PFun_slSetTag*>(targets[3]);
    sl::ViewportHandle viewport(0u);sl::Constants constants;constants.mvecScale={-.25f,.125f};constants.jitterOffset={.25f,-.125f};
    sl::Extent color{0,0,1280,720},guide{0,0,640,360};
    std::array<sl::Resource,3> resources{sl::Resource(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x1000),8),
        sl::Resource(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x2000),96),sl::Resource(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x3000),192)};
    std::array<sl::ResourceTag,3> tags{sl::ResourceTag(&resources[0],sl::kBufferTypeScalingOutputColor,sl::eValidUntilPresent,&color),
        sl::ResourceTag(&resources[1],sl::kBufferTypeDepth,sl::eValidUntilEvaluate,&guide),sl::ResourceTag(&resources[2],sl::kBufferTypeMotionVectors,sl::eValidUntilEvaluate,&guide)};
    auto frame=[&](uint32_t index){fixture_frame=index;sl::FrameToken* out=nullptr;const sl::BaseStructure* inputs[]{&viewport};
        need(get_fn(out,&index)==sl::Result::eOk&&out==&token,"Original token result");
        need(set_fn(constants,*out,viewport)==sl::Result::eOk,"Original constants result");
        need(tag_fn(viewport,tags.data(),3,reinterpret_cast<void*>(0x4000))==sl::Result::eOk,"Original tag result");
        need(eval_fn(sl::kFeatureDLSS_RR,*out,inputs,1,reinterpret_cast<void*>(0x4000))==sl::Result::eOk,"Original Evaluate result");};
    frame(0);need(adapter.snapshot()["current_metadata_candidate"]==false,"Unknown Present is not ready");
    need(adapter.snapshot()["rr_options_at_evaluate"]["values_observed"]==false,"No options invented");
    options_fn(sl::ViewportHandle(0u),rr_options);
    need(!adapter.snapshot()["latest"].contains("declared_widths")&&!adapter.snapshot()["latest"].contains("frame_index"),"Unknown metadata not padded with zero values");
    need(receiver.count==0,"No unrequested binding snapshots at startup");
    receiver.armed=true;emit_inner=true;
    adapter.present(1);frame(1);auto s=adapter.snapshot();need(s["current_metadata_candidate"]==true,"Fresh metadata reaches host");
    need(receiver.count==4&&receiver.renders==0,"Four metadata-only callbacks do not admit rendering");
    need(receiver.game_entries==2&&receiver.game_exits==2&&!receiver.game_call&&!receiver.game_order_error,"Game checkpoint paired across every outer call, even metadata-only returns");
    for(unsigned i=0;i<4;++i){const auto& e=receiver.events[i];need(unsigned(e.point)==i&&e.valid&&e.frame==1&&e.viewport==0&&
        e.call==receiver.events[0].call&&e.command==0x4000&&e.resources[0]==0x1000,"Same-call boundary metadata and order retained");}
    receiver.armed=false;emit_inner=false;
    {const auto& o=receiver.observed;
     need(o.entering==8&&o.returned==8&&o.aborted==0&&!o.order_error&&o.open==0,"Every call is observed once at entry and once at return; Evaluate entry/return paired");
     need(o.apis==std::array<unsigned,4>{2,2,2,2},"Token, constants, tags and Evaluate all reach the read-only observer");
     need(o.last_tag_count==3&&o.last_tag_type==sl::kBufferTypeScalingOutputColor&&o.last_feature==sl::kFeatureDLSS_RR,"Observer receives the decoded tag POD and the Evaluate feature");}
    need(s["latest"]["frame_index"]==1&&s["latest"]["declared_widths"]==lab::json::array({1280,640,640}),"Bounded dimensions/identity copied");
    need(s["latest"]["mvec_scale"]==lab::json::array({-.25f,.125f})&&s["latest"]["tag_calls"]==lab::json::array({7,7,7}),"Actual values/source calls");
    need(s["render_admission"]==false&&s["nr_executed"]==false&&s["blockers"].size()==4,"Metadata never grants rendering");
    need(s["rr_options_at_evaluate"]["pre_exposure"]==.25&&s["rr_options_at_evaluate"]["color_buffers_hdr"]==true,"Actual options bound to Evaluate");
    need(!s["latest"].contains("command")&&!s["latest"].contains("native")&&!s["latest"].contains("token"),"No asynchronous borrowed pointers");
    lab::Controller controller;lab::PipeServer server(controller);server.start();
    auto publish=[&]{auto v=adapter.snapshot();v["enabled"]=true;controller.publish_nr_adapter(v);return v;};
    const auto published=publish();const auto deadline=GetTickCount64()+2000;while(!server.ready()&&GetTickCount64()<deadline)Sleep(1);
    need(server.ready(),"Pipe ready");lab::json request={{"protocol","1.0"},{"request_id","read"},{"client_id","adapter-fixture"},{"method","GetStatus"}};
    auto reply=lab::pipe_request(GetCurrentProcessId(),request);need(reply["ok"]==true&&reply["status"]["nr_adapter"]==published,"Exact admission through real named pipe");
    need(reply["status"]["capabilities"]["nr_control"]==false&&reply["status"]["capabilities"]["nr_adapter_diagnostics"]==true,"Diagnostics not NR capability");
    request["method"]="SetNrMode";request["request_id"]="write";request["params"]={{"mode","on"}};
    need(lab::pipe_request(GetCurrentProcessId(),request)["error"]["code"]=="unsupported","Cannot enable game NR from candidate metadata");
    for(const auto* key:{"render_admission","game_control_available","nr_executed"}){auto bad=published;bad[key]=true;bool caught=false;
        try{controller.publish_nr_adapter(bad);}catch(const std::runtime_error&){caught=true;}need(caught,"Unsupported publication rejected");}
    // Oversize diagnostics are clipped to the bound (the publishing worker is never thrown into);
    // the capability-shape checks above stay refusals. Admission keys survive the clip.
    auto oversized=published;oversized["padding"]=std::string(8192,'x');controller.publish_nr_adapter(oversized);
    const auto clipped=controller.status()["nr_adapter"];
    need(clipped["clipped"]==lab::json::array({"padding"})&&clipped.dump().size()<=8*1024,"Status size bound by clipping");
    need(clipped["mode"]=="admission-only"&&clipped["render_admission"]==false&&clipped["game_control_available"]==false&&clipped["nr_executed"]==false,"Admission keys survive clipping");
    controller.publish_nr_adapter(published);
    adapter.present(1);need(adapter.snapshot()["current_metadata_candidate"]==false,"Present invalidates published candidate");
    frame(2);need(adapter.snapshot()["current_metadata_candidate"]==true,"Fresh frame restores metadata only");
    need(adapter.snapshot()["rr_options_at_evaluate"]["pre_exposure"]==.25,"Persistent accepted options survive Present");
    // A game that upscales without ray reconstruction must be recognizable
    // instead of waiting forever. The SR Evaluate is recorded from the public
    // boundary only: it must not admit, render, or disturb the RR record.
    need(adapter.snapshot()["sr_evaluate"].is_null(),"No SR observation invented before an SR call");
    {const auto before=adapter.snapshot();
     auto upscale=[&](uint32_t index){sl::FrameToken* out=nullptr;const sl::BaseStructure* inputs[]{&viewport};
        need(get_fn(out,&index)==sl::Result::eOk,"SR token result");
        need(tag_fn(viewport,tags.data(),3,reinterpret_cast<void*>(0x4000))==sl::Result::eOk,"SR tag result");
        need(eval_fn(sl::kFeatureDLSS,*out,inputs,1,reinterpret_cast<void*>(0x4000))==sl::Result::eOk,"SR Evaluate result");};
     upscale(9);const auto after=adapter.snapshot();const auto sr=after["sr_evaluate"];
     need(sr["feature"]=="DLSS-SR"&&sr["evaluates"]==1&&sr["succeeded"]==1&&sr["viewport"]==0,"SR Evaluate observed with its viewport");
     const auto last=sr["last_tag_call"];
     need(last["tags"].size()==3&&last["tags"][0][1]==1280&&last["tags"][0][2]==720&&last["tags"][1][1]==640,"Most recent tag extents recorded beside the SR call");
     need(last["same_thread"]==true&&last["scope"].get<std::string>().find("NOT a same-call")!=std::string::npos,"Last tag call is never presented as a frozen same-call binding");
     need(sr["constants_present"]==false&&sr["inline_tag_count"]==0&&sr["command_present"]==true,"SR observation reports only what the call carried");
     need(after["render_admission"]==false&&after["nr_executed"]==false&&receiver.renders==0,"SR observation never admits rendering");
     need(after["latest"]==before["latest"]&&after["metadata_candidates"]==before["metadata_candidates"],"SR call leaves the RR record untouched");
     upscale(10);need(adapter.snapshot()["sr_evaluate"]["evaluates"]==2,"SR observations accumulate");
     need(receiver.observed.last_feature==sl::kFeatureDLSS&&!receiver.observed.order_error,"A non-target SR Evaluate still reaches the read-only observer");}
    change_options_in_evaluate=true;frame(20);change_options_in_evaluate=false;
    need(adapter.snapshot()["rr_options_at_evaluate"]["status"]=="options-changed-during-evaluate"&&
        !adapter.snapshot()["rr_options_at_evaluate"].contains("pre_exposure"),"Mid Evaluate update not paired");
    tags[1].resource=nullptr;frame(3);need(adapter.snapshot()["current_metadata_candidate"]==false,"Revocation visible");tags[1].resource=&resources[1];
    adapter.present(2);need(adapter.snapshot()["multiple_swapchains"]==false,"A single swapchain change is a recreation, not ambiguity");
    adapter.present(1);frame(4);need(adapter.snapshot()["multiple_swapchains"]==true&&adapter.snapshot()["current_metadata_candidate"]==false,"Alternating swapchains make frame association ambiguous and reject admission");
    adapter.stop();const auto stopped=adapter.snapshot();frame(5);adapter.present(1);
    need(receiver.game_entries<=receiver.game_exits&&!receiver.game_call&&!receiver.game_order_error,"Rejected and stopped returns leave no active checkpoint");
    need(adapter.snapshot()==stopped&&stopped["stopped"]==true,"Late returns do not republish after detach");
    publish();need(controller.status()["nr_adapter"]["stopped"]==true,"Stop visible in control status");
    server.stop();lab::uninstall_streamline_observer();installed=false;
    options_watch->stop();need(originals==34&&!virtuals,"Original passthrough and no private token calls");
    std::cout<<lab::json{{"passed",true},{"checks",checks},{"original_calls",originals},{"real_pipe",true},{"actual_sl_loaded",false},
        {"nr_executed",false},{"event_files",0},{"hooks",hooks},{"final_admission",stopped}}.dump()<<'\n';return 0;
}catch(const std::exception& e){if(installed)lab::uninstall_streamline_observer();std::cerr<<e.what()<<'\n';return 1;}}
