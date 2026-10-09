// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The 14 runtime entry points of the live bridge plus the variant handshake.
// Research entry points are exported only by the research bridge, from
// research/src/nr_live_research.cpp; see nr_live_extension.hpp for the seam.
#include "nr_live_core.hpp"
namespace {
using lab::live::Live;using lab::live::Frame;using lab::live::State;using lab::live::Status;
using lab::live::demand;
}
extern "C" __declspec(dllexport) bool __cdecl LabNrLiveCapabilities(lab::live::Capabilities* out){
    if(!out||out->size!=sizeof(*out)||out->abi!=lab::live::version)return false;
    lab::live::Capabilities described;lab::live::describe_live_variant(described);
    described.size=sizeof(described);described.abi=lab::live::version;*out=described;return true;
}
extern "C" __declspec(dllexport) bool __cdecl LabNrLiveStart(ID3D12CommandQueue* seed,HMODULE proxy,const wchar_t* sample,const wchar_t* data,void** out){
    if(!seed||!sample||!data||!out)return false;*out=nullptr;
    try{auto* live=new Live(seed,proxy,sample,data);*out=live;return true;}catch(...){return false;}
}
extern "C" __declspec(dllexport) bool __cdecl LabNrLiveEnableBindingPreservation(void* ctx){
    if(!ctx)return false;auto& s=*static_cast<Live*>(ctx);auto lease=s.gate.try_enter();if(!lease)return false;
    if(s.state!=State::waiting_frame||s.last_call||s.bindings)return false;
    try{s.bindings=s.submission->enable_binding_preservation();s.status.binding_preservation_enabled=1;return true;}
    catch(const std::exception& e){s.fail(e.what());return false;}catch(...){s.fail("Binding preservation installation failed");return false;}
}
extern "C" __declspec(dllexport) void __cdecl LabNrLiveGameBindingEntryV1(void* ctx,std::uint64_t call,std::uint64_t frame,ID3D12GraphicsCommandList* command){
    if(!ctx||!call)return;auto& s=*static_cast<Live*>(ctx);
    if(s.state!=State::ready||s.stopping)return;
    s.callback([&]{if(s.bindings&&s.state==State::ready){
        demand(!s.game_binding_call.load(),"Overlapping game binding entry");
        s.game_binding_thread=GetCurrentThreadId();s.game_binding_call=call;
        // Publish before snapshotting so a partially captured/throwing entry
        // still releases its pins at the matching return, even after failure.
        s.bindings->begin_game_call(call,frame,command);
    }});
}
extern "C" __declspec(dllexport) void __cdecl LabNrLiveGameBindingExitV1(void* ctx,std::uint64_t call){
    if(!ctx||!call)return;auto& s=*static_cast<Live*>(ctx);
    if(s.game_binding_call.load()!=call)return;
    if(s.game_binding_thread.load()!=GetCurrentThreadId()){
        s.asynchronous_failure="Game binding exit thread mismatch";return;
    }
    // Release entry pins even after admission/recording failure. This method
    // does not record GPU work and must not be suppressed by failed/stopping.
    const auto start=GetTickCount64();
    for(;;){auto lease=s.gate.try_enter();if(lease){
        if(s.game_binding_call.load()==call){
            if(s.bindings)s.bindings->end_game_call(call);
            s.game_binding_call=0;s.game_binding_thread=0;
        }return;}
        if(GetTickCount64()-start>=10){s.asynchronous_failure="Game binding exit admission timeout";return;}SwitchToThread();}
}
extern "C" __declspec(dllexport) bool __cdecl LabNrLivePoll(void* ctx,Status* out){
    if(!ctx||!out||out->size!=sizeof(Status)||out->abi!=lab::live::version)return false;auto& s=*static_cast<Live*>(ctx);auto lease=s.gate.try_enter();if(!lease)return false;
    try{if(s.asynchronous_failure.load())s.fail(s.asynchronous_failure.load());
        else if(s.stopping&&s.state!=State::stopped&&s.state!=State::failed){s.status.requested_on=0;
            if(s.extension)s.extension->on_stopping();
            if((!s.extension||s.extension->stop_drained())&&s.retire(false))s.finish();}
        else if(s.state==State::probing_queue){
            const auto probe=s.submission->snapshot();
            if(probe.value("terminal_fault",false)&&s.submission->retry_discarded_probe())s.state=State::waiting_frame;
            else if(probe.value("terminal_fault",false))throw std::runtime_error("Queue probe: "+probe.value("failure_reason",std::string("unknown"))+
                "; record_tid="+std::to_string(probe.value("record_thread",0u))+"; close_tid="+std::to_string(probe.value("close_thread",0u))+
                "; closes="+std::to_string(probe.value("closes",0u))+"; submits="+std::to_string(probe.value("submits",0u)));
            if(s.submission->observed_queue()){demand(s.submission->release_completed(),"Probe not fully returned");s.prepare();}
        }
        else if(s.state==State::draining)s.drain_for_rebuild();
        else if(s.state==State::ready)s.retire(false);
    }catch(const std::exception& e){s.fail(e.what());}catch(...){s.fail("Foreign live worker exception");}
    if(s.extension)s.extension->poll(s); // collector receipts live in the research status
    if(s.submission)s.status.signal_retries=s.submission->signal_retries();
    s.status.state=s.state;*out=s.status;return true;
}
extern "C" __declspec(dllexport) bool __cdecl LabNrLivePollBindings(void* ctx,lab::live::BindingStatus* out){
    if(!ctx||!out||out->size!=sizeof(*out)||out->abi!=3)return false;
    auto& s=*static_cast<Live*>(ctx);auto lease=s.gate.try_enter();if(!lease)return false;
    if(s.bindings){const auto counts=s.bindings->heap_order_stats();
        s.binding_status.heap_order_permutations=counts.permutations;
        s.binding_status.heap_permuted_insertions=counts.affected_insertions;
        const auto unknown=s.bindings->unknown_indirect();
        s.binding_status.unknown_indirect_calls=unknown.calls;s.binding_status.unknown_indirect_tolerated=unknown.tolerated?1u:0u;}
    *out=s.binding_status;return true; // No resource access or GPU scheduling.
}
// Late-attach binding policy (bit 0: tolerate unknown-layout command signatures).
// Only meaningful once binding preservation is enabled; no GPU work, no lease.
extern "C" __declspec(dllexport) bool __cdecl LabNrLiveBindingPolicyV1(void* ctx,unsigned flags){
    if(!ctx||(flags&~1u))return false;auto& s=*static_cast<Live*>(ctx);if(!s.bindings)return false;
    s.bindings->tolerate_unknown_indirect((flags&1u)!=0);return true;
}
// The unrecognized model an installation pinned with the user's opt-in (V1).
// Once, before the first frame: the model is loaded at the first preparation,
// and a pin may never change what is already loaded or widen it twice.
extern "C" __declspec(dllexport) bool __cdecl LabNrLivePinModelV1(void* ctx,const char* sha256){
    if(!ctx||!Live::sha256_text(sha256))return false;auto& s=*static_cast<Live*>(ctx);auto lease=s.gate.try_enter();if(!lease)return false;
    if(s.state!=State::waiting_frame||s.session||s.last_call||!s.pinned_model.empty())return false;
    s.pinned_model.assign(sha256,64);return true;
}
// Read-only handoff of the queue the submission router already observed and
// verified. No lease is taken: this touches neither NR state nor the GPU, and it
// must stay answerable while an Evaluate is in flight.
extern "C" __declspec(dllexport) bool __cdecl LabNrLiveRenderQueue(void* ctx,ID3D12CommandQueue** out){
    if(!ctx||!out)return false;*out=nullptr;
    auto& s=*static_cast<Live*>(ctx);if(s.stopping||!s.submission)return false;
    auto queue=s.submission->observed_queue();if(!queue)return false;
    *out=queue.Detach();return true;
}
extern "C" __declspec(dllexport) bool __cdecl LabNrLiveRequest(void* ctx,std::uint64_t revision,unsigned on){
    if(!ctx||!revision||on>2)return false;auto& s=*static_cast<Live*>(ctx);auto lease=s.gate.try_enter();if(!lease)return false;
    if(s.stopping||s.state!=State::ready||revision<=s.status.request_revision||(on&&s.diagnostics_block_on()))return false;
    s.status.request_revision=revision;s.status.requested_on=on;return true;
}
extern "C" __declspec(dllexport) bool __cdecl LabNrLiveApply(void* ctx,std::uint64_t revision,unsigned on,const lab::nr::Settings* settings){
    if(!ctx)return false;auto& s=*static_cast<Live*>(ctx);auto lease=s.gate.try_enter();if(!lease)return false;
    return lab::live::apply_request(s.status,!s.stopping&&s.state==State::ready&&(!on||!s.diagnostics_block_on()),revision,on,settings);
}
extern "C" __declspec(dllexport) bool __cdecl LabNrLiveConfigure(void* ctx,std::uint64_t revision,const lab::nr::Settings* settings){
    if(!ctx||!settings||!settings->valid()||!revision)return false;auto& s=*static_cast<Live*>(ctx);auto lease=s.gate.try_enter();if(!lease)return false;
    if(s.stopping||s.state!=State::ready||!s.status.requested_on||revision<=s.status.request_revision)return false;
    s.status.request_revision=revision;s.status.requested_settings=*settings;s.status.settings_requested_revision=revision;return true;
}
extern "C" __declspec(dllexport) void __cdecl LabNrLiveEnter(void* ctx,const Frame* frame){
    if(!ctx||!frame)return;auto& s=*static_cast<Live*>(ctx);
    s.callback([&]{s.enter_owned(*frame);});
}
extern "C" __declspec(dllexport) void __cdecl LabNrLiveBoundaryReturned(void* ctx,std::uint64_t call,unsigned ok){
    if(!ctx)return;auto& s=*static_cast<Live*>(ctx);
    s.callback([&]{s.boundary_returned_owned(call,ok);});
}
extern "C" __declspec(dllexport) void __cdecl LabNrLiveStop(void* ctx){if(ctx)static_cast<Live*>(ctx)->stopping=true;}
extern "C" __declspec(dllexport) void __cdecl LabNrLiveReject(void* ctx,const lab::RejectedCall* why){if(!ctx)return;auto& s=*static_cast<Live*>(ctx);
    s.callback([&]{if(s.extension)s.extension->on_fail();if(s.state==State::ready&&s.status.requested_on){
        if(why)s.status.rejection=*why;
        if(why&&why->disposition==lab::RejectedDisposition::constants_missing_before_nr&&
           why->call>s.last_call&&why->frame>s.last_frame&&!s.boundary_call){
            // No NR/convert/copy was recorded in this exact successful RR call.
            // Do not invalidate the earlier frame's completion ticket. Drain it
            // normally, rebuild on a NEW complete frame, and stay explicitly OFF.
            Frame skipped;skipped.call=why->call;skipped.frame=why->frame;
            s.last_call=why->call;s.last_frame=why->frame;
            s.begin_rebuild(skipped,"frame-constants-unavailable");
            s.status.bypass_frame=why->frame;s.status.observed_on=0;++s.status.off_frames;return;
        }
        // The frame index may be unknown (0) when the token cache was cleared,
        // so only our own call id must be monotonic here.
        if(why&&why->disposition==lab::RejectedDisposition::skipped_before_insertion&&
           why->call>s.last_call&&!s.boundary_call){
            // Nothing of ours was recorded in this RR call. Keep the user's ON
            // intent, skip this frame, and request an NR history reset on the
            // next admitted frame. Persistent skipping is surfaced through the
            // counters and the panel; it is never a terminal fault by itself,
            // because a skipped call leaves no partial state behind.
            ++s.status.consecutive_skips;
            s.last_call=why->call;if(why->frame>s.last_frame)s.last_frame=why->frame;s.needs_reset=true;
            ++s.status.skipped_frames;s.status.bypass_frame=why->frame;return;
        }
        char message[256]{};sprintf_s(message,"Live RR rejected while ON: %.47s / %.159s",s.status.rejection.stage,s.status.rejection.reason);
        s.fail(message);
    }});}
