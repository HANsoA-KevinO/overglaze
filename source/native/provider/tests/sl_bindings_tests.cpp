// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_sl_bindings.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
using namespace lab::slboundary;
namespace {
unsigned checks=0;
void need(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
struct Fixture {
    Bindings bindings;std::uint64_t seq=0;
    sl::ViewportHandle v{0};sl::Constants common{};
    void* ptr=reinterpret_cast<void*>(0x9870);void* cmd=reinterpret_cast<void*>(0x7650);
    std::array<sl::Resource,3> resources{
        sl::Resource(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x1000),8),
        sl::Resource(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x2000),64),
        sl::Resource(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x3000),64)};
    sl::Extent output{0,0,1280,720},guide{0,0,640,360};
    std::array<sl::ResourceTag,3> tags{
        sl::ResourceTag(&resources[0],sl::kBufferTypeScalingOutputColor,sl::eValidUntilPresent,&output),
        sl::ResourceTag(&resources[1],sl::kBufferTypeDepth,sl::eValidUntilEvaluate,&guide),
        sl::ResourceTag(&resources[2],sl::kBufferTypeMotionVectors,sl::eValidUntilEvaluate,&guide)};
    Call base(Api api){Call c;c.api=api;c.id=++seq;c.thread=7;c.result=sl::Result::eOk;return c;}
    void emit(Call c){bindings.entering(c);bindings.returned(c);}
    void issue(unsigned index=1,bool known=true){auto c=base(Api::token);c.token=ptr;c.frame_index=index;c.frame_index_known=known;emit(c);}
    Call constants(){auto c=base(Api::constants);c.token=ptr;c.inputs=decode_constants(v,common);return c;}
    Call global(){auto c=base(Api::tags);c.command=cmd;c.inputs=decode_tags(v,tags.data(),3);return c;}
    Call framed(){auto c=global();c.frame_scoped_tags=true;c.token=ptr;return c;}
    void setup(bool present=true){if(present)bindings.present_boundary();issue();emit(constants());emit(global());}
    Call eval(const sl::BaseStructure** local=nullptr,unsigned count=0){auto c=base(Api::evaluate);c.feature=sl::kFeatureDLSS_RR;c.token=ptr;c.command=cmd;
        const sl::BaseStructure* viewport[]{&v};c.inputs=decode_inputs(local?local:viewport,local?count:1);return c;}
    Resolution run(Call c){bindings.entering(c);return bindings.returned(c);}
};
}
// Kept out of main(): main's frame already holds hundreds of Fixture and
// Resolution temporaries and sits close to the default 1 MiB stack.
__declspec(noinline) void self_configuration_and_super_resolution(){
    // ---- self-configuration (controller host): viewport and depth from the calls
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.bindings.self_configure_before_attach();f.v=sl::ViewportHandle{1};
     need(f.bindings.viewport_selection().automatic&&!f.bindings.viewport_selection().locked,"Nothing is locked before the first RR call");
     f.setup();auto r=f.run(f.eval());
     need(r.rejection==Rejection::viewport_selected,"The first RR call selects the viewport and is a skip");
     auto s=f.bindings.viewport_selection();need(s.locked&&s.viewport==1&&s.switches==0,"Viewport 1 selected from the call");
     f.issue(2);f.emit(f.constants());f.emit(f.global());r=f.run(f.eval());
     need(r.ready()&&r.binding.viewport==1,"The next frame on the selected viewport is admitted");
     // Settling: inputs missing right after selection are a skip, then named.
     unsigned skips=0;for(unsigned frame=3;frame<12;++frame){f.issue(frame);f.emit(f.global());r=f.run(f.eval());
         if(r.rejection!=Rejection::viewport_selected)break;++skips;}
     need(skips>0&&skips<4&&r.rejection==Rejection::constants,"After a short settling window missing constants are reported as such");
     // Another viewport's setters and Evaluate leave ours intact.
     f.issue(20);f.emit(f.constants());f.emit(f.global());
     f.v=sl::ViewportHandle{2};f.emit(f.constants());f.emit(f.global());
     r=f.run(f.eval());
     need(r.rejection==Rejection::viewport&&f.bindings.viewport_selection().other_viewport_calls==1,"An RR call on another viewport is refused and counted");
     f.v=sl::ViewportHandle{1};r=f.run(f.eval());
     need(r.ready()&&r.binding.viewport==1,"Another viewport's constants, tags and Evaluate leave ours intact");
     // A viewport the game moved to for 8 consecutive RR calls is taken over.
     f.v=sl::ViewportHandle{5};
     for(unsigned i=0;i<7;++i){f.issue(30+i);need(f.run(f.eval()).rejection==Rejection::viewport,"Other-viewport calls before the switch are refused");}
     f.issue(37);r=f.run(f.eval());s=f.bindings.viewport_selection();
     need(r.rejection==Rejection::viewport_selected&&s.viewport==5&&s.switches==1,"The eighth consecutive call moves the viewport");
     f.issue(38);f.emit(f.constants());f.emit(f.global());
     need(f.run(f.eval()).ready(),"and the new viewport is admitted on its next frame");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.bindings.self_configure_before_attach();f.setup();f.run(f.eval());
     for(unsigned frame=2;frame<22;++frame){f.issue(frame);f.v=sl::ViewportHandle{frame%2};f.emit(f.constants());f.emit(f.global());f.run(f.eval());}
     const auto s=f.bindings.viewport_selection();
     need(s.viewport==0&&s.switches==0,"Two views evaluated every frame never move the lock");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.bindings.self_configure_before_attach();f.tags[1].type=sl::kBufferTypeLinearDepth;f.setup();f.run(f.eval());
     f.issue(2);f.emit(f.constants());f.emit(f.global());auto r=f.run(f.eval());
     need(r.ready()&&r.binding.resources[1].type==sl::kBufferTypeLinearDepth,"Linear depth fills the depth role and keeps its own type");}
    for(bool hardware_first:{true,false}){auto fx=std::make_unique<Fixture>();auto& f=*fx;f.bindings.self_configure_before_attach();f.setup();f.run(f.eval());
     sl::Resource linear_resource(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x4000),64);
     sl::ResourceTag linear_tag(&linear_resource,sl::kBufferTypeLinearDepth,sl::eValidUntilEvaluate,&f.guide);
     std::array<sl::ResourceTag,4> both{f.tags[0],f.tags[1],linear_tag,f.tags[2]};
     if(!hardware_first){both[1]=linear_tag;both[2]=f.tags[1];}
     f.issue(2);f.emit(f.constants());auto c=f.base(Api::tags);c.command=f.cmd;c.inputs=decode_tags(f.v,both.data(),4);f.emit(c);
     auto r=f.run(f.eval());
     need(r.ready()&&r.binding.resources[1].type==sl::kBufferTypeDepth&&r.binding.resources[1].native==f.resources[1].native,
          "Hardware depth wins when a setter tags both semantics");
     f.issue(3);f.emit(f.constants());
     const sl::BaseStructure* in_hw_first[]{&f.v,&f.tags[0],&f.tags[1],&linear_tag,&f.tags[2]};
     const sl::BaseStructure* in_linear_first[]{&f.v,&f.tags[0],&linear_tag,&f.tags[1],&f.tags[2]};
     r=f.run(f.eval(hardware_first?in_hw_first:in_linear_first,5));
     need(r.ready()&&r.binding.local[1]&&r.binding.resources[1].type==sl::kBufferTypeDepth,"Hardware depth wins among inline tags too");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.v=sl::ViewportHandle{1};f.setup();
     need(f.run(f.eval()).rejection==Rejection::viewport,"The pinned profile is unchanged: another viewport stays refused");}
    // ---- Plague Tale: token fetches from other threads; frame-keyed tags across Present
    const auto settled=[](Fixture& f,bool framed){f.bindings.self_configure_before_attach();f.setup();f.run(f.eval());
        for(unsigned frame=2;frame<8;++frame){f.issue(frame);f.emit(f.constants());f.emit(framed?f.framed():f.global());f.run(f.eval());}};
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;settled(f,false);
     need(f.bindings.stats().rejections[0]>=5,"Ready frames are counted");
     f.issue(10);auto k=f.constants();k.concurrent=true;k.token_only_overlap=true;f.emit(k);
     auto g=f.global();g.concurrent=true;g.token_only_overlap=true;f.emit(g);
     auto e=f.eval();e.concurrent=true;e.token_only_overlap=true;
     need(f.run(e).ready(),"Overlapping only token fetches is not a conflict when self-configuring");
     need(f.bindings.stats().benign_token_overlaps>=3,"Benign token overlaps are counted");
     // An overlap by itself does not refuse when self-configuring; a legacy
     // tag setter returning INSIDE the Evaluate does, and says with what.
     f.issue(11);f.emit(f.constants());f.emit(f.global());auto x=f.eval();x.concurrent=true;x.overlapped_api=2;
     need(f.run(x).ready(),"A concurrent Evaluate whose inputs no setter touched is admitted when self-configuring");
     f.issue(12);f.emit(f.constants());f.emit(f.global());x=f.eval();x.concurrent=true;x.overlapped_api=2;
     f.bindings.entering(x);{auto g2=f.global();g2.concurrent=true;f.emit(g2);}
     need(f.bindings.returned(x).rejection==Rejection::overlap&&f.bindings.stats().overlap_with[2]>=1&&f.bindings.stats().same_key_overlaps==1,
          "A legacy tag setter returning inside the Evaluate refuses that one call, and says with what");
     // A legacy global tag survives the previous frame's Present (an engine
     // with a submission thread), and the tolerated Present is counted.
     f.issue(16);f.emit(f.constants());f.emit(f.global());f.bindings.present_boundary();
     need(f.run(f.eval()).ready(),"A legacy global tag survives another frame's Present when self-configuring (Plague Tale)");
     need(f.bindings.stats().present_expiries>=1&&f.bindings.stats().global_tag_calls>0,"The tolerated Present is counted");
     // Freshness is still strict: an Evaluate consumes the tags, the next finds none.
     f.issue(13);f.emit(f.constants());
     need(f.run(f.eval()).rejection==Rejection::resource,"Consumed global tags are not reused");
     const auto s=f.bindings.stats();
     need(s.resource_roles==7u&&(s.resource_causes&2u),"The missing roles and their cause are recorded");
     // A Present during the Evaluate call is tolerated too, for a durable tag.
     f.issue(14);f.emit(f.constants());f.emit(f.global());{auto c=f.eval();f.bindings.entering(c);f.bindings.present_boundary();
      need(f.bindings.returned(c).ready(),"A Present inside the Evaluate window does not revoke a durable tag when self-configuring");}
     // OnlyValidNow is still never carried to an Evaluate.
     f.issue(15);f.emit(f.constants());f.tags[1].lifecycle=sl::eOnlyValidNow;f.emit(f.global());f.tags[1].lifecycle=sl::eValidUntilEvaluate;
     need(f.run(f.eval()).rejection==Rejection::volatile_global,"OnlyValidNow stays refused");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;settled(f,true);
     f.issue(20);f.emit(f.constants());f.emit(f.framed());f.bindings.present_boundary();
     need(f.run(f.eval()).ready(),"A frame-keyed tag survives another frame's Present when self-configuring");
     need(f.bindings.stats().frame_tag_calls>0,"Frame tag calls counted");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.setup();auto e=f.eval();e.concurrent=true;e.token_only_overlap=true;
     need(f.run(e).rejection==Rejection::overlap,"The pinned profile treats any overlap as a conflict");}
    // ---- a broken tag NR does not use does not discard the three it does (Plague Tale)
    for(bool self:{true,false}){auto fx=std::make_unique<Fixture>();auto& f=*fx;
     sl::Resource buffer(sl::ResourceType::eBuffer,reinterpret_cast<void*>(0x5000),0);
     sl::Resource no_native(sl::ResourceType::eTex2d,nullptr,0);
     const std::array<sl::ResourceTag,5> five{f.tags[0],f.tags[1],f.tags[2],
        sl::ResourceTag(&buffer,sl::kBufferTypeAnimatedTextureHint,sl::eValidUntilPresent,&f.guide),
        sl::ResourceTag(&no_native,sl::kBufferTypeAlbedo,sl::eValidUntilPresent,&f.guide)};
     const auto decoded=decode_tags(f.v,five.data(),5);
     need(decoded.issues!=0&&decoded.call_issues==0,"Problems confined to single tags are not call-level");
     if(self){settled(f,false);}else{f.issue();f.emit(f.constants());f.bindings.present_boundary();}
     f.issue(30);f.emit(f.constants());
     auto c=f.base(Api::tags);c.command=f.cmd;c.inputs=decoded;f.emit(c);
     const auto r=f.run(f.eval());
     if(self){need(r.ready(),"Self-configuring: the three role tags survive a broken extra tag");
              need(f.bindings.stats().tags_isolated>=1,"and the isolation is counted");}
     else need(r.rejection==Rejection::resource,"The pinned profile still discards the whole call");}
    // A broken ROLE tag is still refused, by name.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;settled(f,false);
     sl::Resource no_native(sl::ResourceType::eTex2d,nullptr,0);
     const std::array<sl::ResourceTag,3> three{f.tags[0],sl::ResourceTag(&no_native,sl::kBufferTypeDepth,sl::eValidUntilPresent,&f.guide),f.tags[2]};
     f.issue(40);f.emit(f.constants());auto c=f.base(Api::tags);c.command=f.cmd;c.inputs=decode_tags(f.v,three.data(),3);f.emit(c);
     need(f.run(f.eval()).rejection==Rejection::resource&&(f.bindings.stats().resource_roles&2u),"A broken depth tag is refused and named");}
    // ---- DLSS super resolution as a second target
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.setup();auto sr=f.eval();sr.feature=sl::kFeatureDLSS;
     need(f.run(sr).rejection==Rejection::not_target&&!f.bindings.targets_super_resolution(),"SR is not a target by default");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.bindings.target_super_resolution_before_attach();f.setup();auto sr=f.eval();sr.feature=sl::kFeatureDLSS;
     auto r=f.run(sr);
     need(r.ready()&&r.binding.feature==sl::kFeatureDLSS&&r.binding.resources[0].native==f.resources[0].native,"A targeted SR call resolves exactly like RR");
     f.issue(2);f.emit(f.constants());f.emit(f.global());r=f.run(f.eval());
     need(r.ready()&&r.binding.feature==sl::kFeatureDLSS_RR,"RR still resolves beside it");
     f.issue(3);f.emit(f.constants());f.emit(f.global());auto other=f.eval();other.feature=sl::kFeatureDLSS_G;
     need(f.run(other).rejection==Rejection::not_target,"Other features stay non-targets");}

}
// eOnlyValidNow legacy tags (Cyberpunk 2077 with DLSS-G on declares them).
// The adapter copies depth/motion at the tag call and hands this layer a tag
// naming the copy (lab_copy). Here: a copy binds, everything else about the
// tag keeps its rules, an uncopied or output-role tag stays refused BY NAME.
__declspec(noinline) void only_valid_now_copies(){
    void* const depth_copy=reinterpret_cast<void*>(0x6100ull);void* const motion_copy=reinterpret_cast<void*>(0x6200ull);
    // The tag call as the adapter hands it on: role tags OnlyValidNow, copied or refused.
    const auto volatile_call=[&](Fixture& f,bool copy_depth,bool copy_motion,CopyOutcome refusal=CopyOutcome::none){
        auto c=f.global();for(unsigned i=1;i<3;++i){auto& t=c.inputs.tags[i];t.lifecycle=sl::eOnlyValidNow;
            if(i==1?copy_depth:copy_motion){t.native=i==1?depth_copy:motion_copy;t.state=64;t.lab_copy=10+i;}
            else t.copy_refusal=static_cast<std::uint8_t>(refusal);}
        return c;};
    const auto fresh=[](Fixture& f){f.bindings.present_boundary();f.issue();f.emit(f.constants());};
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;fresh(f);f.emit(volatile_call(f,true,true));const auto r=f.run(f.eval());
     need(r.ready()&&r.binding.resources[1].native==depth_copy&&r.binding.resources[2].native==motion_copy&&
          r.binding.resources[1].lab_copy==11&&r.binding.resources[2].lab_copy==12&&r.binding.resources[1].lifecycle==sl::eOnlyValidNow,
          "Copied OnlyValidNow guides bind as the Lab copies; the game's lifecycle is kept");
     const auto s=f.bindings.stats();
     need(s.copies_bound[1]==1&&s.copies_bound[2]==1&&s.role_lifecycles[1*10+2*3+0]==1&&s.role_lifecycles[2*10+2*3+0]==1&&s.role_lifecycles[0*10+2*3+1]==1,
          "Role x source x lifecycle counted per Evaluate, copies bound counted");
     f.emit(f.constants());need(f.run(f.eval()).rejection==Rejection::resource,"A copy is consumed like any tag");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;fresh(f);f.emit(volatile_call(f,false,false));const auto r=f.run(f.eval());
     need(r.rejection==Rejection::volatile_global&&r.volatile_roles==6u&&r.volatile_reasons[1]==0&&r.volatile_reasons[2]==0,
          "Uncopied OnlyValidNow with no copy tried keeps its old name, with the roles");
     need(f.bindings.stats().volatile_roles==6u,"The latest OnlyValidNow refusal is kept in the stats");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;fresh(f);f.emit(volatile_call(f,true,false,CopyOutcome::awaiting_allocation));const auto r=f.run(f.eval());
     need(r.rejection==Rejection::volatile_copy&&r.volatile_roles==4u&&r.volatile_reasons[2]==static_cast<std::uint8_t>(CopyOutcome::awaiting_allocation),
          "A refused copy names its role and why; the copied role is not blamed");
     need(f.bindings.stats().volatile_reasons[2]==static_cast<std::uint8_t>(CopyOutcome::awaiting_allocation),"and the stats keep that reason");}
    // The output colour: NR writes back into it in place, so a copy is never bound.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;fresh(f);auto c=f.global();auto& t=c.inputs.tags[0];
     t.lifecycle=sl::eOnlyValidNow;t.lab_copy=99;f.emit(c);const auto r=f.run(f.eval());
     need(r.rejection==Rejection::volatile_copy&&r.volatile_roles==1u&&r.volatile_reasons[0]==static_cast<std::uint8_t>(CopyOutcome::output_role),
          "An OnlyValidNow output colour stays refused, by name, even if marked copied");}
    // Every other rule still applies to a copy's tag (pinned profile).
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;fresh(f);f.emit(volatile_call(f,true,true));f.bindings.present_boundary();
     need(f.run(f.eval()).rejection==Rejection::resource,"Pinned profile: a Present still ends a copied legacy tag's association");
     // The pinned profile counts it, copies apart.
     need(f.bindings.stats().present_expiries==1&&f.bindings.stats().present_expiries_copies==1,"Pinned profile: the Present that met a fresh copy is counted");}
    // ...but the copies themselves are ours: a Present between the guide tags
    // and Evaluate (2077 with DLSS-G) ends only the game's own output tag.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;fresh(f);f.emit(volatile_call(f,true,true));f.bindings.present_boundary();
     {auto g=f.global();g.inputs=decode_tags(f.v,f.tags.data(),1);f.emit(g);}
     const auto r=f.run(f.eval());
     need(r.ready()&&r.binding.resources[1].native==depth_copy&&r.binding.resources[2].native==motion_copy,
          "Pinned profile: the copies survive a Present and bind with the re-tagged output");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;fresh(f);f.emit(f.global());f.bindings.present_boundary();
     need(f.run(f.eval()).rejection==Rejection::resource&&f.bindings.stats().present_expiries==1&&f.bindings.stats().present_expiries_copies==0,
          "Pinned profile: a Present that expired fresh durable tags is counted, not as a copy");
     f.bindings.present_boundary();f.emit(f.constants());(void)f.run(f.eval());
     need(f.bindings.stats().present_expiries==1,"A Present with nothing fresh is not counted");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;fresh(f);auto c=volatile_call(f,true,true);c.thread=8;f.emit(c);
     need(f.run(f.eval()).rejection==Rejection::thread,"A copied legacy tag from another thread is still refused");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;fresh(f);f.emit(f.framed());auto c=volatile_call(f,false,false,CopyOutcome::frame_scoped);
     c.frame_scoped_tags=true;c.token=f.ptr;f.emit(c);const auto r=f.run(f.eval());
     need(r.rejection==Rejection::volatile_copy&&r.volatile_reasons[1]==static_cast<std::uint8_t>(CopyOutcome::frame_scoped),"A per-frame OnlyValidNow tag is refused by name");}
    // The adapter's Evaluate-time verdict on a copy.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;fresh(f);f.emit(volatile_call(f,true,true));auto e=f.eval();f.bindings.entering(e);
     f.bindings.refuse_entry(e.id+1,2u,{0,static_cast<std::uint8_t>(CopyOutcome::replaced_before_evaluate),0});
     f.bindings.refuse_entry(e.id,2u,{0,static_cast<std::uint8_t>(CopyOutcome::replaced_before_evaluate),0});
     const auto r=f.bindings.returned(e);
     need(r.rejection==Rejection::volatile_copy&&r.volatile_roles==2u&&r.volatile_reasons[1]==static_cast<std::uint8_t>(CopyOutcome::replaced_before_evaluate),
          "refuse_entry refuses exactly the named call, with the role and reason");}
    // Self-configuring: a Present inside the Evaluate window does not end a copy.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.bindings.self_configure_before_attach();f.setup();f.run(f.eval());
     for(unsigned frame=2;frame<8;++frame){f.issue(frame);f.emit(f.constants());f.emit(f.global());f.run(f.eval());}
     f.issue(20);f.emit(f.constants());auto c=volatile_call(f,true,true);c.inputs.tags[0].lifecycle=sl::eValidUntilEvaluate;f.emit(c);
     auto e=f.eval();f.bindings.entering(e);f.bindings.present_boundary();const auto r=f.bindings.returned(e);
     need(r.ready()&&r.presents_during_call==1,"Self-configuring: a copy survives a Present inside its Evaluate window, counted");
     f.issue(21);f.emit(f.constants());c=volatile_call(f,false,false);f.emit(c);
     need(f.run(f.eval()).rejection==Rejection::volatile_global,"Self-configuring: an uncopied OnlyValidNow tag stays refused");}
    // stores_tag_call: an economy for the copier, decided on the call alone.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;
     need(f.bindings.stores_tag_call(f.global()),"A clean legacy call on the pinned viewport is stored");
     auto c=f.global();c.inputs.viewport=1;need(!f.bindings.stores_tag_call(c),"Another viewport's call is not");
     c=f.global();c.concurrent=true;need(!f.bindings.stores_tag_call(c),"Pinned profile: an overlapping call is not");
     c=f.global();c.result=sl::Result::eErrorInvalidParameter;need(!f.bindings.stores_tag_call(c),"A failed call is not");
     c=f.global();c.api=Api::constants;need(!f.bindings.stores_tag_call(c),"Only tag calls");
     need(f.bindings.role_of(sl::kBufferTypeDepth)==1&&f.bindings.role_of(sl::kBufferTypeMotionVectors)==2&&f.bindings.role_of(sl::kBufferTypeScalingOutputColor)==0&&
          f.bindings.role_of(sl::kBufferTypeAlbedo)==-1,"Role map exposed");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.bindings.self_configure_before_attach();
     need(!f.bindings.stores_tag_call(f.global()),"Self-configuring: nothing is stored before a viewport is selected");}
    need(std::string(rejection_name(Rejection::volatile_copy))=="only-valid-now-copy-refused","The new rejection has a stable name");
    for(unsigned i=0;i<static_cast<unsigned>(CopyOutcome::count);++i)
        need(std::string(copy_outcome_name(static_cast<CopyOutcome>(i)))!="unknown-copy-outcome","Every copy outcome is named");
}
// Every self-configuring relaxation, and the pinned
// profile's unchanged strictness beside it.
__declspec(noinline) void gate_audit_relaxations(){
    const auto settle=[](Fixture& f,bool framed){f.bindings.self_configure_before_attach();f.setup();f.run(f.eval());
        for(unsigned frame=2;frame<8;++frame){f.issue(frame);f.emit(f.constants());f.emit(framed?f.framed():f.global());f.run(f.eval());}};
    // A concurrent setter for ANOTHER frame keeps every cache (the pinned profile clears all).
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;settle(f,true);
     f.issue(20);f.emit(f.constants());f.emit(f.framed());
     {f.issue(21);auto k=f.constants();k.concurrent=true;k.overlapped_api=3;f.emit(k);f.issue(20);}
     need(f.run(f.eval()).ready(),"A concurrent constants call for another frame no longer clears ours");
     need(f.bindings.stats().concurrency_clears==0&&f.bindings.stats().tolerated_overlaps>=1,"Nothing cleared; the tolerated overlap is counted");}
    // A frame-keyed tag setter for ANOTHER frame, returning inside our Evaluate, is harmless.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;settle(f,true);
     f.issue(30);f.emit(f.constants());f.emit(f.framed());auto e=f.eval();e.concurrent=true;f.bindings.entering(e);
     {auto g=f.framed();g.concurrent=true;g.token=reinterpret_cast<void*>(0x9990);
      auto tk=f.base(Api::token);tk.token=g.token;tk.frame_index=31;tk.frame_index_known=true;f.emit(tk);f.emit(g);}
     need(f.bindings.returned(e).ready(),"Another frame's frame-keyed tags returning inside our Evaluate do not refuse it");
     // ...but the same frame's tags do.
     f.issue(32);f.emit(f.constants());f.emit(f.framed());e=f.eval();e.concurrent=true;f.bindings.entering(e);
     {auto g=f.framed();g.concurrent=true;f.emit(g);}
     need(f.bindings.returned(e).rejection==Rejection::overlap,"The same frame's tags returning inside our Evaluate refuse that one call");
     f.issue(33);f.emit(f.constants());f.emit(f.framed());
     need(f.run(f.eval()).ready(),"The frame after it binds normally");}
    // A tag call that failed or had a call-level problem doubts only its own target.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;settle(f,true);
     f.issue(40);f.emit(f.constants());f.emit(f.framed());
     {auto g=f.framed();g.token=reinterpret_cast<void*>(0x9990);g.result=sl::Result::eErrorInvalidParameter;
      auto tk=f.base(Api::token);tk.token=g.token;tk.frame_index=41;tk.frame_index_known=true;f.emit(tk);f.emit(g);}
     need(f.run(f.eval()).ready(),"A failed tag call for another frame leaves ours fresh");
     f.issue(42);f.emit(f.constants());f.emit(f.framed());
     {auto g=f.framed();g.token=reinterpret_cast<void*>(0x9990);g.inputs.call_issues|=invalid;g.inputs.issues|=invalid;
      auto tk=f.base(Api::token);tk.token=g.token;tk.frame_index=43;tk.frame_index_known=true;f.emit(tk);f.emit(g);}
     need(f.run(f.eval()).ready(),"A malformed tag call for another frame leaves ours fresh");}
    // A non-target Evaluate consumes nothing of ours.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;settle(f,false);
     f.issue(50);f.emit(f.constants());f.emit(f.global());
     {auto other=f.eval();other.feature=sl::kFeatureDLSS;f.run(other);} // SR is not a target here
     need(f.run(f.eval()).ready(),"Another feature's Evaluate between tagging and ours does not expire our tags");}
    // Inline constants, and an unusable inline tag that is not one of our roles.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;settle(f,false);
     f.issue(60);f.emit(f.global());
     f.common.mvecScale={.25f,.5f};
     const sl::BaseStructure* chain[]{&f.v,&f.common};
     auto r=f.run(f.eval(chain,2));
     need(r.ready()&&r.binding.constants.mvecScale.x==.25f&&r.binding.constants_call==r.binding.call,
          "Constants passed inline with the Evaluate are this call's constants");
     sl::Resource buffer(sl::ResourceType::eBuffer,reinterpret_cast<void*>(0x5000),0);
     sl::ResourceTag odd(&buffer,sl::kBufferTypeAlbedo,sl::eValidUntilPresent,&f.guide);
     f.issue(61);f.emit(f.constants());f.emit(f.global());
     const sl::BaseStructure* with_odd[]{&f.v,&odd};
     auto e=f.eval(with_odd,2);need(e.inputs.issues!=0&&e.inputs.call_issues==0,"An odd inline tag is a per-tag issue, not a call-level one");
     need(f.run(e).ready(),"An unusable inline tag that is not ours does not refuse the Evaluate");}
    // A full token table gives up its oldest entry instead of clearing everything.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;settle(f,false);
     for(unsigned i=0;i<20;++i){auto c=f.base(Api::token);c.token=reinterpret_cast<void*>(0x10000+i*16);c.frame_index=100+i;c.frame_index_known=true;f.emit(c);}
     f.issue(70);f.emit(f.constants());f.emit(f.global());
     need(f.run(f.eval()).ready(),"Twenty distinct tokens do not clear the cache when self-configuring");}
    // Mixed tagging APIs: a role the frame-keyed call did not fill falls back to a fresh legacy tag.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;settle(f,true);
     f.issue(80);f.emit(f.constants());
     {auto g=f.framed();g.inputs=decode_tags(f.v,f.tags.data(),2);f.emit(g);}
     {auto g=f.global();g.inputs=decode_tags(f.v,f.tags.data()+2,1);f.emit(g);}
     const auto r=f.run(f.eval());
     need(r.ready()&&r.binding.resources[2].native==f.resources[2].native,"A legacy tag fills the role the frame-keyed call left empty");}
    // The pinned profile keeps these strict.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.setup();
     auto k=f.constants();k.concurrent=true;f.emit(k);
     need(f.bindings.stats().concurrency_clears>=1,"The pinned profile still clears on a concurrent setter");
     f.bindings.present_boundary();f.issue(2);f.emit(f.constants());f.emit(f.global());
     const sl::BaseStructure* chain[]{&f.v,&f.common};
     need(f.run(f.eval(chain,2)).rejection==Rejection::metadata,"The pinned profile still refuses inline constants");}
}
// With frame generation (Cyberpunk 2077, DLSS-G on) the game tags
// frame N -- and may already be inside Evaluate N -- before it presents frame N-1.
// A Present the game's own latency marker declares for an EARLIER frame than
// everything live expires nothing; between tags and Evaluate the same or a later
// frame, an undeclared Present, or live data whose frame cannot be derived keep the
// conservative boundary. Inside the Evaluate window only a hard boundary ends the call.
__declspec(noinline) void present_frame_attribution(){
    // Pinned profile (the research host, 2077). Frame 1 makes the globals' frame
    // derivable: tags set after a target Evaluate serve the frame after it.
    const auto primed=[](Fixture& f){f.setup();need(f.run(f.eval()).ready(),"Frame 1 binds");
        f.issue(2);f.emit(f.constants());f.emit(f.global());};
    // Inside the Evaluate window (the 2077 skips: snapshot-invalidated; present-boundary).
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;primed(f);auto c=f.eval();f.bindings.entering(c);
     bool spared=false;std::thread t([&]{spared=f.bindings.present_boundary_of_frame(1);});t.join();
     const auto r=f.bindings.returned(c);
     need(spared&&r.ready()&&!(r.invalidations&invalidation_present),"A Present declared for the previous frame inside Evaluate 2 invalidates nothing");
     need(r.presents_during_call==1,"The spared Present is still counted in the call, not hidden");
     const auto st=f.bindings.stats();
     need(st.present_expiries_spared_previous_frame==1&&st.presents_declared==1&&st.present_attribution_lock_misses==0,"Spared and declared Presents counted");}
    for(const std::uint32_t declared:{2u,3u}){auto fx=std::make_unique<Fixture>();auto& f=*fx;primed(f);auto c=f.eval();f.bindings.entering(c);
     bool spared=true;std::thread t([&]{spared=f.bindings.present_boundary_of_frame(declared);});t.join();
     const auto r=f.bindings.returned(c);
     // Inside the window no Present can be this frame's own: it ends nothing.
     need(!spared&&r.ready()&&!(r.invalidations&invalidation_present)&&r.presents_during_call==1,
          declared==2?"A Present declared for the Evaluate's own frame inside its window ends nothing, counted":"A Present declared for a later frame inside the window ends nothing, counted");
     need(f.bindings.stats().present_expiries_spared_previous_frame==0&&f.bindings.stats().presents_declared==1,"Declared but not spared");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;primed(f);auto c=f.eval();f.bindings.entering(c);
     std::thread t([&]{f.bindings.present_boundary();});t.join();const auto r=f.bindings.returned(c);
     need(r.ready()&&r.presents_during_call==1&&f.bindings.stats().presents_declared==0,"An undeclared Present inside the window (frame generation's pacing thread) ends nothing, counted");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;primed(f);auto c=f.eval();f.bindings.entering(c);
     std::thread t([&]{f.bindings.hard_boundary();});t.join();const auto r=f.bindings.returned(c);
     need(r.rejection==Rejection::stale&&r.invalidations==invalidation_present,"A hard boundary (resize, stop) inside the window still ends the call");}
    // Between the tags and the Evaluate (the 2077 fresh-resource-missing-or-revoked).
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;primed(f);
     need(f.bindings.present_boundary_of_frame(1),"The previous frame's Present after frame 2's tags is spared");
     const auto r=f.run(f.eval());need(r.ready()&&r.binding.frame_index==2,"Frame 2 binds its own tags across the previous frame's Present");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;primed(f);
     need(!f.bindings.present_boundary_of_frame(2),"Frame 2's own Present is not spared");
     need(f.run(f.eval()).rejection==Rejection::resource,"Frame 2's own Present still expires its global tags");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;primed(f);f.bindings.present_boundary();
     need(f.run(f.eval()).rejection==Rejection::resource,"A Present without a game marker still expires the global tags");}
    // No derivable frame: nothing is spared.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.setup();
     need(!f.bindings.present_boundary_of_frame(0),"Before any Evaluate a global tag's frame is unknown: not spared");
     need(f.run(f.eval()).rejection==Rejection::resource,"...and the Present expires it as before");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;primed(f);f.bindings.aborted(f.eval()); // an observation loss clears the cache
     f.issue(3);f.emit(f.constants());f.emit(f.global());
     need(!f.bindings.present_boundary_of_frame(2),"After a cache clear the frame is derived again only from the next Evaluate");
     need(f.run(f.eval()).rejection==Rejection::resource,"...so the Present still expires those tags");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;primed(f);auto other=f.eval();other.feature=sl::kFeatureDLSS;(void)f.run(other);
     f.emit(f.global());
     need(!f.bindings.present_boundary_of_frame(1),"After a non-target Evaluate consumed the globals their next frame is not derived");
     need(f.run(f.eval()).rejection==Rejection::resource,"...and the Present expires them");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.setup();
     need(!f.bindings.present_boundary_of_frame(5),"Nothing derivable live: an ordinary boundary");
     need(f.bindings.stats().present_expiries_spared_previous_frame==0,"Nothing spared without a derivable frame");}
    // Per-frame tags carry their own frame key.
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.bindings.present_boundary();f.issue(7);f.emit(f.constants());f.emit(f.framed());
     need(f.bindings.present_boundary_of_frame(6),"A Present of frame 6 spares frame 7's per-frame tags");
     need(f.run(f.eval()).ready(),"Frame 7 binds its per-frame tags");}
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;f.bindings.present_boundary();f.issue(7);f.emit(f.constants());f.emit(f.framed());
     need(!f.bindings.present_boundary_of_frame(7),"Frame 7's own Present is not spared");
     need(!f.run(f.eval()).ready(),"...and still expires its per-frame tags (pinned)");}
    // A run of frames, every one with the previous frame's Present inside its
    // window, the pattern measured on 2077 (77 of 1822 frames skipped).
    {auto fx=std::make_unique<Fixture>();auto& f=*fx;primed(f);unsigned admitted=0;
     for(std::uint32_t frame=2;frame<40;++frame){if(frame>2){f.issue(frame);f.emit(f.constants());f.emit(f.global());}
         auto c=f.eval();f.bindings.entering(c);(void)f.bindings.present_boundary_of_frame(frame-1);
         if(f.bindings.returned(c).ready())++admitted;}
     need(admitted==38&&f.bindings.stats().present_expiries_spared_previous_frame==38,"Every frame binds across the previous frame's Present");}
}
int main(){try {
    {Fixture f;f.setup();auto c=f.eval();c.concurrent=true;
     const auto r=f.run(c);need(r.rejection==Rejection::overlap&&r.overlap_sources==overlap_api_interval,"Original API overlap remains rejected and classified");}
    {Fixture f;f.setup();auto c=f.eval();c.parent=99;
     const auto r=f.run(c);need(r.rejection==Rejection::overlap&&r.overlap_sources==overlap_nested_call,"Nested call remains rejected and classified");}
    {Fixture f;f.setup();auto a=f.eval();f.bindings.entering(a);auto b=f.eval();f.bindings.entering(b);
     const auto r=f.bindings.returned(a);need(r.rejection==Rejection::overlap&&r.overlap_sources==overlap_pending_evaluate,"Overlapping target invocations remain rejected");}
    need(std::string(overlap_detail(overlap_api_interval|overlap_return_lock).data())=="original-api-interval|return-lock","Independent overlap causes retained");
    need(std::string(overlap_detail(0).data())=="unspecified"&&overlap_detail(31).back()==0,"Overlap detail stays bounded; absent cause not invented");
    {Fixture f;f.setup();const auto c=f.eval();auto r=f.run(c);need(r.ready(),"Complete fresh global transaction");
     need(r.binding.call==c.id&&r.binding.frame_index==1&&r.binding.constants_call==2&&r.binding.tag_calls[0]==3,"Exact source call IDs");
     need(r.binding.resources[0].native==f.resources[0].native&&!r.binding.local[0],"Borrow exact output");
     f.emit(f.constants());need(f.run(f.eval()).rejection==Rejection::resource,"Consumed global tags not reused");}
    {Fixture f;f.setup(false);need(f.run(f.eval()).rejection==Rejection::present_unknown,"Global tags require connected Present invalidation");}
    {Fixture f;f.setup();f.bindings.present_boundary();need(!f.run(f.eval()).ready(),"Present invalidates earlier tags/constants");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);std::thread t([&]{f.bindings.present_boundary();});t.join();
     const auto r=f.bindings.returned(c);
     need(r.ready()&&r.presents_during_call==1,"A concurrent Present ends no global tag already frozen for this call, counted");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);std::thread t([&]{f.bindings.hard_boundary();});t.join();
     const auto r=f.bindings.returned(c);
     need(r.rejection==Rejection::stale&&r.invalidations==invalidation_present,"A concurrent hard boundary is precisely diagnosed and still refused");}
    // Frame generation's pacing thread presents repeatedly inside one Evaluate
    // window. A Present ends GLOBAL tag validity; it does not end the validity
    // of a tag passed inline with this very Evaluate and declared valid until
    // Evaluate. Those three cases must stay apart.
    {Fixture f;f.setup();
     std::array<sl::ResourceTag,3> inline_tags{
        sl::ResourceTag(&f.resources[0],sl::kBufferTypeScalingOutputColor,sl::eValidUntilEvaluate,&f.output),
        sl::ResourceTag(&f.resources[1],sl::kBufferTypeDepth,sl::eValidUntilEvaluate,&f.guide),
        sl::ResourceTag(&f.resources[2],sl::kBufferTypeMotionVectors,sl::eValidUntilEvaluate,&f.guide)};
     const sl::BaseStructure* inputs[]{&f.v,&inline_tags[0],&inline_tags[1],&inline_tags[2]};
     auto c=f.eval(inputs,4);f.bindings.entering(c);
     std::thread t([&]{f.bindings.present_boundary();});t.join();
     const auto r=f.bindings.returned(c);
     need(r.binding.local[0]&&r.binding.local[1]&&r.binding.local[2],"Inline tags are recorded as local");
     need(r.ready()&&!(r.invalidations&invalidation_present),"Present does not invalidate inline eValidUntilEvaluate tags");
     need(r.presents_during_call==1,"A tolerated Present is still counted, not hidden");}
    {Fixture f;f.setup();
     std::array<sl::ResourceTag,3> inline_tags{
        sl::ResourceTag(&f.resources[0],sl::kBufferTypeScalingOutputColor,sl::eValidUntilPresent,&f.output),
        sl::ResourceTag(&f.resources[1],sl::kBufferTypeDepth,sl::eValidUntilEvaluate,&f.guide),
        sl::ResourceTag(&f.resources[2],sl::kBufferTypeMotionVectors,sl::eValidUntilEvaluate,&f.guide)};
     const sl::BaseStructure* inputs[]{&f.v,&inline_tags[0],&inline_tags[1],&inline_tags[2]};
     auto c=f.eval(inputs,4);f.bindings.entering(c);
     std::thread t([&]{f.bindings.present_boundary();});t.join();
     const auto r=f.bindings.returned(c);
     need(r.ready()&&r.presents_during_call==1,
          "An inline until-Present tag survives a Present inside its own window: that Present is not this frame's");}
    {Fixture f;f.setup();
     std::array<sl::ResourceTag,3> inline_tags{
        sl::ResourceTag(&f.resources[0],sl::kBufferTypeScalingOutputColor,sl::eValidUntilEvaluate,&f.output),
        sl::ResourceTag(&f.resources[1],sl::kBufferTypeDepth,sl::eValidUntilEvaluate,&f.guide),
        sl::ResourceTag(&f.resources[2],sl::kBufferTypeMotionVectors,sl::eValidUntilEvaluate,&f.guide)};
     const sl::BaseStructure* inputs[]{&f.v,&inline_tags[0],&inline_tags[1],&inline_tags[2]};
     auto c=f.eval(inputs,4);f.bindings.entering(c);
     auto other=f.eval(inputs,4);f.bindings.aborted(other);
     const auto r=f.bindings.returned(c);
     need(r.rejection==Rejection::stale&&(r.invalidations&invalidation_abort),
          "Another call aborting still invalidates inline tags");}
    {Fixture f;f.setup();f.issue(2);need(f.run(f.eval()).rejection==Rejection::constants,"Reused token pointer loses old constants");}
    {Fixture f;f.setup();f.issue(1);need(f.run(f.eval()).ready(),"Repeated explicit request for same token/index is not new frame");}
    {Fixture f;f.setup();const auto setter=f.seq-1;
     f.ptr=reinterpret_cast<void*>(0x9880);f.issue(2);
     f.ptr=reinterpret_cast<void*>(0x9890);f.issue(1);f.emit(f.global());
     const auto r=f.run(f.eval());
     need(r.ready()&&r.binding.constants_call==setter&&r.binding.frame_index==1,
          "Explicit same frame reissued at another token address keeps its exact per-frame constants");}
    {Fixture f;f.setup();const auto setter=f.seq-1;f.issue(2);f.issue(1);f.emit(f.global());
     const auto r=f.run(f.eval());need(r.ready()&&r.binding.constants_call==setter,
          "Token pool wraps back to an explicitly cached frame without inventing a new setter");}
    {Fixture f;f.setup();f.ptr=reinterpret_cast<void*>(0x9880);f.issue(1);
     auto bad=f.constants();bad.result=sl::Result::eErrorInvalidParameter;f.emit(bad);
     f.ptr=reinterpret_cast<void*>(0x9870);f.emit(f.global());
     auto r=f.run(f.eval());need(r.rejection==Rejection::constants&&r.binding.constants_call==bad.id,
          "Failed setter through an alias invalidates the exact frame for every token");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);
     f.ptr=reinterpret_cast<void*>(0x9880);f.issue(1);f.emit(f.constants());
     need(f.bindings.returned(c).rejection==Rejection::stale,"Alias setter cannot change constants under a frozen Evaluate");}
    {Fixture f;f.setup();for(unsigned frame=2;frame<=4;++frame){f.issue(frame);f.emit(f.constants());}
     f.issue(1);f.emit(f.global());need(f.run(f.eval()).rejection==Rejection::constants,
          "Evicted frame constants are not replaced by last-set or same modulo data");}
    {Fixture f;f.setup();f.issue(2);f.common.jitterOffset.x=.25f;f.emit(f.constants());
     f.issue(1);f.emit(f.global());auto r=f.run(f.eval());
     need(r.ready()&&r.binding.constants.jitterOffset.x!=.25f,"Interleaved explicit frames keep distinct constant values");}
    {Fixture f;f.setup();auto interrupted=f.eval();f.bindings.entering(interrupted);f.bindings.aborted(interrupted);
     f.issue(1);f.emit(f.global());need(f.run(f.eval()).rejection==Rejection::constants,
          "Abort/observation invalidation clears frame constants even on an explicit reissue");}
    {Fixture f;f.setup();f.issue(2);f.emit(f.constants());
     f.bindings.present_boundary();f.issue(2);f.emit(f.global());
     const auto r=f.run(f.eval());need(r.ready()&&r.binding.frame_index==2,"Previous Present must not erase already-published next-frame constants");}
    {Fixture f;f.setup();const auto first=f.run(f.eval());f.emit(f.global());const auto second=f.run(f.eval());
     need(first.ready()&&second.ready()&&first.binding.constants_call==second.binding.constants_call,"Common constants are per frame, not consumed by Evaluate; resources still refreshed");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);f.emit(f.constants());
     const auto r=f.bindings.returned(c);
     need(r.rejection==Rejection::stale&&r.invalidations==invalidation_constants,"Frozen constants revision change is distinct from Present invalidation");}
    {Fixture f;f.setup();f.issue(2,false);need(f.run(f.eval()).rejection==Rejection::token,"Implicit caller index is not invented");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);f.issue(2);const auto r=f.bindings.returned(c);
     need(r.rejection==Rejection::stale&&r.invalidations==invalidation_token,"Token generation change is precisely diagnosed and still refused");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);auto other=f.base(Api::tags);f.bindings.aborted(other);
     const auto r=f.bindings.returned(c);need(r.rejection==Rejection::stale&&r.invalidations==invalidation_abort,"Other call abort retained as distinct invalidation");}
    {const auto detail=invalidation_detail(invalidation_present|invalidation_constants|invalidation_adapter_coverage);
     need(std::string(detail.data())=="present-boundary|constants-revision|adapter-coverage","Multiple causes retained in bounded text");
     need(std::string(invalidation_detail(0).data())=="unspecified","Missing detail is not invented");
     need(invalidation_detail(255).back()==0,"Worst-case diagnostic remains bounded and terminated");}
    {Fixture f;
     for(unsigned frame=1;frame<=2400;++frame){f.bindings.present_boundary();f.issue(frame);f.emit(f.constants());f.emit(f.global());
         const auto r=f.run(f.eval());need(r.ready()&&r.binding.frame_index==frame&&!r.invalidations,"Long CPU token reuse retains exact current frame without old diagnostic bits");}
     f.bindings.present_boundary();f.issue(2401);f.emit(f.constants());f.emit(f.global());const auto c=f.eval();f.bindings.entering(c);
     f.bindings.hard_boundary();const auto rejected=f.bindings.returned(c);
     need(rejected.rejection==Rejection::stale&&rejected.invalidations==invalidation_present&&rejected.binding.frame_index==2401,
          "Late invalidation remains tied to exact call after 2400 good CPU transactions, not a visual/GPU test");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);c.result=sl::Result::eErrorInvalidParameter;need(!f.bindings.returned(c).ready(),"Original failure cannot execute NR");}
    {Fixture f;f.setup();auto c=f.constants();c.result=sl::Result::eErrorInvalidParameter;f.emit(c);need(!f.run(f.eval()).ready(),"Failed constants don't reuse prior success");}
    {Fixture f;f.setup();auto c=f.global();c.result=sl::Result::eErrorInvalidParameter;f.emit(c);need(!f.run(f.eval()).ready(),"Failed tags don't reuse prior success");}
    {Fixture f;f.setup();f.tags[1].resource=nullptr;f.emit(f.global());need(f.run(f.eval()).rejection==Rejection::resource,"Global revoke");}
    {Fixture f;f.setup();f.tags[1].lifecycle=sl::eOnlyValidNow;f.emit(f.global());need(f.run(f.eval()).rejection==Rejection::volatile_global,"Expired global volatile resource");}
    {Fixture f;f.setup();sl::Resource replacement(sl::ResourceType::eTex2d,reinterpret_cast<void*>(0x5000),64);
     sl::ResourceTag local(&replacement,sl::kBufferTypeDepth,sl::eOnlyValidNow,&f.guide);const sl::BaseStructure* in[]{&f.v,&local};
     auto r=f.run(f.eval(in,2));need(r.ready()&&r.binding.local[1]&&r.binding.resources[1].native==replacement.native,"Same-call local override is current, not delayed global");
     f.emit(f.constants());need(!f.run(f.eval()).ready(),"Local override did not populate global state");}
    {Fixture f;f.setup();sl::ResourceTag local(nullptr,sl::kBufferTypeDepth,sl::eValidUntilEvaluate);const sl::BaseStructure* in[]{&f.v,&local};
     need(f.run(f.eval(in,2)).rejection==Rejection::resource,"Local null shadows valid global tag");}
    {Fixture f;f.issue();f.emit(f.constants());const sl::BaseStructure* in[]{&f.v,&f.tags[0],&f.tags[1],&f.tags[2]};
     auto r=f.run(f.eval(in,4));need(r.ready()&&r.binding.local[0]&&r.binding.local[1]&&r.binding.local[2],"All-local resources require no cross-call global epoch");}
    {Fixture f;f.setup();const sl::BaseStructure* in[]{&f.v,&f.common};need(f.run(f.eval(in,2)).rejection==Rejection::metadata,"Unverified local common-constant precedence rejected");}
    // Constants are keyed by frame-token index AND viewport and must be fresh,
    // so a different source thread adds no doubt: admitted, and RECORDED.
    {Fixture f;f.setup();auto c=f.constants();c.thread=8;f.emit(c);const auto r=f.run(f.eval());
     need(r.ready(),"Constants proven by frame token and viewport are not refused for their source thread");
     need((r.cross_thread_sources&cross_thread_constants)!=0,"A cross-thread constants source is recorded, never silent");}
    // The negative that keeps the relaxation honest: a LEGACY GLOBAL tag has no
    // frame key at all, so thread identity is the only evidence it belongs to
    // this call. It stays a refusal, and is not merely recorded.
    {Fixture f;f.setup();auto c=f.global();c.thread=8;f.emit(c);const auto r=f.run(f.eval());
     need(r.rejection==Rejection::thread,"A global tag from another thread is still refused outright");
     need((r.cross_thread_sources&cross_thread_frame_tag)==0,"A refused global tag is not filed as a tolerated cross-thread source");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);c.concurrent=true;need(f.bindings.returned(c).rejection==Rejection::overlap,"Overlapping public call poisons selection");}
    {Fixture f;f.setup();auto outer=f.eval(),inner=f.eval();f.bindings.entering(outer);f.bindings.entering(inner);
     need(!f.bindings.returned(inner).ready()&&!f.bindings.returned(outer).ready(),"Nested Evaluate cannot overwrite pending snapshot");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);f.bindings.aborted(c);need(!f.bindings.returned(c).ready(),"Exception abort clears transaction");
     f.setup();need(f.run(f.eval()).ready(),"Can start fresh after no-work abort");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);c.command=reinterpret_cast<void*>(0x8888);need(f.bindings.returned(c).rejection==Rejection::mismatch,"Exact command identity");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);c.token=reinterpret_cast<void*>(0x8888);need(f.bindings.returned(c).rejection==Rejection::mismatch,"Exact token identity");}
    {Fixture f;f.setup();auto c=f.eval();c.feature=sl::kFeatureDLSS;need(!f.run(c).ready(),"SR not silently treated as selected RR profile");need(!f.run(f.eval()).ready(),"Other Evaluate expires conservative global interval");}
    {Fixture f;f.setup();auto c=f.eval();c.inputs.viewport=1;need(f.run(c).rejection==Rejection::viewport,"Unsupported viewport");}
    {Fixture f;f.setup();auto c=f.eval();c.inputs.tag_count=UINT32_MAX;need(f.run(c).rejection==Rejection::metadata,"Malformed local count bounded independently of decoder");}
    {Fixture f;f.setup();auto c=f.global();c.inputs.tag_count=UINT32_MAX;f.emit(c);need(!f.run(f.eval()).ready(),"Malformed global count invalidates tags without out-of-bounds access");}
    {Fixture f;f.setup();auto c=f.eval();f.bindings.entering(c);c.inputs.tag_count=UINT32_MAX;need(f.bindings.returned(c).rejection==Rejection::metadata,"Malformed return count invalidates frozen snapshot");}
    {Fixture f;f.setup();for(unsigned i=0;i<16;++i){f.ptr=reinterpret_cast<void*>(0x100000ULL+i);f.issue(i);}
     need(!f.run(f.eval()).ready(),"Bounded token table does not invent an evicted identity");}
    {Fixture f;f.bindings.present_boundary();f.issue();f.emit(f.constants());f.emit(f.framed());
     const auto r=f.run(f.eval());need(r.ready()&&r.binding.tag_calls[0]==3,"Frame-scoped tags pair with exact public token index");
     need(!f.run(f.eval()).ready(),"Frame tags consumed rather than reused");}
    {Fixture f;f.bindings.present_boundary();f.issue(1);f.emit(f.constants());f.emit(f.framed());
     f.issue(2);f.emit(f.constants());need(!f.run(f.eval()).ready(),"Next frame cannot borrow previous frame tags");
     f.issue(1);need(f.run(f.eval()).ready(),"Failed other-frame lookup does not consume the first frame");}
    {Fixture f;f.bindings.present_boundary();f.issue(1);f.emit(f.constants());f.emit(f.framed());
     f.resources[0].native=reinterpret_cast<void*>(0x5550);f.issue(2);f.emit(f.constants());f.emit(f.framed());
     f.issue(1);auto a=f.run(f.eval());f.issue(2);auto b=f.run(f.eval());
     need(a.ready()&&b.ready()&&a.binding.resources[0].native!=b.binding.resources[0].native,"Interleaved frames keep separate resources");}
    {Fixture f;f.bindings.present_boundary();f.issue(1);f.emit(f.constants());f.emit(f.framed());
     f.ptr=reinterpret_cast<void*>(0x9980);f.issue(1);need(f.run(f.eval()).ready(),"Same explicit frame can be reissued without reading token internals");}
    {Fixture f;f.setup();auto c=f.framed();c.token=nullptr;f.emit(c);need(!f.run(f.eval()).ready(),"Unissued frame tag cannot fall back to legacy cache");}
    {Fixture f;f.setup();f.emit(f.framed());auto c=f.framed();c.result=sl::Result::eErrorInvalidParameter;f.emit(c);
     need(!f.run(f.eval()).ready(),"Failed per-frame setter revokes cached resources");}
    {Fixture f;f.setup();f.emit(f.framed());f.bindings.present_boundary();need(!f.run(f.eval()).ready(),"Conservative Present expiry includes per-frame resources");}
    {Fixture f;f.setup();f.emit(f.framed());f.tags[1].resource=nullptr;f.emit(f.framed());need(!f.run(f.eval()).ready(),"Per-frame null revocation");}
    {Fixture f;f.setup();f.emit(f.framed());f.tags[1].lifecycle=sl::eOnlyValidNow;f.emit(f.framed());
     need(f.run(f.eval()).rejection==Rejection::volatile_global,"Frame-scoped OnlyValidNow is not durable");}
    {Fixture f;f.setup();f.emit(f.framed());f.emit(f.global());need(!f.run(f.eval()).ready(),"Mixed legacy/per-frame global APIs rejected");}
    {Fixture f;f.setup();f.emit(f.framed());auto other=f.eval();other.feature=sl::kFeatureDLSS;f.run(other);
     need(!f.run(f.eval()).ready(),"Other feature consumes conservative same-frame tags");}
    // Same rule for a PER-FRAME tag: the frame key carries the association.
    {Fixture f;f.setup();auto c=f.framed();c.thread=8;f.emit(c);const auto r=f.run(f.eval());
     need(r.ready(),"A per-frame tag keyed to this frame is not refused for its source thread");
     need((r.cross_thread_sources&cross_thread_frame_tag)!=0,"A cross-thread per-frame tag is recorded");}
    {Fixture f;f.setup();auto c=f.framed();c.inputs.viewport=1;f.emit(c);need(!f.run(f.eval()).ready(),"Wrong viewport rejected");}
    {Fixture f;f.setup();auto c=f.framed();c.inputs.tag_count=UINT32_MAX;f.emit(c);need(!f.run(f.eval()).ready(),"Per-frame count bounded");}
    {Fixture f;f.tags[1].type=sl::kBufferTypeLinearDepth;f.setup();
     need(f.run(f.eval()).rejection==Rejection::resource,"Hardware profile never treats linear depth as hardware depth");}
    for(bool framed:{false,true}){Fixture f;f.bindings.select_linear_depth_before_attach(true);f.tags[1].type=sl::kBufferTypeLinearDepth;
     f.setup();if(framed)f.emit(f.framed());auto r=f.run(f.eval());
     need(r.ready()&&r.binding.resources[1].type==sl::kBufferTypeLinearDepth,"Selected depth semantic survives global/frame binding");
     f.tags[1].type=sl::kBufferTypeDepth;f.emit(framed?f.framed():f.global());
     need(!f.run(f.eval()).ready(),"Wrong semantic cannot replenish consumed linear depth");}
    {Fixture f;f.bindings.select_linear_depth_before_attach(true);f.issue();f.emit(f.constants());f.tags[1].type=sl::kBufferTypeLinearDepth;
     const sl::BaseStructure* in[]{&f.v,&f.tags[0],&f.tags[1],&f.tags[2]};auto r=f.run(f.eval(in,4));
     need(r.ready()&&r.binding.local[1]&&r.binding.resources[1].type==sl::kBufferTypeLinearDepth,"Local linear depth retains exact type and call");
     f.tags[1].resource=nullptr;need(!f.run(f.eval(in,4)).ready(),"Local linear-depth revoke is not filled from another semantic");}
    self_configuration_and_super_resolution();
    gate_audit_relaxations();
    only_valid_now_copies();
    present_frame_attribution();

    std::cout<<"PASS binding_checks="<<checks<<" raw_files=0 game_control=false\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
