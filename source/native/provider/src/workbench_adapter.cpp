// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_workbench_adapter.hpp"
#include "lab_streamline.hpp"
#include <algorithm>
#include <utility>
#include "lab_game_profile.hpp"
namespace lab {
void WorkbenchAdapter::binding_boundary(const slboundary::Call& c,diagnostic::BindingPoint point,const slboundary::InnerCall* inner)noexcept{
    if(!research_||!research_->wants_binding_boundaries()||c.api!=slboundary::Api::evaluate||c.feature!=sl::kFeatureDLSS_RR)return;
    const auto frozen=bindings_.frozen_entry(c);const auto& b=frozen.binding;
    diagnostic::BindingBoundaryEvent e;e.point=point;e.call=c.id;e.frame=b.frame_index;e.viewport=b.viewport;
    e.thread=GetCurrentThreadId();e.command=reinterpret_cast<std::uint64_t>(inner?inner->command:c.command);
    e.valid=frozen.ready()&&!c.concurrent&&!c.parent&&e.thread==b.thread;
    if(inner){e.frame=inner->frame;e.viewport=inner->viewport;
        e.valid=e.valid&&inner->metadata_valid&&inner->sequence_valid&&inner->result==sl::Result::eOk;}
    if(point==diagnostic::BindingPoint::outer_return)e.valid=e.valid&&c.result==sl::Result::eOk;
    // What the game tagged, also where the binding names a Lab copy of it.
    for(unsigned i=0;i<3;++i){e.resources[i]=reinterpret_cast<std::uint64_t>(b.resources[i].tagged_native());e.declared_states[i]=b.resources[i].tagged_state();}
    research_->binding_boundary(e);
}
void WorkbenchAdapter::verify_copies(const slboundary::Call& c,void* native_list) noexcept {
    if(!copies_.active())return; // no copy texture ever existed: nothing to verify, no lock taken
    const auto frozen=bindings_.frozen_entry(c);if(!frozen.ready())return;
    std::uint32_t roles=0;std::array<std::uint8_t,3> reasons{};
    for(unsigned i=1;i<3;++i){const auto& t=frozen.binding.resources[i];if(!t.lab_copy)continue;
        const auto why=copies_.verify(i,t,frozen.binding.tag_calls[i],native_list);
        if(why!=slboundary::CopyOutcome::none){roles|=1u<<i;reasons[i]=static_cast<std::uint8_t>(why);}}
    if(roles)bindings_.refuse_entry(c.id,roles,reasons);
}
void WorkbenchAdapter::finish_resource_call(std::uint64_t call) noexcept {
    // A different or late invocation must not complete a newer CPU lease.
    if(call)active_resource_call_.compare_exchange_strong(call,0);
}
void WorkbenchAdapter::entering(const slboundary::Call& c) noexcept {
    if(stopped_)return;
    // Research observation of every call, before the original runs and outside
    // every lock below. Null in the controller: one pointer test, nothing else.
    if(research_)research_->observe_call(c,ObservedPoint::entering);
    const auto loss_at_entry=lost_.load();bindings_.entering(c);
    if(c.api!=slboundary::Api::evaluate || !bindings_.target(c.feature))return;
    auto* const watch=c.feature==sl::kFeatureDLSS?sr_options_:options_;
    const auto options=watch?watch->freeze(c.inputs.viewport):rr::Packet{};
    slboundary::ResourceLease resources,old;
    // Self-configuring: an Evaluate handed a Streamline proxy list is checked
    // for device identity through its native list (released after the lock).
    Microsoft::WRL::ComPtr<IUnknown> native_list;
    if(self_configured_&&inspect_resources_&&c.command)if(auto* native=native_interface_.load())try{void* base=nullptr;
        if(native(c.command,&base)==sl::Result::eOk&&base){native_list.Attach(static_cast<IUnknown*>(base));
            if(native_list.Get()==c.command)native_list.Reset();}}catch(...){native_list.Reset();}
    // A Lab copy of an eOnlyValidNow tag must still be the copy taken at that
    // tag call; otherwise the call is refused before any lease is taken.
    if(inspect_resources_)verify_copies(c,native_list?static_cast<void*>(native_list.Get()):c.command);
    // This path runs only in the already verified D3D12 adapter. COM calls occur
    // outside our locks, before the original Evaluate may consume/revoke tags.
    if(inspect_resources_)try{resources=slboundary::ResourceLease::acquire(bindings_.frozen_entry(c),self_configured_,native_list.Get());}catch(...){++lost_;return;}
    auto lock=game_lock();if(!lock.owns_lock()){++lost_;return;}
    if(stopped_)return;
    if(active_resource_call_){++lost_;return;} // Never discard a live invocation on overlap.
    old=std::move(pending_resources_);pending_resources_=std::move(resources);
    pending_options_call_=c.id;pending_options_=options;pending_options_watch_=watch;
    pending_loss_epoch_=loss_at_entry;
    pending_host_contract_=slhost::read_host_contract(host_flags_address_.load());
    active_resource_call_=c.id;
    lock.unlock();
    binding_boundary(c,diagnostic::BindingPoint::outer_entry);
    if(live_)live_->game_binding_enter(bindings_.frozen_entry(c),c.command);
    if(research_&&research_->wants_access_observation())research_->observe_entry(bindings_.frozen_entry(c),c.command);
}
void WorkbenchAdapter::aborted(const slboundary::Call& c) noexcept {
    if(live_)live_->game_binding_exit(c.id);
    auto failed=c;failed.result=sl::Result::eErrorInvalidParameter;binding_boundary(failed,diagnostic::BindingPoint::outer_return);
    if(research_)research_->observe_return(c.id,false);
    if(live_)live_->aborted(c.id);
    finish_resource_call(c.id);
    bindings_.aborted(c);++lost_;
    {slboundary::ResourceLease release;
    std::unique_lock lock(mutex_,std::try_to_lock);
    if(lock.owns_lock() && pending_resources_.call()==c.id)release=std::move(pending_resources_);
    // release is destroyed after lock, never a foreign COM Release under lock.
    }
    if(research_)research_->observe_call(c,ObservedPoint::aborted);
}
void WorkbenchAdapter::present(std::uint64_t value) noexcept {
    if(stopped_ || !value)return;
    // A swapchain that changes once is a recreation (menu, resize, mode
    // switch) and keeps a single presentation source. Two chains presenting
    // alternately (A,B,A within a short window) make frame association
    // ambiguous; that state clears again after a long run on one chain.
    const auto previous=chain_.exchange(value);
    if(previous&&previous!=value){
        const auto before_previous=previous_chain_.exchange(previous);
        if(before_previous==value&&presents_since_switch_.load()<16)multiple_chains_=true;
        presents_since_switch_=0;
    }else if(++presents_since_switch_>=256&&multiple_chains_)multiple_chains_=false;
    // Which frame this Present is (frame generation, e.g. Cyberpunk 2077 DLSS-G), in the
    // game's own words. A Present declared for an earlier frame than the tags and
    // the snapshot that are live expires none of them, so it is no adapter epoch
    // either; every other Present stays the conservative boundary it always was.
    chain::PresentMarker marker;
    if(auto* m=markers_.load(std::memory_order_acquire)){LARGE_INTEGER q{};QueryPerformanceCounter(&q);
        marker=m->at_present(GetCurrentThreadId(),static_cast<std::uint64_t>(q.QuadPart));}
    const bool spared=marker.declares_frame()&&bindings_.present_boundary_of_frame(marker.frame);
    if(!marker.declares_frame())bindings_.present_boundary();
    ++presents_;if(!spared)++epoch_;copies_.present();
}
void WorkbenchAdapter::inner_returned(const slboundary::Call& c,const slboundary::InnerCall& i) noexcept {
    if(!stopped_)binding_boundary(c,i.phase==slboundary::InnerCall::Phase::begin?diagnostic::BindingPoint::rr_begin_return:diagnostic::BindingPoint::rr_end_return,&i);
    if(stopped_ || i.phase!=slboundary::InnerCall::Phase::end)return;
    const auto r=bindings_.frozen_entry(c);
    InnerLatest v;v.call=c.id;v.frame=i.frame;v.viewport=i.viewport;v.sequence=i.sequence_valid;
    v.metadata=r.ready()&&!c.concurrent&&!c.parent&&i.metadata_valid&&i.sequence_valid&&i.result==sl::Result::eOk&&
        r.binding.frame_index==i.frame&&r.binding.viewport==i.viewport&&r.binding.thread==GetCurrentThreadId();
    v.reason=v.metadata?"native-command-unverified":"inner-call-mismatch";
    if(v.metadata){
        v.native_command=c.command==i.command;
        v.public_command_native=v.native_command;
        if(!v.native_command)if(auto native=native_interface_.load())try{
            void* base=nullptr;
            if(native(c.command,&base)==sl::Result::eOk&&base){v.native_command=base==i.command;static_cast<IUnknown*>(base)->Release();}
        }catch(...){v.reason="native-interface-call-failed";}
        if(v.native_command)v.reason="paired-inner-return-before-common-restore";
    }
    auto lock=game_lock();if(!lock.owns_lock()){++lost_;return;}
    if(stopped_)return;
    if(active_resource_call_!=c.id || !pending_resources_.held() || !pending_resources_.facts().inspected ||
       !pending_resources_.facts().same_device || pending_options_call_!=c.id ||
       !pending_options_.valid() || pending_options_.viewport!=i.viewport || !options_ ||
       !options_->unchanged(pending_options_) || lost_!=pending_loss_epoch_ || chains_ambiguous()){
        v.metadata=false;v.reason="entry-lease-options-or-coverage-missing";}
    inner_latest_=v;
    if(v.metadata&&v.native_command)++inner_candidates_;else ++inner_rejected_;
}
// Public metadata of an SR (kFeatureDLSS) Evaluate. Nothing here freezes tags,
// takes the resource lease, touches the RR pending state or calls the live
// receiver: it only records that the game upscales without RR, and with which
// tags and extents, so the next route can be designed on observed facts.
// Most recent public tag call, kept so an SR-only game can be described. It is
// the LAST tag call seen, never a proven same-call frozen binding: only the RR
// path freezes tags per invocation.
void WorkbenchAdapter::note_tag_call(const slboundary::Call& c) noexcept {
    TagsSeen t;t.call=c.id;t.thread=c.thread;
    t.call_issues=c.inputs.call_issues;t.all_issues=c.inputs.issues;t.result=static_cast<std::uint32_t>(c.result);
    t.viewport=c.inputs.viewport_present?c.inputs.viewport:UINT32_MAX;
    t.count=std::min<std::uint32_t>(c.inputs.tag_count,static_cast<std::uint32_t>(t.types.size()));
    for(std::uint32_t j=0;j<t.count;++j){const auto& g=c.inputs.tags[j];
        t.types[j]=g.type;t.widths[j]=g.extent.width;t.heights[j]=g.extent.height;t.states[j]=g.state;
        t.issues[j]=g.issues;t.resource_types[j]=static_cast<std::uint32_t>(g.resource_type);t.lifecycles[j]=static_cast<std::uint32_t>(g.lifecycle);
        if(g.null_resource)t.null_mask|=1u<<j;}
    std::lock_guard lock(upscaler_mutex_);tags_=t;
}
void WorkbenchAdapter::note_upscaler_call(const slboundary::Call& c) noexcept {
    UpscalerSeen v;
    v.call=c.id;v.frame=c.frame_index;v.frame_known=c.frame_index_known;v.thread=c.thread;
    v.viewport=c.inputs.viewport_present?c.inputs.viewport:UINT32_MAX;
    v.constants_present=c.inputs.constants_present;v.input_issues=c.inputs.issues;
    v.command_present=c.command!=nullptr;v.result_ok=c.result==sl::Result::eOk&&!c.concurrent&&!c.parent;
    v.inline_tag_count=c.inputs.tag_count;
    std::lock_guard lock(upscaler_mutex_);
    v.evaluates=upscaler_.evaluates+1;v.succeeded=upscaler_.succeeded+(v.result_ok?1u:0u);
    v.tags=tags_;v.tags_same_thread=tags_.call!=0&&tags_.thread==c.thread;
    upscaler_=v;
}
void WorkbenchAdapter::restore_entering(const slboundary::Call& c,const slboundary::RestoreCall& r) noexcept {
    // Retained as an observation boundary. Controlled rendering does not
    // depend on whether common takes this optional branch.
    (void)c;(void)r;
}
void WorkbenchAdapter::restore_returned(const slboundary::Call& c,const slboundary::RestoreCall& r) noexcept {
    if(stopped_)return;
    auto lock=game_lock();if(!lock.owns_lock()){++lost_;return;}
    if(stopped_||inner_latest_.call!=c.id)return;
    auto& v=inner_latest_;v.restore_result=r.result;
    v.restore_matched=r.sequence_valid&&r.caller_verified&&r.result==0&&v.metadata&&v.native_command&&
        active_resource_call_==c.id&&pending_resources_.held()&&pending_options_call_==c.id&&
        options_&&options_->unchanged(pending_options_)&&lost_==pending_loss_epoch_&&!chains_ambiguous();
}
void WorkbenchAdapter::returned(const slboundary::Call& c) noexcept {
    // Declared first, so it runs LAST on every exit: the research observer sees
    // the return only after any controlled insertion has finished with the call
    // (live_->enter below runs synchronously inside this function).
    struct ObserveReturned {IResearchObserver* research;const slboundary::Call& call;
        ~ObserveReturned(){if(research)research->observe_call(call,ObservedPoint::returned);}} observe_returned{research_,c};
    struct EndGameBindings {SlAdmissionSink* live;std::uint64_t call;~EndGameBindings(){if(live)live->game_binding_exit(call);}}
        game_bindings{c.api==slboundary::Api::evaluate&&bindings_.target(c.feature)?live_:nullptr,c.id};
    // The marker observer learns token address -> frame index from every successful
    // slGetNewFrameToken return, the binding's own source (once a function is hooked).
    if(c.api==slboundary::Api::token&&c.result==sl::Result::eOk&&c.token)
        if(auto* m=markers_.load(std::memory_order_acquire);m&&m->functions())m->token_returned(c.token,c.frame_index,c.frame_index_known);
    if(!stopped_)binding_boundary(c,diagnostic::BindingPoint::outer_return);
    // Close the read-only window before any controlled insertion can execute.
    if(research_&&c.api==slboundary::Api::evaluate&&c.feature==sl::kFeatureDLSS_RR)
        research_->observe_return(c.id,c.result==sl::Result::eOk&&!c.concurrent&&!c.parent);
    if(stopped_){slboundary::ResourceLease release;
        std::unique_lock lock(mutex_,std::try_to_lock);
        finish_resource_call(c.id);
        if(lock.owns_lock() && pending_resources_.call()==c.id)release=std::move(pending_resources_);
        return;}
    const auto api=static_cast<unsigned>(c.api);if(api<calls_.size())++calls_[api];
    // A tag call whose eOnlyValidNow depth/motion were copied right here, on
    // its own list, is bound as the copy; the research observer and the public
    // tag record above keep seeing what the game itself passed.
    std::optional<slboundary::Call> copied;
    const bool substituted=inspect_resources_&&c.api==slboundary::Api::tags&&copies_.tag_returned(c,bindings_,native_interface_.load(),copied);
    const auto before=epoch_.load();const auto r=bindings_.returned(substituted?*copied:c);
    if(c.api==slboundary::Api::tags)note_tag_call(c);
    if(c.api==slboundary::Api::evaluate && c.feature==sl::kFeatureDLSS)note_upscaler_call(c);
    if(c.api!=slboundary::Api::evaluate || !bindings_.target(c.feature))return;
    RejectedCall why;why.call=c.id;why.frame=r.binding.frame_index;
    strcpy_s(why.stage,"outer-admission");strcpy_s(why.reason,"metadata-lock-contention");
    // Every refusal from here on follows the game's successful (or failed)
    // original return with nothing of ours recorded: a skip that keeps ON and
    // resets history; the runtime's skip budget still ends a run of them. Only
    // a nested call stays terminal. The pinned profile keeps its constants
    // bypass below. (It used to stay terminal here: one lock contention with
    // the status snapshot stopped NR in 2077.)
    if(!c.parent)why.disposition=RejectedDisposition::skipped_before_insertion;
    auto explain=[&](const char* stage,const char* reason){strncpy_s(why.stage,stage,_TRUNCATE);strncpy_s(why.reason,reason,_TRUNCATE);};
    bool admitted=false;struct RejectIncomplete {SlAdmissionSink* live;bool& admitted;const RejectedCall& why;
        ~RejectIncomplete(){if(live&&!admitted)live->unavailable(why);}} reject{live_,admitted,why};
    // Every exit: the copier learns whether the Lab copies this call bound were
    // used. A run refused for another reason backs it off.
    struct CopyVerdict {slboundary::VolatileCopies& copies;const slboundary::Resolution& r;const bool& admitted;
        ~CopyVerdict(){copies.evaluated(r,admitted);}} copy_verdict{copies_,r,admitted};
    slboundary::ResourceLease release;
    auto lock=game_lock();
    finish_resource_call(c.id);
    if(!lock.owns_lock()){++lost_;return;}
    if(pending_resources_.call()==c.id)release=std::move(pending_resources_);
    if(stopped_)return;
    const bool coverage_complete=pending_options_call_==c.id&&lost_==pending_loss_epoch_;
    if(inner_latest_.call==c.id)inner_latest_.outer_succeeded=c.result==sl::Result::eOk&&!c.concurrent&&coverage_complete&&r.ready();
    latest_={};auto& v=latest_;v.call=c.id;v.feature=c.feature;v.epoch=before;v.loss_epoch=pending_loss_epoch_;v.reason=r.rejection;
    v.local_tag_count=std::min<unsigned>(c.inputs.tag_count,16);v.input_issues=c.inputs.issues;
    // What the game asked for, recorded whatever the verdict, so a viewport
    // refusal can be read instead of guessed at.
    v.observed_viewport=c.inputs.viewport;v.observed_viewport_present=c.inputs.viewport_present;
    for(unsigned j=0;j<v.local_tag_count;++j){v.local_tag_types[j]=c.inputs.tags[j].type;v.local_tag_issues[j]=c.inputs.tags[j].issues;
        if(c.inputs.tags[j].null_resource)v.local_null_mask|=1u<<j;}
    v.resources=release.facts();v.resources.matched_return=release.matches(r);
    if(pending_options_call_==c.id){v.options=pending_options_;
        if(v.options.valid() && (!pending_options_watch_ || !pending_options_watch_->unchanged(v.options)))v.options.issue=rr::Issue::changed;}
    if(pending_options_call_==c.id)pending_options_call_=0;
    std::uint32_t adapter_invalidations=0;
    if(before!=epoch_.load())adapter_invalidations|=slboundary::invalidation_adapter_epoch;
    if(chains_ambiguous())adapter_invalidations|=slboundary::invalidation_multiple_swapchains;
    if(!coverage_complete)adapter_invalidations|=slboundary::invalidation_adapter_coverage;
    v.invalidations=r.invalidations|adapter_invalidations;v.presents_during_call=r.presents_during_call;
    v.cross_thread_sources=r.cross_thread_sources;
    if(adapter_invalidations)v.reason=slboundary::Rejection::stale;
    if(v.reason!=slboundary::Rejection::none){explain("frozen-binding",slboundary::rejection_name(v.reason));
        if(v.reason==slboundary::Rejection::overlap){const auto detail=slboundary::overlap_detail(r.overlap_sources);
            const char* other=c.overlapped_api==0?"token":c.overlapped_api==1?"constants":c.overlapped_api==2?"tags":c.overlapped_api==3?"evaluate":"unknown";
            snprintf(why.reason,sizeof(why.reason),"overlapping-api-call; %.100s; other=%s@%u",detail.data(),other,c.overlapped_thread);}
        if(v.reason==slboundary::Rejection::stale){const auto detail=slboundary::invalidation_detail(v.invalidations);
            snprintf(why.reason,sizeof(why.reason),"snapshot-invalidated; %.137s",detail.data());}
        // OnlyValidNow: which roles, and per role why no Lab copy was bound.
        if(v.reason==slboundary::Rejection::volatile_global||v.reason==slboundary::Rejection::volatile_copy){
            char roles[128]{};int used=0;
            for(unsigned i=0;i<3&&used>=0&&used<int(sizeof(roles));++i)if(r.volatile_roles&(1u<<i)){
                auto code=static_cast<slboundary::CopyOutcome>(r.volatile_reasons[i]);
                if(code==slboundary::CopyOutcome::none&&!copies_.wanted())code=slboundary::CopyOutcome::not_wanted;
                used+=snprintf(roles+used,sizeof(roles)-used,"%s%s=%s",used?",":"",i==0?"output":i==1?"depth":"motion",slboundary::copy_outcome_name(code));}
            snprintf(why.reason,sizeof(why.reason),"%s; %.125s",slboundary::rejection_name(v.reason),roles);}
        // Self-configuring, missing constants are one skipped frame, not the
        // rebuild-and-stay-OFF bypass: the next frame usually has them.
        if(v.reason==slboundary::Rejection::constants&&r.rejection==v.reason&&coverage_complete&&!self_configured_&&
           before==epoch_.load()&&!multiple_chains_&&!c.parent&&!conflicting(c)&&c.result==sl::Result::eOk)
            why.disposition=RejectedDisposition::constants_missing_before_nr;
        // Nothing of ours was recorded for this call, so whatever the cause
        // (concurrent SL call, token/tag cache cleared, lost coverage, a failed
        // outer RR call), skipping leaves no partial state. Only nested calls
        // stay terminal; constants keep their own bypass path.
        if(v.reason!=slboundary::Rejection::constants&&!c.parent&&!(r.overlap_sources&slboundary::overlap_nested_call))
            why.disposition=RejectedDisposition::skipped_before_insertion;
        if(r.overlap_sources&slboundary::overlap_nested_call)why.disposition=RejectedDisposition::terminal;
        if(v.reason==slboundary::Rejection::constants)snprintf(why.reason,sizeof(why.reason),"%s; %s; setter=%llu; token-generation=%llu",
            slboundary::rejection_name(v.reason),slboundary::constants_state_name(r.binding.constants_state),
            static_cast<unsigned long long>(r.binding.constants_call),static_cast<unsigned long long>(r.binding.token_generation));
        ++rejected_;return;}
    ++candidates_;const auto& b=r.binding;v.token_generation=b.token_generation;v.constants_call=b.constants_call;
    v.frame=b.frame_index;v.viewport=b.viewport;v.thread=b.thread;v.tag_calls=b.tag_calls;v.local=b.local;
    // Declared states are the game's; a role bound as a Lab copy is marked.
    for(unsigned i=0;i<3;++i){v.resource_types[i]=b.resources[i].type;v.states[i]=b.resources[i].tagged_state();v.width[i]=b.resources[i].extent.width;v.height[i]=b.resources[i].extent.height;
        if(b.resources[i].lab_copy)v.lab_copies|=1u<<i;}
    v.constants=b.constants;
    v.constants_copied=true;v.mvec_x=b.constants.mvecScale.x;v.mvec_y=b.constants.mvecScale.y;
    v.jitter_x=b.constants.jitterOffset.x;v.jitter_y=b.constants.jitterOffset.y;
    const auto host=slhost::read_host_contract(host_flags_address_.load());
    // Modern SL's public Evaluate contract assigns restoration to its caller.
    // Apply only to a reviewed game/common pair AND this matching native (not
    // unwrapped proxy) invocation. Neither absent restore calls nor flags=200
    // alone grant admission. This is an API obligation, not observed rebind.
    const bool native_contract=native_evaluate_contract_&&host.readable&&inner_latest_.call==c.id&&
        inner_latest_.metadata&&inner_latest_.public_command_native;
    const bool rebind_contract=(host.host_rebind_required()||native_contract)&&host==pending_host_contract_;
    // Per feature. The reviewed private evidence -- begin/end inner
    // callbacks and the common restore -- exists for RR only, so it is required
    // for RR only. A super-resolution call, a target only where the host chose
    // it, is admitted on the public proof below: exactly the rule every call
    // follows on a host that has no reviewed profile at all. On the controller
    // inner_admission_required_ is already false, so nothing changes there.
    const bool inner_required=c.feature==sl::kFeatureDLSS_RR&&inner_admission_required_.load(std::memory_order_acquire);
    // Evidence every admission needs, all of it from the public boundary. The
    // frozen binding is already known clean (v.reason==none) at this point.
    const bool shared_proof=live_&&!c.parent&&coverage_complete&&!chains_ambiguous()&&release.held()&&
        v.resources.inspected&&v.resources.same_device&&v.resources.matched_return&&
        v.options.valid()&&v.options.viewport==b.viewport;
    // Public proof: the plugin-internal native-command cross-check is
    // unavailable without a reviewed profile, so verify publicly that the
    // buffer the game handed to Evaluate IS the native D3D12 list. A
    // Streamline proxy unwraps to a different pointer and is refused (skipped,
    // never inserted into), rather than failing downstream.
    bool command_native=false,command_unwrapped=false;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> unwrapped; // held until the frame is handed over
    if(!inner_required&&shared_proof&&c.command&&c.result==sl::Result::eOk&&!conflicting(c)&&r.ready()){
        if(auto* native=native_interface_.load()){
            void* base=nullptr;
            try{if(native(c.command,&base)==sl::Result::eOk&&base){
                Microsoft::WRL::ComPtr<IUnknown> held;held.Attach(static_cast<IUnknown*>(base)); // slGetNativeInterface AddRefs
                command_native=base==c.command;
                // Self-configuring: a game that records through Streamline's
                // proxy device hands Evaluate a proxy list. Its native list is
                // admitted instead when it is a DIRECT D3D12 list on the device
                // of the tagged resources; NR is recorded on that list, which is
                // where every proxy call lands anyway, and the insertion point
                // (after Evaluate returns, before the caller's rebind) is the
                // same. The runtime is handed the native list, never the proxy.
                if(!command_native&&self_configured_){
                    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;Microsoft::WRL::ComPtr<ID3D12Device> list_device,resource_device;
                    Microsoft::WRL::ComPtr<ID3D12Resource> resource;Microsoft::WRL::ComPtr<IUnknown> a,bdev;
                    if(SUCCEEDED(held.As(&list))&&list->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT&&SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&list_device)))&&
                       b.resources[0].native&&SUCCEEDED(static_cast<IUnknown*>(b.resources[0].native)->QueryInterface(IID_PPV_ARGS(&resource)))&&
                       SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&resource_device)))&&SUCCEEDED(list_device.As(&a))&&SUCCEEDED(resource_device.As(&bdev))&&a.Get()==bdev.Get()){
                        unwrapped=list;command_unwrapped=true;}
                }}}
            catch(...){command_native=command_unwrapped=false;unwrapped.Reset();}
        }
    }
    const bool private_proof=rebind_contract&&inner_latest_.call==c.id&&inner_latest_.metadata&&
        inner_latest_.native_command&&inner_latest_.outer_succeeded;
    const bool public_proof=c.result==sl::Result::eOk&&!conflicting(c)&&r.ready()&&(command_native||command_unwrapped);
    const bool complete=shared_proof&&(inner_required?private_proof:public_proof);
    const auto options=v.options;
    if(inner_required&&!rebind_contract)explain("host-contract","host-rebind-contract-missing-or-changed");
    else if(!coverage_complete||chains_ambiguous())explain("call-coverage",!coverage_complete?"callback-loss":"multiple-swapchains");
    else if(!v.options.valid())explain(c.feature==sl::kFeatureDLSS?"SR-options":"RR-options",rr::name(v.options.issue));
    else if(v.options.viewport!=b.viewport)explain(c.feature==sl::kFeatureDLSS?"SR-options":"RR-options","viewport-mismatch");
    else if(inner_required&&inner_latest_.call!=c.id)explain("inner-callback","matching-inner-return-missing");
    else if(inner_required&&(!inner_latest_.metadata||!inner_latest_.native_command))explain("inner-callback",inner_latest_.reason);
    else if(inner_required&&(!inner_latest_.outer_succeeded||c.parent))explain("outer-call","failed-or-nested-outer-call");
    else if(!inner_required&&(c.result!=sl::Result::eOk||conflicting(c)||c.parent||!r.ready()))explain("outer-call","failed-or-nested-outer-call");
    else if(!release.held()||!v.resources.inspected||!v.resources.same_device||!v.resources.matched_return)explain("resource-lease","entry-resource-lease-incomplete-or-mismatched");
    else if(!inner_required&&!command_native&&!command_unwrapped)explain("public-command",self_configured_
        ?"evaluate-command-buffer-does-not-unwrap-to-a-native-direct-d3d12-list-on-the-resources-device"
        :"evaluate-command-buffer-is-not-the-native-d3d12-list");
    else if(!complete)explain("outer-admission","no-live-receiver");
    if(complete){strcpy_s(v.admission_stage,"admitted");v.admission_reason[0]=0;++admitted_calls_;}
    else{strncpy_s(v.admission_stage,why.stage,_TRUNCATE);strncpy_s(v.admission_reason,why.reason,_TRUNCATE);}
    v.command_kind=command_native?0u:command_unwrapped?1u:2u;
    lock.unlock();
    if(complete){
        // Still inside the outer interposer detour, BEFORE the caller resumes
        // and performs its contracted state rebind. Entry COM references remain
        // alive here; the renderer separately retains GPU-lifetime references.
        admitted=true;live_->enter(r,options,command_unwrapped?static_cast<void*>(unwrapped.Get()):c.command);live_->boundary_returned(c.id,true);
    }
}
json WorkbenchAdapter::snapshot() {
    Latest v;InnerLatest inner;bool held=false,current=false;std::uint64_t candidates=0,rejected=0,inner_candidates=0,inner_rejected=0;
    slboundary::ResourceLease release;
    {
        std::lock_guard lock(mutex_);
        if(!active_resource_call_)release=std::move(pending_resources_);
        v=latest_;held=pending_resources_.held();candidates=candidates_;rejected=rejected_;
        inner=inner_latest_;inner_candidates=inner_candidates_;inner_rejected=inner_rejected_;
        current=!stopped_ && !multiple_chains_ && v.loss_epoch==lost_ && v.call && v.epoch==epoch_ && v.reason==slboundary::Rejection::none;
    }
    // No allocation/JSON serialization or foreign COM Release under mutex_.
    release={};
    UpscalerSeen sr;TagsSeen tagged;{std::lock_guard lock(upscaler_mutex_);sr=upscaler_;tagged=tags_;}
    // Most recent public tag call from ANY feature, exposed regardless of SR so
    // that a first observation launch can read the game's viewport and tag
    // types (hardware vs linear depth, output colour extent) even while every
    // RR call is still rejected by a default-hypothesis profile.
    json last_tags=nullptr;
    if(tagged.call){
        last_tags={{"call",tagged.call},{"thread",tagged.thread},{"viewport",tagged.viewport==UINT32_MAX?json(nullptr):json(tagged.viewport)},
            {"result",tagged.result},{"call_issues",tagged.call_issues},{"issues",tagged.all_issues},
            {"tags",json::array()},{"tags_columns",{"type","width","height","state","null","issues","resource_type","lifecycle"}},
            {"scope","most recent public tag call, NOT a same-call frozen binding; issues bits: 1 fault, 2 bound, 4 duplicate, 8 version, 32 invalid, 64 extension"}};
        for(std::uint32_t j=0;j<tagged.count;++j)
            last_tags["tags"].push_back({tagged.types[j],tagged.widths[j],tagged.heights[j],tagged.states[j],bool(tagged.null_mask&(1u<<j)),
                tagged.issues[j],tagged.resource_types[j],tagged.lifecycles[j]});
    }
    json upscaler=nullptr;
    if(sr.evaluates){
        upscaler={{"feature","DLSS-SR"},{"evaluates",sr.evaluates},{"succeeded",sr.succeeded},{"call",sr.call},
            {"frame",sr.frame_known?json(sr.frame):json(nullptr)},{"thread",sr.thread},
            {"viewport",sr.viewport==UINT32_MAX?json(nullptr):json(sr.viewport)},
            {"constants_present",sr.constants_present},{"command_present",sr.command_present},
            {"inline_tag_count",sr.inline_tag_count},{"input_issues",sr.input_issues},
            {"last_tag_call",sr.tags.call?json{{"call",sr.tags.call},{"thread",sr.tags.thread},{"same_thread",sr.tags_same_thread},
                {"viewport",sr.tags.viewport==UINT32_MAX?json(nullptr):json(sr.tags.viewport)},
                {"tags",json::array()},{"tags_columns",{"type","width","height","state","null"}},
                {"scope","most recent public tag call, NOT a same-call frozen binding"}}:json(nullptr)},
            {"scope","public-boundary observation only; NR is inserted after RR and never after this call"}};
        for(std::uint32_t j=0;j<sr.tags.count;++j)
            upscaler["last_tag_call"]["tags"].push_back({sr.tags.types[j],sr.tags.widths[j],sr.tags.heights[j],sr.tags.states[j],bool(sr.tags.null_mask&(1u<<j))});
    }
    json source=nullptr;if(v.call)source={{"call",v.call},{"feature",v.feature==sl::kFeatureDLSS?"sr":"rr"},
        {"admission",{{"stage",v.admission_stage[0]?json(std::string(v.admission_stage)):json(nullptr)},
            {"reason",v.admission_reason[0]?json(std::string(v.admission_reason)):json(nullptr)},
            {"command",v.command_kind==0?"native":v.command_kind==1?"sl-proxy-unwrapped":"not-established"}}},{"binding_result",slboundary::rejection_name(v.reason)},{"common_constants_copied",v.constants_copied}};
    if(v.call){source["invalidation_sources"]=v.invalidations;
        source["presents_during_call"]=v.presents_during_call;
        source["presents_during_call_scope"]="Presents observed between entry and return; tolerated for inline eValidUntilEvaluate tags and for a Present the game's own marker declares for an earlier frame";
        // A cross-thread engine must be visible, not inferred from its absence.
        source["cross_thread_sources"]=std::string(slboundary::cross_thread_detail(v.cross_thread_sources).data());
        source["cross_thread_scope"]="inputs whose setter ran on another thread; admitted only because frame-token index and viewport already prove the association, and never for a legacy global tag";
        source["invalidation_detail"]=v.invalidations?json(slboundary::invalidation_detail(v.invalidations).data()):json(nullptr);
        source["input_issues"]=v.input_issues;source["local_tags"]=json::array();
        for(unsigned j=0;j<v.local_tag_count;++j)source["local_tags"].push_back({v.local_tag_types[j],v.local_tag_issues[j],bool(v.local_null_mask&(1u<<j))});
        source["local_tags_columns"]={"type","issues","null"};
        // What the game asked for versus what this adapter package admits. Equal
        // values are the only ones admitted; this pair exists so a mismatch can
        // be read off instead of guessed at.
        if(self_configured_){const auto s=viewport_selection();
            source["viewport"]={{"observed",v.observed_viewport},{"observed_present",v.observed_viewport_present},
                {"admitted",s.locked?json(s.viewport):json(nullptr)},{"self_configured",true},{"switches",s.switches},
                {"other_viewport_calls",s.other_viewport_calls},
                {"scope","self-configured: the first RR viewport is admitted; it moves only after consecutive RR calls on another one"}};
        }else
        source["viewport"]={{"observed",v.observed_viewport},{"observed_present",v.observed_viewport_present},
            {"admitted",selected_viewport()},
            {"scope","the adapter package's facts fix the admitted viewport; an observation is not permission to change it"}};}
    // Missing metadata is absent, not zero dimensions/states/identity.
    if(v.call && v.reason==slboundary::Rejection::none)source.update({{"frame_index",v.frame},
        {"token_generation",v.token_generation},{"constants_call",v.constants_call},{"tag_calls",v.tag_calls},
        {"viewport",v.viewport},{"thread",v.thread},{"local",v.local},{"declared_states",v.states},
        {"declared_widths",v.width},{"declared_heights",v.height},{"resource_tag_types",v.resource_types},
        {"mvec_scale",json::array({v.mvec_x,v.mvec_y})},{"jitter",json::array({v.jitter_x,v.jitter_y})}});
    // Only where it happened (the snapshot bound is shared): roles bound as Lab
    // copies; declared_states above stay the game's own.
    if(v.call&&v.reason==slboundary::Rejection::none&&v.lab_copies){json roles=json::array();
        if(v.lab_copies&2u)roles.push_back("depth");if(v.lab_copies&4u)roles.push_back("motion");source["lab_copy_roles"]=roles;}
    if(v.constants_copied){const auto& c=v.constants;json projection=json::array();
        for(unsigned j=0;j<4;++j){const auto& row=c.cameraViewToClip[j];projection.push_back({row.x,row.y,row.z,row.w});}
        source["guide_constants"]={{"camera_view_to_clip",projection},{"camera_near",c.cameraNear},{"camera_far",c.cameraFar},
            {"depth_inverted",int(c.depthInverted)},{"orthographic",int(c.orthographicProjection)},
            {"camera_motion_included",int(c.cameraMotionIncluded)},{"motion_3d",int(c.motionVectors3D)},
            {"motion_jittered",int(c.motionVectorsJittered)},{"motion_dilated",int(c.motionVectorsDilated)},{"reset",int(c.reset)},
            {"scope","copied-exact-frame-public-constants-not-verified-pixel-meaning"}};}
    return {{"version",1},{"mode","admission-only"},{"scope","this-lab-backend-not-other-game-integrations"},{"game_control_available",false},{"nr_executed",false},
        {"texture_reads",0},{"event_files",0},{"gpu_completion_connected",false},{"stopped",stopped_.load()},
        {"sr_target",bindings_.targets_super_resolution()},
        {"binding_stats",[&]{const auto st=bindings_.stats();json rej=json::object();
            for(unsigned i=0;i<16;++i)if(st.rejections[i])rej[slboundary::rejection_name(static_cast<slboundary::Rejection>(i))]=st.rejections[i];
            const char* apis[]{"token","constants","tags","evaluate","unknown"};json with=json::object();
            for(unsigned i=0;i<5;++i)if(st.overlap_with[i])with[apis[i]]=st.overlap_with[i];
            json roles=json::array(),causes=json::array();
            for(unsigned i=0;i<3;++i)if(st.resource_roles&(1u<<i))roles.push_back(i==0?"output-colour":i==1?"depth":"motion");
            if(st.resource_causes&1u)causes.push_back("never-tagged");if(st.resource_causes&2u)causes.push_back("consumed-or-expired");
            if(st.resource_causes&4u)causes.push_back("invalid-tag");
            // role -> source -> lifecycle -> Evaluates, nonzero only: which roles a
            // game declares OnlyValidNow is read here, per Evaluate, not per tag call.
            const char* role_names[]{"output-colour","depth","motion"};const char* sources[]{"inline","frame","global"};
            const char* lifecycles[]{"only-valid-now","until-present","until-evaluate"};json by_role=json::object();
            for(unsigned i=0;i<3;++i){json role=json::object();
                for(unsigned s=0;s<3;++s){json by_life=json::object();
                    for(unsigned l=0;l<3;++l)if(const auto n=st.role_lifecycles[i*10+s*3+l])by_life[lifecycles[l]]=n;
                    if(!by_life.empty())role[sources[s]]=by_life;}
                if(const auto n=st.role_lifecycles[i*10+9])role["untagged"]=n;
                if(!role.empty())by_role[role_names[i]]=role;}
            json last_volatile=nullptr;
            if(st.volatile_roles){last_volatile=json::object();
                for(unsigned i=0;i<3;++i)if(st.volatile_roles&(1u<<i))last_volatile[role_names[i]]=slboundary::copy_outcome_name(static_cast<slboundary::CopyOutcome>(st.volatile_reasons[i]));}
            json out{{"outcomes",rej},{"overlap_with",with},{"benign_token_overlaps",st.benign_token_overlaps},{"role_lifecycles",by_role},
                {"concurrency_clears",st.concurrency_clears},
                {bindings_.self_configured()?"presents_while_tags_fresh_tolerated":"present_expiries_with_fresh_tags",st.present_expiries},
                {"tag_calls",{{"global",st.global_tag_calls},{"frame",st.frame_tag_calls}}},
                {"tag_calls_dropped",{{"setter-failed",st.tag_drops[0]},{"real-overlap",st.tag_drops[1]},{"call-level-decode",st.tag_drops[2]},{"other",st.tag_drops[3]}}},
                {"tag_calls_isolated",st.tags_isolated},
                {"tolerated_overlaps",st.tolerated_overlaps},{"same_key_overlaps",st.same_key_overlaps},
                {"binding_lock_waits",st.lock_waits},{"unkeyed_tag_calls",st.unkeyed_tag_calls},
                {"last_resource_failure",roles.empty()?json(nullptr):json{{"roles",roles},{"causes",causes}}},
                {"scope","every target Evaluate since attach, by binding outcome; 'ready' is the count that bound"}};
            // Only once a game declared something OnlyValidNow: the fixed bound is shared.
            if(st.copies_bound[1]||st.copies_bound[2])out["copies_bound"]={{"depth",st.copies_bound[1]},{"motion",st.copies_bound[2]}};
            if(!last_volatile.is_null())out["last_only_valid_now_refusal"]=last_volatile;
            if(st.present_expiries_copies)out["present_expiries_of_lab_copies"]=st.present_expiries_copies;
            // The game's own frame for each Present (latency markers), always shown
            // once it said anything: a spared Present would otherwise skip a frame.
            out["present_expiries_spared_previous_frame"]=st.present_expiries_spared_previous_frame;
            if(st.presents_declared)out["presents_declared_by_game_marker"]=st.presents_declared;
            if(st.present_attribution_lock_misses)out["present_attribution_lock_misses"]=st.present_attribution_lock_misses;
            return out;}()},
        {"api_calls",{calls_[0].load(),calls_[1].load(),calls_[2].load(),calls_[3].load()}},
        {"api_call_order",{"token","constants","tags","evaluate"}},
        {"metadata_candidates",candidates},{"metadata_rejections",rejected},{"callback_losses",lost_.load()},
        {"callback_loss_scope","affected-invocation; cumulative-loss-history-retained"},
        {"present_callbacks",presents_.load()},{"multiple_swapchains",multiple_chains_.load()},{"multiple_swapchains_blocking",chains_ambiguous()},
        {"admission_lock_waits",lock_waits_.load()},
        {"present_boundary_kind","host-pre-present-invalidation-not-success"},{"current_metadata_candidate",current},
        {"latest",source},{"sr_evaluate",upscaler},{"last_public_tag_call",last_tags},{"rr_options_at_evaluate",rr::describe(v.options)},
        {"only_valid_now_copies",copies_.describe()},
        {"resource_entry",slboundary::describe(v.resources)},
        {"resource_inspection_enabled",inspect_resources_},{"cpu_resource_lease_pending",held},
        {"controlled_insertion_policy","after-successful-evaluate-before-host-rebind"},
        {"host_rebind_contract",[&]{auto h=slhost::read_host_contract(host_flags_address_.load());return json{
            {"flags_readable",h.readable},{"preference_flags",h.readable?json(h.flags):json(nullptr)},
            {"host_rebind_required",h.host_rebind_required()},{"actual_host_rebind_observed",false}};}()},
        {"native_evaluate_contract",{{"reviewed_profile",native_evaluate_contract_.load()},
            {"matching_native_command",inner.metadata&&inner.public_command_native},
            {"scope","public-SL-Evaluate-caller-obligation-not-observed-restoration"}}},
        {"inner_window",{{"call",inner.call},{"frame",inner.frame},{"viewport",inner.viewport},{"status",inner.reason},
            {"sequence_matched",inner.sequence},{"metadata_matched",inner.metadata},{"native_command_matched",inner.native_command},
            {"restore_call_matched",inner.restore_matched},{"restore_result",inner.restore_result==UINT32_MAX?json(nullptr):json(inner.restore_result)},
            {"outer_return_succeeded",inner.outer_succeeded},{"full_state_restoration_verified",false},
            {"candidates",inner_candidates},{"rejected",inner_rejected},{"runtime_restoration_verified",false},{"nr_executed",false},
            {"scope","historical-live-callback-pair-not-rendering-permission"}}},
        {"render_admission",false},{"blockers",json::array({"game-color-exposure-contract-unverified",
        "game-command-bindings-restoration-unverified","game-completion-and-inflight-policy-unconnected","existing-nr-addon-exclusion-unverified"})}};
}
void WorkbenchAdapterHost::set_game_profile(std::string_view profile){
    if(!profiles::game(profile))throw std::logic_error("Cannot change an active or unknown game profile");
    set_game_facts(profiles::Facts::from(*profiles::game(profile)));
}
void WorkbenchAdapterHost::self_configure_before_attach(){
    if(attempted_||stopped_)throw std::logic_error("Self-configuration must be chosen before attach");
    adapter_.self_configure_before_attach();
    // A game that fetches its frame token without an index (Streamline numbers
    // the frames) is identified through the token's public interface.
    set_streamline_token_index_through_interface(true);
}
void WorkbenchAdapterHost::set_game_facts(const profiles::Facts& facts){
    if(attempted_||stopped_||facts.id.empty()||facts.executable.empty())throw std::logic_error("Cannot change an active or unknown game profile");
    facts_=facts;game_profile_=facts.id;
    adapter_.select_viewport_before_attach(facts.viewport);
    adapter_.select_linear_depth_before_attach(facts.linear_depth);
}
namespace {
bool attach_public_boundary(const std::array<void*,4>& api,slboundary::Sink* sink,json& hooks,bool frame_tagging,void* extra_frame_tag){
    if(!extra_frame_tag)return install_streamline_boundary(api,sink,hooks,frame_tagging);
    if(frame_tagging)return false; // Cannot assign two signatures to one slot.
    return install_streamline_dual_tag_boundary({api[0],api[1],api[2],api[3],extra_frame_tag},sink,hooks);
}
}
void WorkbenchAdapterHost::poll() {
    if(!enabled_ || stopped_)return;
    if(installed_&&scope_==WorkbenchScope::full_metadata){poll_options();poll_sr_options();if(research_)research_->poll_restore(*this);}
    if(attempted_)return;
    HMODULE m{};if(!GetModuleHandleExW(0,L"sl.interposer.dll",&m))return;attempted_=true;
    struct ModuleReference {HMODULE module;~ModuleReference(){FreeLibrary(module);}} reference{m};
    module_=module_identity(m);
    // Identity gate is the module's own Authenticode signature; the entry
    // points come from its PUBLIC export table. Whether this is one of the
    // builds reviewed by hand is provenance a research extension may add.
    if(module_.value("signature","")!="ValidCachedTrust"){
        error_="Loaded interposer is not a valid signed NVIDIA module";return;}
    const auto api=slpublic::resolve_public_api(m);
    if(!api.complete()){error_="Public Streamline exports missing or outside the verified image";return;}
    hooks_["public_api"]=slpublic::public_api_report(api);
    if(research_)hooks_["public_api"]["known_reviewed_build"]=research_->known_reviewed_interposer(game_profile_,module_.value("sha256",""));
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(m),&pinned)){
        error_="Could not pin verified interposer";return;}
    adapter_.set_native_interface(reinterpret_cast<PFun_slGetNativeInterface*>(api.native_interface));
    installed_=attach_public_boundary(api.targets(),&adapter_,hooks_,api.frame_tagging(),api.extra_frame_tag());
    if(!installed_)error_="Lightweight SL hook unavailable or conflicting; no retry";
    // Without a research extension no reviewed begin/end callback can ever be
    // installed, so this host admits on public evidence only -- established
    // here, not left to the RR options hook. A pure-SR game (A Plague Tale:
    // Resonance) never loads RR, so that hook never attaches; left to it, the
    // adapter would stay on its default "private evidence required" forever and
    // refuse every clean binding at host-contract.
    if(installed_&&!research_)adapter_.set_inner_admission_required(false);
    if(installed_&&scope_==WorkbenchScope::full_metadata){poll_options();poll_sr_options();if(research_)research_->poll_restore(*this);}
}
bool WorkbenchAdapterHost::attach_fixture(const std::array<void*,4>& api,void* options,const std::array<void*,2>& inner,const void* flags,bool frame_tagging,void* extra_frame_tag,bool native_evaluate_contract,PFun_slGetNativeInterface* native_interface,void* sr_options){
    if(attempted_||installed_||stopped_||!enabled_||(frame_tagging&&extra_frame_tag))return false;
    // A fixture that supplies the reviewed private entry points but no research
    // discovery is refused outright: silently downgrading to public-only
    // admission would look like a profile gap instead of a wiring mistake.
    if(inner[0]&&inner[1]&&!research_)return false;
    attempted_=options_attempted_=true;
    if(scope_==WorkbenchScope::full_metadata){
        options_installed_=options_.install(options,options_hooks_);
        if(sr_target_&&sr_options){sr_options_attempted_=true;sr_options_installed_=sr_options_.install(sr_options,sr_options_hooks_);}
        inner_installed_=options_installed_&&research_&&research_->install_fixture_inner_callbacks(inner);
        adapter_.set_host_flags_address(flags);
        adapter_.set_native_evaluate_contract(native_evaluate_contract&&facts_.native_evaluate_host_rebind);
    }
    adapter_.set_inner_admission_required(inner_installed_);
    if(native_interface)adapter_.set_native_interface(native_interface);
    // Attaching does not require the reviewed begin/end hooks. Without
    // them the adapter admits on public evidence only.
    installed_=attach_public_boundary(api,&adapter_,hooks_,frame_tagging,extra_frame_tag);
    if(!installed_)error_="Isolated synthetic API attachment failed";
    module_={{"origin","synthetic-local-fixture-not-actual-Streamline"}};return installed_;
}
void WorkbenchAdapterHost::poll_options() {
    const auto now=GetTickCount64();
    WorkbenchOptionsRecord record{options_,options_module_,options_hooks_,options_error_,options_resolution_,options_attempted_,options_installed_};
    if(research_)research_->check_options(record,now);
    if(options_attempted_)return;
    if(now<next_options_scan_)return;next_options_scan_=now+1000;
    if(attach_public_options()){if(options_installed_)options_resolution_="public-slGetFeatureFunction";return;} // public path
    // The public path is the controller's only one: keep polling it. A research
    // extension may look for the setter another way (lab_workbench_extension.hpp).
    HMODULE module{};
    if(!research_||!research_->poll_options(record,now,module))return;
    if(options_installed_)inner_installed_=research_->attach_inner_callbacks(module,options_module_);
    adapter_.set_inner_admission_required(inner_installed_);
}
// The setter comes from the interposer's PUBLIC slGetFeatureFunction, which
// hands back exactly the pointer the game itself calls. No per-version setter
// RVA, no module-name matching, no NGX-OTA path list: any signed Streamline
// build works. The pointer must still land in a signed NVIDIA image.
namespace {
// The public setter the game itself receives from slGetFeatureFunction, checked
// to live in a signed NVIDIA image, pinned and watched. -1: not resolvable yet
// (documented to need a set device and a loaded feature -- keep polling); 0:
// refused before any hook; 1: installed; 2: the hook itself was unavailable.
int attach_public_setter(sl::Feature feature,const char* function,const char* label,rr::Watch& watch,
                         json& module,json& hooks,std::string& error,HMODULE& owner_out,const WorkbenchResearchExtension* research){
    HMODULE interposer{};
    if(!GetModuleHandleExW(0,L"sl.interposer.dll",&interposer))return -1;
    struct Reference{HMODULE m;~Reference(){if(m)FreeLibrary(m);}} hold{interposer};
    auto* resolver=reinterpret_cast<PFun_slGetFeatureFunction*>(slpublic::verified_export(interposer,"slGetFeatureFunction"));
    if(!resolver){error="slGetFeatureFunction missing from the verified interposer";return 0;}
    void* setter=nullptr;
    if(resolver(feature,function,setter)!=sl::Result::eOk||!setter)return -1;
    HMODULE owner{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(setter),&owner)){
        error=std::string(label)+" options setter belongs to no loaded module";return 0;}
    struct OwnerReference{HMODULE m;~OwnerReference(){if(m)FreeLibrary(m);}} owned{owner};
    error.clear();
    module=module_identity(owner);
    hooks["public_options"]={{"resolution","public-slGetFeatureFunction"},{"function",function},
        {"address",reinterpret_cast<std::uint64_t>(setter)},
        {"scope","the same setter pointer the game receives; an accepted REQUEST only, not a rendered value"}};
    if(research)hooks["public_options"]["known_reviewed_build"]=research->known_reviewed_options_module(module.value("sha256",""));
    if(module.value("signature","")!="ValidCachedTrust"){
        error=std::string(label)+" options module is not a valid signed NVIDIA module";return 0;}
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(setter,&info,sizeof(info))||info.State!=MEM_COMMIT||info.AllocationBase!=owner||(info.Protect&PAGE_GUARD)||
       !(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))){
        error=std::string(label)+" options setter is outside the signed module's executable image";return 0;}
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(setter),&pinned)){
        error="Could not pin the "+std::string(label)+" options module";return 0;}
    owner_out=owner;
    if(!watch.install(setter,hooks)){error=std::string(label)+" options hook unavailable; no retry";return 2;}
    return 1;
}
}
bool WorkbenchAdapterHost::attach_public_options(){
    HMODULE owner{};
    const int r=attach_public_setter(sl::kFeatureDLSS_RR,"slDLSSDSetOptions","RR",options_,options_module_,options_hooks_,options_error_,owner,research_);
    if(r<0)return false;
    options_attempted_=true;
    if(r==0)return true;
    options_installed_=r==1;
    if(options_installed_&&research_)inner_installed_=research_->attach_inner_callbacks(owner,options_module_);
    adapter_.set_inner_admission_required(inner_installed_);
    return true;
}
// SR's setter, resolved the same public way. Polled only when SR is a target;
// a game that never loads DLSS SR simply keeps it waiting.
void WorkbenchAdapterHost::poll_sr_options(){
    if(!sr_target_||sr_options_attempted_)return;
    const auto now=GetTickCount64();if(now<next_sr_options_scan_)return;next_sr_options_scan_=now+1000;
    HMODULE owner{};
    const int r=attach_public_setter(sl::kFeatureDLSS,"slDLSSSetOptions","SR",sr_options_,sr_options_module_,sr_options_hooks_,sr_options_error_,owner,research_);
    if(r<0)return;
    sr_options_attempted_=true;sr_options_installed_=r==1;
}
void WorkbenchAdapterHost::target_super_resolution_before_attach(){
    if(attempted_||stopped_)throw std::logic_error("Super resolution must be targeted before attach");
    sr_target_=true;adapter_.target_super_resolution_before_attach(&sr_options_);
}
// Research-only instrumentation from here on. A build with no reviewed begin/end
// profile simply runs without it; public admission does not require this evidence.
void WorkbenchAdapterHost::stop(){if(stopped_)return;stopped_=true;if(research_)research_->stop();options_.stop();sr_options_.stop();adapter_.stop();if(installed_)detach_streamline_boundary();}
json WorkbenchAdapterHost::snapshot() {
    auto s=adapter_.snapshot();s["game_profile"]=game_profile_;s["enabled"]=enabled_;s["host_state"]=stopped_?"stopped":!enabled_?"disabled":installed_?"admission-only":attempted_?"rejected":"awaiting-loaded-interposer";
    s["observation_scope"]=scope_==WorkbenchScope::public_topology_only?"public-topology-only-no-resource-lease-no-inner-hooks":"full-metadata";
    // What the adapter actually requires, not whether a private hook exists:
    // reporting the latter can show "public" while admission still requires
    // private evidence.
    s["admission_mode"]=adapter_.inner_admission_required()?"private-inner-callbacks-required":"public-evaluate-evidence-only";
    // The private evidence is RR's own; an SR target never needs it.
    if(sr_target_)s["admission_mode_sr"]="public-evaluate-evidence-only";
    s["admission_scope"]=adapter_.inner_admission_required()
        ?"reviewed begin/end callbacks corroborate frame/viewport/native command list on this build"
        :"no reviewed profile for this Streamline build: admission uses the public Evaluate return, a clean frozen binding, our resource lease, matching options/viewport and a native command buffer; state restoration is the host's own InsertionBindings";
    s["module"]=module_;s["hooks"]=hooks_;s["error"]=error_.empty()?json(nullptr):json(error_);
    s["rr_options"]=options_.snapshot();s["rr_options"]["module"]=options_module_;s["rr_options"]["hooks"]=options_hooks_;
    s["rr_options"]["host_state"]=stopped_?"stopped":!enabled_?"disabled":options_installed_?"observing-requests":options_attempted_?"rejected":"awaiting-loaded-rr";
    s["rr_options"]["error"]=options_error_.empty()?json(nullptr):json(options_error_);
    s["rr_options"]["resolution"]=options_resolution_;
    if(sr_target_){
        s["sr_options"]=sr_options_.snapshot();s["sr_options"]["module"]=sr_options_module_;s["sr_options"]["hooks"]=sr_options_hooks_;
        s["sr_options"]["host_state"]=stopped_?"stopped":!enabled_?"disabled":sr_options_installed_?"observing-requests":sr_options_attempted_?"rejected":"awaiting-loaded-sr";
        s["sr_options"]["error"]=sr_options_error_.empty()?json(nullptr):json(sr_options_error_);
    }
    if(research_)research_->describe(s);
    else{
        // Controller build: neither private hook exists, and nothing pretends otherwise.
        s["rr_inner"]={{"installed",false},{"error","no private RR callbacks in this build"}};
        s["common_restore"]={{"installed",false},{"error","no common restore hook in this build"}};
    }
    return bound_adapter_snapshot(std::move(s),adapter_snapshot_limit);
}
json bound_adapter_snapshot(json s,std::size_t limit){
    if(!s.is_object())return s;
    // Least valuable first. Static descriptive text goes before observed
    // data; the observed public tag record and the latest binding go last.
    static constexpr const char* order[]{
        "/only_valid_now_copies/scope","/only_valid_now_copies/slots_columns",
        "/latest/guide_constants/camera_view_to_clip",
        "/hooks/public_api/scope",
        "/admission_scope",
        "/inner_window/scope",
        "/latest/guide_constants/scope",
        "/sr_evaluate/scope",
        "/sr_evaluate/last_tag_call",
        "/latest/local_tags",
        "/hooks",
        "/rr_options/hooks","/sr_options/hooks","/rr_inner/hooks","/common_restore/hooks",
        "/latest/guide_constants",
        "/module/path","/rr_options/module/path","/sr_options/module/path","/common_restore/module/path",
        "/last_public_tag_call",
        "/sr_evaluate",
        "/inner_window",
        "/host_rebind_contract",
        "/resource_entry",
        "/rr_options_at_evaluate"};
    // The trimming note itself counts against the bound: a fixed 384-byte
    // reserve is exceeded once a dozen paths are listed, and the controller
    // would then clip the largest key whole -- binding_stats -- instead.
    static constexpr const char* scope="optional diagnostic detail removed to stay within the controller byte bound; admission evidence unaffected";
    json trimmed=json::array();
    const auto fits=[&]{return s.dump().size()+(trimmed.empty()?0:json{{"trimmed",trimmed},{"trimmed_scope",scope}}.dump().size())<=limit;};
    for(const char* path:order){
        if(fits())break;
        const json::json_pointer p(path);
        if(!s.contains(p))continue;
        auto& holder=s.at(p.parent_pointer());
        if(!holder.is_object())continue;
        holder.erase(p.back());trimmed.push_back(path);
    }
    if(!trimmed.empty()){s["trimmed"]=std::move(trimmed);s["trimmed_scope"]=scope;}
    return s;
}
}
