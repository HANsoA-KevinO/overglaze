// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_streamline.hpp"
#include <sl_core_api.h>
#include <MinHook.h>
#include <type_traits>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
namespace lab {
namespace {
std::atomic<slobserve::Sink*> sink{nullptr};
std::atomic<slboundary::Sink*> boundary_sink{nullptr};
std::atomic<std::uint64_t> boundary_ids{0},boundary_active{0};
// Keep detour/callback lifetime separate from the original API interval.
// A completed Evaluate's Lab receiver can wait for our prior GPU submission;
// that is not an overlapping call into Streamline. Adapter locks, exact-call
// leases and the NR admission gate still protect controlled insertion.
std::atomic<std::uint64_t> boundary_api_active{0};
std::atomic<std::uint64_t> boundary_overlap{0};
std::atomic<std::uint64_t> boundary_last_entry{0}; // (Api<<32)|thread of the latest boundary entry.
// The same bookkeeping for every call that is NOT slGetNewFrameToken, so a call
// can tell "overlapped only token fetches" from "overlapped tags/constants/Evaluate".
std::atomic<std::uint64_t> boundary_nontoken_active{0},boundary_nontoken_entries{0};
thread_local std::uint64_t boundary_parent=0;
struct Boundary;
thread_local Boundary* live_boundary=nullptr;
std::array<void*,5> installed{};
std::atomic<unsigned> export_count{0};
std::atomic<bool> token_interface{false},token_interface_faulted{false};
std::atomic<std::uint64_t> token_interface_reads{0};
// Only a POD frame here: __try cannot share a frame with C++ unwinding.
__declspec(noinline) bool token_index(const sl::FrameToken* token,std::uint32_t& out) noexcept {
    __try{out=static_cast<std::uint32_t>(*token);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
std::atomic<std::uint64_t> legacy_tag_calls{0},frame_tag_calls{0};
std::atomic<std::uint64_t> ids{0},calls{0},metadata_failures{0},active{0};
thread_local std::uint64_t evaluate_context=0;
PFun_slGetNewFrameToken* token_original=nullptr;
PFun_slEvaluateFeature* evaluate_original=nullptr;
PFun_slSetConstants* constants_original=nullptr;
PFun_slSetTag* tag_original=nullptr;
SetTagForFrame* frame_tag_original=nullptr;
// Only observer-owned reads are guarded. Original calls execute outside SEH.
bool read_bytes(void* dst,const void* src,size_t size) noexcept {
    __try {memcpy(dst,src,size);return true;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
}
template<class T> bool read(T& dst,const void* src) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);return src && read_bytes(&dst,src,sizeof(T));
}
// Status bits: fault, bound, duplicate viewport, unsupported viewport version,
// repeated/cyclic chain, invalid viewport, caller index changed during API.
void viewport_value(slobserve::Streamline& e,const sl::BaseStructure* ptr,const sl::BaseStructure& base) noexcept {
    if(base.structType!=sl::ViewportHandle::s_structType)return;
    if(++e.viewport_matches>1)e.read_status|=4;
    if(base.structVersion!=sl::kStructVersion1){e.read_status|=8;return;}
    sl::ViewportHandle value;
    if(!read(value,ptr)){e.read_status|=1;return;}
    if(value.structType!=base.structType || value.structVersion!=base.structVersion || value.next!=base.next){e.read_status|=1;return;}
    e.viewport=static_cast<uint32_t>(value);e.viewport_read=true;
    if(e.viewport==UINT_MAX)e.read_status|=32;
}
void direct_viewport(slobserve::Streamline& e,const sl::ViewportHandle& viewport) noexcept {
    sl::BaseStructure base({},0);
    if(!read(base,&viewport)){e.read_status|=1;return;}
    if(base.structType!=sl::ViewportHandle::s_structType){e.read_status|=32;return;}
    viewport_value(e,&viewport,base);
}
void input_viewport(slobserve::Streamline& e,const sl::BaseStructure** inputs,uint32_t count) noexcept {
    if(count>16){e.read_status|=2;return;}
    const sl::BaseStructure* seen[32]{};unsigned total=0;
    for(unsigned i=0;i<count;++i){const sl::BaseStructure* ptr=nullptr;
        if(!read(ptr,inputs?inputs+i:nullptr)){e.read_status|=1;return;}
        while(ptr){
            for(unsigned j=0;j<total;++j)if(seen[j]==ptr){e.read_status|=16;return;}
            if(total==32){e.read_status|=2;return;}seen[total++]=ptr;
            sl::BaseStructure base({},0);if(!read(base,ptr)){e.read_status|=1;return;}
            viewport_value(e,ptr,base);ptr=base.next;
        }
    }
}
struct Observation {
    slobserve::Sink* log=nullptr;slobserve::Event e{};std::uint64_t saved=0;bool scoped=false;
    explicit Observation(unsigned api) noexcept {
        log=sink.load();if(!log || !log->recording()){log=nullptr;return;}
        ++active;++calls;e.phase=slobserve::Event::Phase::begin;e.sl.id=++ids;
        e.sl.api=api;e.sl.parent=evaluate_context;e.thread=GetCurrentThreadId();
    }
    void begin() noexcept {
        if(!log)return;LARGE_INTEGER q{};QueryPerformanceCounter(&q);e.qpc=e.api_begin_qpc=q.QuadPart;
        log->record(e);
        if(e.sl.api==1){saved=evaluate_context;evaluate_context=e.sl.id;scoped=true;}
    }
    void finish(sl::Result result) noexcept {
        if(!log)return;LARGE_INTEGER q{};QueryPerformanceCounter(&q);e.qpc=e.api_end_qpc=q.QuadPart;
        e.phase=slobserve::Event::Phase::end;e.result=static_cast<unsigned>(result);
        if(e.sl.read_status)++metadata_failures;log->record(e);
    }
    ~Observation(){if(scoped)evaluate_context=saved;if(log)--active;}
};
struct Boundary {
    slboundary::Sink* sink=boundary_sink.load();
    slboundary::Call call{};std::uint64_t saved=boundary_parent,overlap=0;bool started=false,finished=false,api_active=false;
    std::uint64_t nontoken_prior=0,nontoken_entry=0;bool nontoken_active=false;
    bool other_nontoken() const noexcept {
        return nontoken_prior!=0||boundary_nontoken_entries.load()!=nontoken_entry||boundary_nontoken_active.load()>(nontoken_active?1u:0u);}
    Boundary* previous=live_boundary;
    slboundary::InnerCall inner_begin{};
    bool inner_began=false,inner_ended=false,inner_invalid=false,restore_seen=false,restore_entered=false;
    explicit Boundary(slboundary::Api api) noexcept {
        if(!sink)return;++boundary_active;
        const auto prior=boundary_api_active.fetch_add(1);api_active=true;
        if(prior)++boundary_overlap;overlap=boundary_overlap.load();
        call.api=api;call.id=++boundary_ids;call.parent=saved;call.thread=GetCurrentThreadId();
        if(api!=slboundary::Api::token){nontoken_prior=boundary_nontoken_active.fetch_add(1);nontoken_active=true;nontoken_entry=boundary_nontoken_entries.fetch_add(1)+1;}
        else{nontoken_prior=boundary_nontoken_active.load();nontoken_entry=boundary_nontoken_entries.load();}
        const auto previous_entry=boundary_last_entry.exchange((std::uint64_t(api)<<32)|call.thread);
        call.concurrent=prior!=0||boundary_api_active.load()>1;
        if(call.concurrent&&previous_entry){call.overlapped_api=unsigned(previous_entry>>32);call.overlapped_thread=unsigned(previous_entry&0xffffffffu);}
        if(api==slboundary::Api::evaluate){boundary_parent=call.id;live_boundary=this;}
    }
    void begin() noexcept {if(sink){started=true;call.token_only_overlap=call.concurrent&&!other_nontoken();sink->entering(call);}}
    void finish(sl::Result result) noexcept {if(sink){call.result=result;finished=true;
        call.concurrent=call.concurrent || boundary_api_active.load()>1 || overlap!=boundary_overlap.load();
        if(call.concurrent&&call.overlapped_api==UINT32_MAX){const auto latest=boundary_last_entry.load();
            if(latest&&(unsigned(latest&0xffffffffu)!=call.thread||unsigned(latest>>32)!=unsigned(call.api))){call.overlapped_api=unsigned(latest>>32);call.overlapped_thread=unsigned(latest&0xffffffffu);}}
        call.token_only_overlap=call.concurrent&&!other_nontoken();
        // All original/API-output reads are finished. Do not include the
        // receiver's post-return work in future callers' API overlap status.
        if(nontoken_active){--boundary_nontoken_active;nontoken_active=false;}
        --boundary_api_active;api_active=false;sink->returned(call);}}
    void inner(slboundary::InnerCall event) noexcept {
        call.concurrent=call.concurrent || boundary_api_active.load()>1 || overlap!=boundary_overlap.load();
        if(!event.metadata_valid || !event.command || call.concurrent || call.parent || event.result!=sl::Result::eOk)inner_invalid=true;
        if(event.phase==slboundary::InnerCall::Phase::begin){
            if(inner_began||inner_ended)inner_invalid=true;
            inner_began=true;inner_begin=event;
        }else {
            if(!inner_began || inner_ended || event.command!=inner_begin.command || event.frame!=inner_begin.frame || event.viewport!=inner_begin.viewport)inner_invalid=true;
            inner_ended=true;
        }
        event.sequence_valid=!inner_invalid;sink->inner_returned(call,event);
    }
    void restore(slboundary::RestoreCall event) noexcept {
        call.concurrent=call.concurrent || boundary_api_active.load()>1 || overlap!=boundary_overlap.load();
        event.sequence_valid=inner_began&&inner_ended&&!inner_invalid&&!restore_seen&&!call.concurrent&&!call.parent&&
            event.caller_verified&&event.command&&event.command==inner_begin.command&&event.result==0;
        restore_seen=true;sink->restore_returned(call,event);
    }
    void restore_entry(slboundary::RestoreCall event) noexcept {
        call.concurrent=call.concurrent || boundary_api_active.load()>1 || overlap!=boundary_overlap.load();
        event.sequence_valid=inner_began&&inner_ended&&!inner_invalid&&!restore_seen&&!restore_entered&&!call.concurrent&&!call.parent&&
            event.caller_verified&&event.command&&event.command==inner_begin.command;
        restore_entered=true;sink->restore_entering(call,event);
    }
    ~Boundary(){if(sink){if(api_active){--boundary_api_active;api_active=false;}
        if(nontoken_active){--boundary_nontoken_active;nontoken_active=false;}
        if(started&&!finished)sink->aborted(call);boundary_parent=saved;
        if(call.api==slboundary::Api::evaluate)live_boundary=previous;--boundary_active;}}
};
sl::Result token_hook(sl::FrameToken*& token,const uint32_t* index) {
    Boundary b(slboundary::Api::token);
    if(b.sink && index){b.call.frame_index_known=read(b.call.frame_index,index);if(!b.call.frame_index_known)b.call.inputs.issues|=slboundary::fault;}
    Observation o(0);
    if(o.log){o.e.sl.output_argument=reinterpret_cast<std::uint64_t>(&token);
        o.e.sl.index_argument=reinterpret_cast<std::uint64_t>(index);
        if(index){o.e.sl.index_read=read(o.e.sl.index,index);if(!o.e.sl.index_read)o.e.sl.read_status|=1;}}
    b.begin();o.begin();const auto r=token_original(token,index);
    if(o.log){
        if(r==sl::Result::eOk){sl::FrameToken* value=nullptr;o.e.sl.token_read=read(value,&token);
            if(o.e.sl.token_read)o.e.sl.token=reinterpret_cast<std::uint64_t>(value);else o.e.sl.read_status|=1;}
        if(index){uint32_t after=0;if(!read(after,index))o.e.sl.read_status|=1;
            else if(o.e.sl.index_read && after!=o.e.sl.index)o.e.sl.read_status|=64;}
    }
    if(b.sink && r==sl::Result::eOk && !slboundary::read_pointer(b.call.token,&token))b.call.inputs.issues|=slboundary::fault;
    if(b.sink && index){std::uint32_t after=0;
        if(!read(after,index) || !b.call.frame_index_known || after!=b.call.frame_index){b.call.frame_index_known=false;b.call.inputs.issues|=slboundary::invalid;}}
    // No caller index: Streamline numbered the frame. Ask the token itself.
    if(b.sink && !index && r==sl::Result::eOk && b.call.token && token_interface.load() && !token_interface_faulted.load()){
        std::uint32_t value=0;
        if(token_index(static_cast<const sl::FrameToken*>(b.call.token),value)){b.call.frame_index=value;b.call.frame_index_known=true;++token_interface_reads;}
        else token_interface_faulted=true; // one fault disables it for the process
    }
    o.finish(r);b.finish(r);return r;
}
sl::Result evaluate_hook(sl::Feature feature,const sl::FrameToken& token,const sl::BaseStructure** inputs,uint32_t count,sl::CommandBuffer* command) {
    Boundary b(slboundary::Api::evaluate);
    if(b.sink){b.call.token=const_cast<sl::FrameToken*>(&token);b.call.command=command;b.call.feature=feature;
        b.call.inputs=slboundary::decode_inputs(inputs,count);}
    Observation o(1);
    if(o.log){o.e.sl.token=reinterpret_cast<std::uint64_t>(&token);o.e.sl.feature=feature;
        o.e.sl.inputs=reinterpret_cast<std::uint64_t>(inputs);o.e.sl.count=count;
        o.e.command=reinterpret_cast<std::uint64_t>(command);input_viewport(o.e.sl,inputs,count);}
    b.begin();o.begin();const auto r=evaluate_original(feature,token,inputs,count,command);o.finish(r);b.finish(r);return r;
}
sl::Result constants_hook(const sl::Constants& values,const sl::FrameToken& token,const sl::ViewportHandle& viewport) {
    Boundary b(slboundary::Api::constants);
    if(b.sink){b.call.token=const_cast<sl::FrameToken*>(&token);b.call.inputs=slboundary::decode_constants(viewport,values);}
    Observation o(2);
    if(o.log){o.e.sl.token=reinterpret_cast<std::uint64_t>(&token);o.e.sl.inputs=reinterpret_cast<std::uint64_t>(&values);direct_viewport(o.e.sl,viewport);}
    b.begin();o.begin();const auto r=constants_original(values,token,viewport);o.finish(r);b.finish(r);return r;
}
sl::Result tag_hook(const sl::ViewportHandle& viewport,const sl::ResourceTag* tags,uint32_t count,sl::CommandBuffer* command) {
    Boundary b(slboundary::Api::tags);
    if(b.sink)++legacy_tag_calls;
    if(b.sink){b.call.command=command;b.call.inputs=slboundary::decode_tags(viewport,tags,count);}
    Observation o(3);
    if(o.log){o.e.sl.inputs=reinterpret_cast<std::uint64_t>(tags);o.e.sl.count=count;
        o.e.command=reinterpret_cast<std::uint64_t>(command);direct_viewport(o.e.sl,viewport);}
    b.begin();o.begin();const auto r=tag_original(viewport,tags,count,command);o.finish(r);b.finish(r);return r;
}
static_assert(std::is_same_v<decltype(&token_hook),PFun_slGetNewFrameToken*>);
static_assert(std::is_same_v<decltype(&evaluate_hook),PFun_slEvaluateFeature*>);
static_assert(std::is_same_v<decltype(&constants_hook),PFun_slSetConstants*>);
static_assert(std::is_same_v<decltype(&tag_hook),PFun_slSetTag*>);
sl::Result frame_tag_hook(const sl::FrameToken& token,const sl::ViewportHandle& viewport,const sl::ResourceTag* tags,uint32_t count,sl::CommandBuffer* command){
    Boundary b(slboundary::Api::tags);
    if(b.sink)++frame_tag_calls;
    if(b.sink){b.call.frame_scoped_tags=true;b.call.token=const_cast<sl::FrameToken*>(&token);
        b.call.command=command;b.call.inputs=slboundary::decode_tags(viewport,tags,count);}
    b.begin();const auto result=frame_tag_original(token,viewport,tags,count,command);b.finish(result);return result;
}
static_assert(std::is_same_v<decltype(&frame_tag_hook),SetTagForFrame*>);
}
std::uint64_t current_streamline_evaluate() noexcept{return evaluate_context;}
bool notify_streamline_inner(slboundary::InnerCall event) noexcept {
    auto* b=live_boundary;
    if(!b || !b->started || b->finished || b->call.feature!=sl::kFeatureDLSS_RR || boundary_sink.load()!=b->sink)return false;
    b->inner(event);return true;
}
bool notify_streamline_restore(slboundary::RestoreCall event) noexcept {
    auto* b=live_boundary;
    if(!b || !b->started || b->finished || b->call.feature!=sl::kFeatureDLSS_RR || boundary_sink.load()!=b->sink)return false;
    b->restore(event);return true;
}
bool notify_streamline_restore_entry(slboundary::RestoreCall event) noexcept {
    auto* b=live_boundary;
    if(!b || !b->started || b->finished || b->call.feature!=sl::kFeatureDLSS_RR || boundary_sink.load()!=b->sink)return false;
    b->restore_entry(event);return true;
}
static bool install(const std::array<void*,4>& targets,slobserve::Sink* log,slboundary::Sink* boundary,json& diagnostics,bool frame_tagging=false,void* extra_frame_tag=nullptr) {
    if((!log && !boundary) || (log && boundary) || sink || boundary_sink || std::any_of(installed.begin(),installed.end(),[](void* p){return p!=nullptr;}))return false;
    if(std::any_of(targets.begin(),targets.end(),[](void* p){return p==nullptr;}))return false;
    if((frame_tagging || extra_frame_tag) && log)return false;
    if(extra_frame_tag && frame_tagging)return false;
    if(extra_frame_tag && std::find(targets.begin(),targets.end(),extra_frame_tag)!=targets.end())return false;
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)if(targets[i]==targets[j])return false;
    auto status=MH_Initialize();if(status!=MH_OK && status!=MH_ERROR_ALREADY_INITIALIZED)return false;
    const unsigned count=extra_frame_tag?5u:4u;
    std::array<void*,5> entries{targets[0],targets[1],targets[2],targets[3],extra_frame_tag};
    void* detours[]={reinterpret_cast<void*>(token_hook),reinterpret_cast<void*>(evaluate_hook),reinterpret_cast<void*>(constants_hook),reinterpret_cast<void*>(tag_hook),reinterpret_cast<void*>(frame_tag_hook)};
    void** originals[]={reinterpret_cast<void**>(&token_original),reinterpret_cast<void**>(&evaluate_original),reinterpret_cast<void**>(&constants_original),reinterpret_cast<void**>(&tag_original),reinterpret_cast<void**>(&frame_tag_original)};
    const char* names[]={"slGetNewFrameToken","slEvaluateFeature","slSetConstants",frame_tagging?"slSetTagForFrame":"slSetTag","slSetTagForFrame"};
    if(frame_tagging){detours[3]=reinterpret_cast<void*>(frame_tag_hook);originals[3]=reinterpret_cast<void**>(&frame_tag_original);}
    for(unsigned i=0;i<count;++i){status=MH_CreateHook(entries[i],detours[i],originals[i]);
        diagnostics[names[i]]={{"address",reinterpret_cast<std::uint64_t>(entries[i])},{"create_hook_result",status}};
        if(status!=MH_OK){uninstall_streamline_observer();return false;}installed[i]=entries[i];}
    export_count=count;
    sink=log;boundary_sink=boundary;
    for(unsigned i=0;i<count;++i){const auto enabled=MH_EnableHook(installed[i]);diagnostics[names[i]]["enable_hook_result"]=enabled;
        if(enabled!=MH_OK){
            // Some original calls may already be in flight through a trampoline.
            // Stop admission, but never free installed code under those callers.
            // The game host/sink and verified module remain process-pinned.
            sink=nullptr;boundary_sink=nullptr;diagnostics["incomplete_install_retained"]=true;return false;
        }}
    diagnostics["header_commit"]="fbe73ba0dc817da8db877c62038d415816209139";
    diagnostics["tagging_observation"]=extra_frame_tag?"both-public-abis":frame_tagging?"frame-tags-only":"legacy-tags-only";
    diagnostics["private_frame_token_layout_read"]=false;diagnostics["mode"]=boundary?"bounded-api-boundary":"observer";return true;
}
namespace slpublic {
void* verified_export(HMODULE module,const char* name) noexcept {
    if(!module||!name)return nullptr;
    auto* p=reinterpret_cast<void*>(GetProcAddress(module,name));
    if(!p)return nullptr;
    // The same page/image contract every resolved entry point meets: the export
    // must resolve into this very module's committed executable image. A
    // forwarded, patched or data export is refused.
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(p,&info,sizeof(info))||info.State!=MEM_COMMIT||info.AllocationBase!=module||
       (info.Protect&PAGE_GUARD)||
       !(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return nullptr;
    return p;
}
PublicApi resolve_public_api(HMODULE module) noexcept {
    PublicApi a;
    a.token=verified_export(module,"slGetNewFrameToken");
    a.evaluate=verified_export(module,"slEvaluateFeature");
    a.constants=verified_export(module,"slSetConstants");
    a.legacy_tag=verified_export(module,"slSetTag");
    a.frame_tag=verified_export(module,"slSetTagForFrame");
    a.native_interface=verified_export(module,"slGetNativeInterface");
    a.feature_function=verified_export(module,"slGetFeatureFunction");
    return a;
}
json public_api_report(const PublicApi& a){
    auto at=[](void* p){return p?json(reinterpret_cast<std::uint64_t>(p)):json(nullptr);};
    return {{"resolution","public-export-table"},{"tag_abi",a.tag_abi()},{"complete",a.complete()},
        {"slGetNewFrameToken",at(a.token)},{"slEvaluateFeature",at(a.evaluate)},{"slSetConstants",at(a.constants)},
        {"slSetTag",at(a.legacy_tag)},{"slSetTagForFrame",at(a.frame_tag)},
        {"slGetNativeInterface",at(a.native_interface)},{"slGetFeatureFunction",at(a.feature_function)},
        {"scope","addresses verified to lie inside the signed interposer's executable image; not proof of the game's tagging preference"}};
}
}
bool install_streamline_observer(const std::array<void*,4>& targets,slobserve::Sink* log,json& diagnostics) {return install(targets,log,nullptr,diagnostics);}
bool install_streamline_boundary(const std::array<void*,4>& targets,slboundary::Sink* boundary,json& diagnostics,bool frame_tagging) {return install(targets,nullptr,boundary,diagnostics,frame_tagging);}
bool install_streamline_dual_tag_boundary(const std::array<void*,5>& targets,slboundary::Sink* boundary,json& diagnostics) {
    if(!targets[4])return false;
    return install({targets[0],targets[1],targets[2],targets[3]},nullptr,boundary,diagnostics,false,targets[4]);
}
void set_streamline_token_index_through_interface(bool value) noexcept {token_interface=value;}
void detach_streamline_boundary(){boundary_sink=nullptr;}
json streamline_snapshot(){return {{"version",1},{"calls",calls.load()},
    {"token_index_through_interface",{{"enabled",token_interface.load()},{"reads",token_interface_reads.load()},{"faulted",token_interface_faulted.load()}}},{"metadata_read_failures",metadata_failures.load()},
    {"boundary_calls",boundary_ids.load()},{"boundary_active_calls",boundary_active.load()},
    {"active_calls",active.load()},{"scope","public interposer exports; calls after hook installation only"},
    {"installed_export_count",export_count.load()},{"legacy_tag_calls",legacy_tag_calls.load()},{"frame_tag_calls",frame_tag_calls.load()},
    {"frame_token_virtual_called",false},{"resource_tags_captured",false},{"constants_contents_captured",false},
    {"real_frames_classified",false},{"generated_frames_classified",false}};}
void detach_streamline_observer(){sink=nullptr;}
void uninstall_streamline_observer(){for(auto p:installed)if(p)MH_DisableHook(p);sink=nullptr;boundary_sink=nullptr;
    for(auto p:installed)if(p)MH_RemoveHook(p);installed.fill(nullptr);export_count=0;}
}
