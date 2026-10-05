// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_streamline.hpp"
#include <iostream>
using namespace lab::slboundary;
namespace {
unsigned checks=0,virtual_calls=0,original_calls=0,completed_calls=0;
bool inside=false,reenter=false,throw_original=false,mutate_index=false;
sl::Result result=sl::Result::eOk;
struct Token final:sl::FrameToken {operator std::uint32_t()const override{++virtual_calls;return 1;}} token;
PFun_slEvaluateFeature* invoke=nullptr;
void need(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Receiver final:Sink {
    std::array<Call,16> calls{};unsigned count=0;bool overflow=false,bad_scope=false;
    unsigned aborted_count=0;
    void aborted(const Call&) noexcept override {++aborted_count;}
    void returned(const Call& c) noexcept override {
        if(inside || completed_calls<count+1)bad_scope=true;
        if(count==calls.size()){overflow=true;return;}calls[count++]=c;
    }
};
__declspec(noinline) sl::Result get(sl::FrameToken*& out,const uint32_t* index) {++original_calls;out=&token;
    if(mutate_index&&index)++*const_cast<uint32_t*>(index);++completed_calls;return result;}
__declspec(noinline) sl::Result set(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&){++original_calls;++completed_calls;return result;}
__declspec(noinline) sl::Result tags(const sl::ViewportHandle&,const sl::ResourceTag*,uint32_t,sl::CommandBuffer*){++original_calls;++completed_calls;return result;}
__declspec(noinline) sl::Result evaluate(sl::Feature f,const sl::FrameToken& t,const sl::BaseStructure** inputs,uint32_t count,sl::CommandBuffer* cmd) {
    ++original_calls;
    if(throw_original)throw std::runtime_error("original API exception");
    if(reenter){reenter=false;invoke(f,t,inputs,count,cmd);}
    inside=true;++completed_calls;inside=false;return result;
}
}
int main(){bool hooked=false;try {
    sl::ViewportHandle v(7u);sl::Extent e{0,0,2560,1080};
    sl::Resource depth(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x1234),0x40);
    sl::Resource motion(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x5678),0x40);
    sl::ResourceTag d(&depth,sl::kBufferTypeDepth,sl::eValidUntilEvaluate,&e);
    sl::ResourceTag m(&motion,sl::kBufferTypeMotionVectors,sl::eValidUntilPresent,&e);
    sl::Constants c;c.jitterOffset={.25f,-.125f};c.mvecScale={1.f/2560,1.f/1080};c.depthInverted=sl::eTrue;c.reset=sl::eFalse;
    const sl::BaseStructure* in[]{&v,&d,&m,&c};
    auto packet=decode_inputs(in,4);
    need(packet.issues==0&&packet.tag_count==2&&packet.viewport==7&&packet.constants_present,"Valid typed packet");
    need(packet.tags[0].native==depth.native&&packet.tags[0].state==0x40&&packet.tags[0].extent.width==2560,"Exact borrowed resource metadata");
    need(packet.tags[0].lifecycle==sl::eValidUntilEvaluate&&packet.tags[1].lifecycle==sl::eValidUntilPresent,"Lifecycle is not normalized");
    c.jitterOffset.x=9;need(packet.constants.jitterOffset.x==.25f,"Constants must be copied before original");
    need(packet.constants.next==nullptr,"Packet must not retain a caller chain");
    const sl::BaseStructure* local[]{&v};need(decode_inputs(local,1).tag_count==0,"Never fill local tags from last global tags");
    sl::ResourceTag revoked(nullptr,sl::kBufferTypeDepth,sl::eValidUntilPresent);
    packet=decode_tags(v,&revoked,1);need(packet.issues==0&&packet.tags[0].null_resource,"Explicit global tag revocation");
    d.lifecycle=sl::eOnlyValidNow;packet=decode_tags(v,&d,1);need(packet.issues==0&&packet.tags[0].lifecycle==sl::eOnlyValidNow,"OnlyValidNow must remain explicit");d.lifecycle=sl::eValidUntilEvaluate;
    need(decode_inputs(reinterpret_cast<const sl::BaseStructure**>(1),1).issues&fault,"Bad input address");
    need(decode_inputs(nullptr,17).issues&bound,"Input count bound before pointer access");
    need(decode_tags(v,nullptr,17).issues&bound,"Tag count bound before pointer access");
    need(decode_tags(v,reinterpret_cast<const sl::ResourceTag*>(1),1).issues&fault,"Bad tag address");
    d.resource=reinterpret_cast<sl::Resource*>(1);need(decode_tags(v,&d,1).issues&fault,"Bad resource address");d.resource=&depth;
    d.structVersion=2;need(decode_tags(v,&d,1).issues&version,"Unknown tag version");d.structVersion=1;
    depth.structVersion=2;need(decode_tags(v,&d,1).issues&version,"Unknown resource version");depth.structVersion=1;
    v.structVersion=2;need(decode_inputs(local,1).issues&version,"Unknown viewport version");v.structVersion=1;
    c.structVersion=1;need(decode_constants(v,c).issues&version,"Do not over-read older constant layout");c.structVersion=2;
    c.structType={};need(decode_constants(v,c).issues&invalid,"Wrong constant GUID");c.structType=sl::Constants::s_structType;
    d.next=&d;need(decode_inputs(in,4).issues&cycle,"Cycle");d.next=nullptr;
    sl::ResourceTag dup=d;const sl::BaseStructure* duplicate_tags[]{&v,&d,&dup};
    need(decode_inputs(duplicate_tags,3).issues&duplicate,"Repeated semantic tag");
    sl::ViewportHandle second(8u);const sl::BaseStructure* duplicate_view[]{&v,&second};
    need(decode_inputs(duplicate_view,2).issues&duplicate,"Repeated viewport");
    const sl::BaseStructure* duplicate_ptr[]{&v,&v};need(decode_inputs(duplicate_ptr,2).issues&cycle,"Repeated pointer");
    std::array<sl::ViewportHandle,33> chain{};for(unsigned i=0;i+1<chain.size();++i)chain[i].next=&chain[i+1];
    const sl::BaseStructure* long_chain[]{&chain[0]};need(decode_inputs(long_chain,1).issues&bound,"Total chain bound");
    sl::BaseStructure extension_node({},1);d.next=&extension_node;
    need(decode_tags(v,&d,1).issues&extension,"Tag extension not silently dropped");
    need(decode_inputs(in,4).issues&unknown_input,"Unknown local extension not silently dropped");d.next=nullptr;
    depth.next=&extension_node;need(decode_tags(v,&d,1).issues&extension,"Resource extension not silently dropped");depth.next=nullptr;
    depth.state=UINT32_MAX;need(decode_tags(v,&d,1).issues&invalid,"No guessed state");depth.state=0x40;
    d.extent.height=0;need(decode_tags(v,&d,1).issues&invalid,"Partial extent invalid");d.extent=e;
    d.lifecycle=static_cast<sl::ResourceLifecycle>(99);need(decode_tags(v,&d,1).issues&invalid,"Unknown lifecycle");d.lifecycle=sl::eValidUntilEvaluate;
    need(decode_inputs(nullptr,0).issues&invalid,"Missing viewport");

    // Real MinHook on our own public-signature functions, with NO observer log.
    Receiver receiver;lab::json diagnostics;
    std::array<void*,4> targets{reinterpret_cast<void*>(get),reinterpret_cast<void*>(evaluate),reinterpret_cast<void*>(set),reinterpret_cast<void*>(tags)};
    need(lab::install_streamline_boundary(targets,&receiver,diagnostics),"Standalone boundary install without CommandLog");hooked=true;
    need(!lab::install_streamline_boundary(targets,&receiver,diagnostics),"No double installation");
    auto volatile get_fn=reinterpret_cast<PFun_slGetNewFrameToken*>(targets[0]);
    auto volatile set_fn=reinterpret_cast<PFun_slSetConstants*>(targets[2]);
    auto volatile tags_fn=reinterpret_cast<PFun_slSetTag*>(targets[3]);
    invoke=reinterpret_cast<PFun_slEvaluateFeature*>(targets[1]);
    sl::FrameToken* out=nullptr;need(get_fn(out,nullptr)==result&&out==&token,"Token forwarding");
    need(set_fn(c,token,v)==result,"Constants forwarding");
    need(tags_fn(v,&d,1,nullptr)==result,"Global tag forwarding");
    need(invoke(sl::kFeatureDLSS_RR,token,in,4,reinterpret_cast<void*>(0x1000))==result,"Evaluate forwarding");
    auto& call=receiver.calls[3];need(call.api==Api::evaluate&&call.inputs.tag_count==2&&call.inputs.viewport==7&&call.token==&token&&call.command==reinterpret_cast<void*>(0x1000),"Evaluate exact inputs");
    need(receiver.calls[2].api==Api::tags&&receiver.calls[2].token==nullptr,"No invented token for global tags");
    need(invoke(sl::kFeatureDLSS_RR,token,local,1,nullptr)==result&&receiver.calls[4].inputs.tag_count==0,"Local/global isolation through hook");
    result=sl::Result::eErrorInvalidParameter;
    need(invoke(sl::kFeatureDLSS_RR,token,reinterpret_cast<const sl::BaseStructure**>(1),1,nullptr)==result,"Metadata failure must not suppress original");
    need(receiver.calls[5].result==result&&(receiver.calls[5].inputs.issues&fault),"Failed call is not successful binding");result=sl::Result::eOk;
    reenter=true;invoke(sl::kFeatureDLSS_RR,token,local,1,nullptr);
    need(receiver.calls[6].parent==receiver.calls[7].id&&receiver.calls[7].parent==0,"Exact nesting and return order");
    need(receiver.calls[6].concurrent&&receiver.calls[7].concurrent,"Nested overlap is visible to consumers");
    uint32_t index=42;get_fn(out,&index);need(receiver.calls[8].frame_index_known&&receiver.calls[8].frame_index==42,"Explicit caller frame index copied");
    mutate_index=true;get_fn(out,&index);mutate_index=false;
    need(index==43&&!receiver.calls[9].frame_index_known&&(receiver.calls[9].inputs.issues&invalid),"Changed caller index cannot identify a frame");
    throw_original=true;bool caught=false;try{invoke(sl::kFeatureDLSS_RR,token,local,1,nullptr);}catch(const std::runtime_error&){caught=true;}throw_original=false;
    need(caught&&receiver.aborted_count==1&&receiver.count==10,"Original exception escapes, boundary abort delivered without fake return");
    const auto received=receiver.count;lab::detach_streamline_boundary();get_fn(out,nullptr);
    need(receiver.count==received&&original_calls==received+2,"Detach keeps forwarding without callbacks");
    need(!virtual_calls&&!receiver.overflow&&!receiver.bad_scope,"No private token call or premature callback");
    const auto stats=lab::streamline_snapshot();need(stats["calls"]==0&&stats["boundary_active_calls"]==0,"No observer activity");
    lab::uninstall_streamline_observer();hooked=false;
    std::cout<<"PASS checks="<<checks<<" boundary_calls="<<received<<" raw_files=0 game_control=false\n";return 0;
}catch(const std::exception& e){if(hooked)lab::uninstall_streamline_observer();std::cerr<<e.what()<<'\n';return 1;}}
