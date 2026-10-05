// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_streamline.hpp"
#include "lab_sl_bindings.hpp"
#include <iostream>
namespace {
unsigned checks=0,legacy_calls=0,framed_calls=0,virtuals=0;
bool fail_tags=false,throw_tags=false;
void need(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Token:sl::FrameToken {operator uint32_t()const override{++virtuals;return 0;}} tokens[2];
sl::ViewportHandle viewport{0u};
void* command=reinterpret_cast<void*>(0x9000);
sl::FrameToken* expected_token=nullptr;
std::array<sl::ResourceTag,3> tags;
__declspec(noinline) sl::Result token(sl::FrameToken*& out,const uint32_t* index){out=&tokens[*index%2];return sl::Result::eOk;}
__declspec(noinline) sl::Result evaluate(sl::Feature,const sl::FrameToken&,const sl::BaseStructure**,uint32_t,void*){return sl::Result::eOk;}
__declspec(noinline) sl::Result constants(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&){return sl::Result::eOk;}
__declspec(noinline) sl::Result legacy(const sl::ViewportHandle& v,const sl::ResourceTag* p,uint32_t n,void* c){
    ++legacy_calls;need(&v==&viewport&&p==tags.data()&&n==3&&c==command,"Legacy arguments intact");
    if(throw_tags)throw std::runtime_error("foreign tag failure");
    return fail_tags?sl::Result::eErrorInvalidParameter:sl::Result::eOk;
}
__declspec(noinline) sl::Result framed(const sl::FrameToken& t,const sl::ViewportHandle& v,const sl::ResourceTag* p,uint32_t n,void* c){
    ++framed_calls;need(&t==expected_token&&&v==&viewport&&p==tags.data()&&n==3&&c==command,"Frame tag five arguments intact");
    if(throw_tags)throw std::runtime_error("foreign frame tag failure");
    return fail_tags?sl::Result::eErrorInvalidParameter:sl::Result::eOk;
}
struct Sink:lab::slboundary::Sink {
    lab::slboundary::Bindings bindings;
    lab::slboundary::Resolution last;
    unsigned legacy_events=0,frame_events=0,aborts=0;
    void entering(const lab::slboundary::Call& c) noexcept override{bindings.entering(c);}
    void returned(const lab::slboundary::Call& c) noexcept override{
        auto r=bindings.returned(c);
        if(c.api==lab::slboundary::Api::evaluate)last=r;
        if(c.api==lab::slboundary::Api::tags){if(c.frame_scoped_tags)++frame_events;else ++legacy_events;}
    }
    void aborted(const lab::slboundary::Call& c) noexcept override{++aborts;bindings.aborted(c);}
};
}
int main(){bool installed=false;try{
    Sink sink;lab::json diag;
    std::array<void*,5> api{reinterpret_cast<void*>(&token),reinterpret_cast<void*>(&evaluate),reinterpret_cast<void*>(&constants),
        reinterpret_cast<void*>(&legacy),reinterpret_cast<void*>(&framed)};
    auto bad=api;bad[4]=bad[3];need(!lab::install_streamline_dual_tag_boundary(bad,&sink,diag),"Aliased ABI targets refused before hooks");
    bad=api;bad[4]=nullptr;need(!lab::install_streamline_dual_tag_boundary(bad,&sink,diag),"Missing ABI refused");
    need(!lab::install_streamline_dual_tag_boundary(api,nullptr,diag),"No sink refused");
    need(lab::install_streamline_dual_tag_boundary(api,&sink,diag),"Both tag hooks installed");installed=true;
    need(diag["tagging_observation"]=="both-public-abis"&&lab::streamline_snapshot()["installed_export_count"]==5,"Explicit dual observation status");
    need(!lab::install_streamline_dual_tag_boundary(api,&sink,diag),"Duplicate installation refused");
    auto volatile get=reinterpret_cast<PFun_slGetNewFrameToken*>(api[0]);
    auto volatile eval=reinterpret_cast<PFun_slEvaluateFeature*>(api[1]);
    auto volatile set=reinterpret_cast<PFun_slSetConstants*>(api[2]);
    auto volatile old_tag=reinterpret_cast<PFun_slSetTag*>(api[3]);
    auto volatile frame_tag=reinterpret_cast<lab::SetTagForFrame*>(api[4]);
    sl::Extent full{0,0,1280,720},half{0,0,640,360};
    std::array<sl::Resource,3> resources{sl::Resource(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x1000),8),
        sl::Resource(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x2000),96),sl::Resource(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x3000),192)};
    tags={sl::ResourceTag(&resources[0],sl::kBufferTypeScalingOutputColor,sl::eValidUntilPresent,&full),
        sl::ResourceTag(&resources[1],sl::kBufferTypeDepth,sl::eValidUntilEvaluate,&half),
        sl::ResourceTag(&resources[2],sl::kBufferTypeMotionVectors,sl::eValidUntilEvaluate,&half)};
    sl::Constants values;values.mvecScale={1,1};values.jitterOffset={0,0};
    const sl::BaseStructure* inputs[]{&viewport};
    auto begin=[&](uint32_t index){sink.bindings.present_boundary();sl::FrameToken* out=nullptr;
        need(get(out,&index)==sl::Result::eOk,"Issued public token");expected_token=out;
        need(set(values,*out,viewport)==sl::Result::eOk,"Constants forwarded");return out;};
    auto run=[&](sl::FrameToken* t){need(eval(sl::kFeatureDLSS_RR,*t,inputs,1,command)==sl::Result::eOk,"Evaluate result unchanged");};
    auto* first=begin(1);old_tag(viewport,tags.data(),3,command);run(first);
    need(sink.last.ready()&&sink.last.binding.frame_index==1,"Legacy-only path admitted as metadata");
    auto* second=begin(2);frame_tag(*second,viewport,tags.data(),3,command);run(second);
    need(sink.last.ready()&&sink.last.binding.frame_index==2,"Frame-scoped path uses actual frame");
    auto* third=begin(3);old_tag(viewport,tags.data(),3,command);run(third);
    need(!sink.last.ready(),"After frame tagging, legacy tags cannot silently fill missing frame");
    expected_token=second;frame_tag(*second,viewport,tags.data(),3,command);run(third);
    need(!sink.last.ready(),"Other frame's tags do not satisfy this call");
    expected_token=third;fail_tags=true;
    need(frame_tag(*third,viewport,tags.data(),3,command)==sl::Result::eErrorInvalidParameter,"Failed result preserved");run(third);
    need(!sink.last.ready(),"Failed tag submission cannot supply resources");fail_tags=false;
    frame_tag(*third,viewport,tags.data(),3,command);run(third);
    need(sink.last.ready(),"Correct fresh frame can recover without permanent failure");
    for(bool use_frame:{false,true}){throw_tags=true;bool caught=false;
        try{if(use_frame)frame_tag(*third,viewport,tags.data(),3,command);else old_tag(viewport,tags.data(),3,command);}
        catch(const std::runtime_error&){caught=true;}need(caught,"Foreign exception propagated");throw_tags=false;}
    need(sink.aborts==2,"Both original exceptions invalidate metadata");
    const auto before=lab::streamline_snapshot();const auto delivered=sink.frame_events+sink.legacy_events;
    lab::detach_streamline_boundary();old_tag(viewport,tags.data(),3,command);frame_tag(*third,viewport,tags.data(),3,command);
    const auto after=lab::streamline_snapshot();
    need(before["legacy_tag_calls"]==after["legacy_tag_calls"]&&before["frame_tag_calls"]==after["frame_tag_calls"]&&
        delivered==sink.frame_events+sink.legacy_events,"Detached calls forward without metadata/counters");
    need(legacy_calls==4&&framed_calls==6&&!virtuals,"Both originals exact counts, no token virtual calls");
    lab::uninstall_streamline_observer();installed=false;
    need(lab::streamline_snapshot()["installed_export_count"]==0,"Quiescent test cleanup removes five hooks");
    std::cout<<lab::json{{"passed",true},{"checks",checks},{"legacy_original_calls",legacy_calls},{"frame_original_calls",framed_calls},
        {"game_executed",false},{"gpu_executed",false},{"files_captured",0}}.dump()<<'\n';return 0;
}catch(const std::exception& e){if(installed)lab::uninstall_streamline_observer();std::cerr<<e.what()<<'\n';return 1;}}
