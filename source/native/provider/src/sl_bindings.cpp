// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_sl_bindings.hpp"
#include <cstring>
namespace lab::slboundary {
std::array<char,144> overlap_detail(std::uint32_t mask) noexcept {
    std::array<char,144> out{};std::size_t used=0;
    const auto append=[&](const char* name){const auto n=std::strlen(name);
        if(used+n+(used?1:0)>=out.size())return;
        if(used)out[used++]='|';std::memcpy(out.data()+used,name,n);used+=n;};
    if(mask&overlap_api_interval)append("original-api-interval");
    if(mask&overlap_nested_call)append("nested-call");
    if(mask&overlap_pending_evaluate)append("pending-evaluate");
    if(mask&overlap_freeze_lock)append("freeze-lock");
    if(mask&overlap_return_lock)append("return-lock");
    if(!used)append("unspecified");return out;
}
std::array<char,144> cross_thread_detail(std::uint32_t mask) noexcept {
    std::array<char,144> out{};std::size_t used=0;
    const auto append=[&](const char* name){const auto n=std::strlen(name);
        if(used+n+(used?1:0)>=out.size())return;
        if(used)out[used++]='|';std::memcpy(out.data()+used,name,n);used+=n;};
    if(mask&cross_thread_constants)append("constants-set-on-another-thread");
    if(mask&cross_thread_frame_tag)append("per-frame-tag-set-on-another-thread");
    if(!used)append("same-thread");return out;
}
std::array<char,144> invalidation_detail(std::uint32_t mask) noexcept {
    std::array<char,144> out{};std::size_t used=0;
    const auto append=[&](const char* name){const auto n=std::strlen(name);
        if(used+n+(used?1:0)>=out.size())return;
        if(used)out[used++]='|';std::memcpy(out.data()+used,name,n);used+=n;};
    if(mask&invalidation_present)append("present-boundary");
    if(mask&invalidation_loss)append("binding-observation-loss");
    if(mask&invalidation_token)append("token-generation");
    if(mask&invalidation_constants)append("constants-revision");
    if(mask&invalidation_abort)append("other-call-aborted");
    if(mask&invalidation_adapter_epoch)append("adapter-epoch");
    if(mask&invalidation_adapter_coverage)append("adapter-coverage");
    if(mask&invalidation_multiple_swapchains)append("multiple-swapchains");
    if(!used)append("unspecified");return out;
}
int Bindings::role(sl::BufferType type) const noexcept {
    if(type==sl::kBufferTypeScalingOutputColor)return 0;
    if(type==depth_type_||(self_configure_&&depth_rank(type)))return 1;
    if(type==sl::kBufferTypeMotionVectors)return 2;
    return -1;
}
const char* rejection_name(Rejection r) noexcept {
    switch(r){case Rejection::none:return "ready";case Rejection::not_target:return "not-target";
    case Rejection::metadata:return "metadata-invalid";case Rejection::viewport:return "viewport-unsupported";
    case Rejection::token:return "explicit-token-generation-missing";case Rejection::constants:return "frame-matching-constants-missing";
    case Rejection::resource:return "fresh-resource-missing-or-revoked";case Rejection::volatile_global:return "global-only-valid-now-expired";
    case Rejection::present_unknown:return "present-boundary-not-connected";case Rejection::overlap:return "overlapping-api-call";
    case Rejection::mismatch:return "call-does-not-match-entry";case Rejection::stale:return "snapshot-invalidated";
    case Rejection::thread:return "source-thread-profile-unsupported";case Rejection::capacity:return "token-capacity";
    case Rejection::viewport_selected:return "viewport-selected-history-reset";
    case Rejection::volatile_copy:return "only-valid-now-copy-refused";}
    return "unknown-rejection";
}
const char* copy_outcome_name(CopyOutcome o) noexcept {
    switch(o){case CopyOutcome::none:return "not-copied";case CopyOutcome::copied:return "copied-at-tag-time";
    case CopyOutcome::not_wanted:return "not-copied-no-nr-frame-wanted";
    case CopyOutcome::output_role:return "output-colour-written-in-place-not-copyable";
    case CopyOutcome::frame_scoped:return "per-frame-tag-not-copied";
    case CopyOutcome::call_not_stored:return "tag-call-not-stored";
    case CopyOutcome::no_command_buffer:return "no-command-buffer";
    case CopyOutcome::command_list_unusable:return "command-buffer-not-a-direct-d3d12-list";
    case CopyOutcome::device_mismatch:return "resource-on-another-device";
    case CopyOutcome::invalid_tag:return "invalid-tag";case CopyOutcome::state_unknown:return "declared-state-unknown";
    case CopyOutcome::not_a_texture:return "not-a-single-2d-texture";case CopyOutcome::multisample:return "multisample-unsupported";
    case CopyOutcome::format_unsupported:return "format-unsupported";
    case CopyOutcome::simultaneous_access:return "simultaneous-access-unsupported";
    case CopyOutcome::layout_unsupported:return "layout-unsupported";
    case CopyOutcome::awaiting_allocation:return "copy-texture-awaiting-allocation";
    case CopyOutcome::slots_busy:return "copy-slots-busy";case CopyOutcome::lock_contention:return "copy-lock-contention";
    case CopyOutcome::replaced_before_evaluate:return "copy-replaced-before-evaluate";
    case CopyOutcome::superseded:return "superseded-by-another-tag-of-the-role";
    case CopyOutcome::awaiting_reshape:return "copy-texture-awaiting-reshape";
    case CopyOutcome::backoff:return "not-copied-backoff-after-refused-evaluates";
    case CopyOutcome::count:break;}
    return "unknown-copy-outcome";
}
bool Bindings::stores_tag_call(const Call& c) const noexcept {
    if(c.api!=Api::tags||c.result!=sl::Result::eOk||conflicting(c)||c.inputs.tag_count>c.inputs.tags.size())return false;
    if((self_configure_?c.inputs.call_issues:c.inputs.issues)!=0)return false;
    const auto v=published_viewport_.load();
    if(!c.inputs.viewport_present||v<0||c.inputs.viewport!=static_cast<std::uint32_t>(v))return false;
    // The pinned profile drops a legacy call once per-frame tags were seen.
    return c.frame_scoped_tags||self_configure_||!frame_tags_seen_.load();
}
void Bindings::refuse_entry(std::uint64_t call,std::uint32_t roles,const std::array<std::uint8_t,3>& reasons) noexcept {
    auto guard=acquire();if(!guard.owns_lock()){++loss_;return;} // sync() then refuses the call as stale
    if(!pending_call_||pending_call_!=call||!roles)return;
    if(pending_.rejection==Rejection::none)pending_.rejection=Rejection::volatile_copy;
    pending_.volatile_roles|=roles;for(unsigned i=0;i<3;++i)if(roles&(1u<<i))pending_.volatile_reasons[i]=reasons[i];
}
void Bindings::expire_resources() noexcept {
    for(auto& g:globals_)g.fresh=false;
    for(auto& f:frame_tags_)for(auto& g:f.resources)g.fresh=false;
}
void Bindings::clear_cache() noexcept {for(auto& t:tokens_){t.pointer=nullptr;t.known=false;}for(auto& c:common_)c={};expire_resources();next_frame_known_=false;}
void Bindings::expire_call_target(const Call& c) noexcept {
    if(!c.frame_scoped_tags){for(auto& g:globals_)g.fresh=false;return;}
    if(auto* t=token(c.token);t&&t->known)if(auto* f=frame_tags(t->index,c.inputs.viewport))for(auto& g:f->resources)g.fresh=false;
}
Bindings::FrameTags* Bindings::frame_tags(std::uint32_t frame,std::uint32_t viewport) noexcept {
    for(auto& f:frame_tags_)if(f.occupied&&f.frame==frame&&f.viewport==viewport)return &f;return nullptr;
}
Bindings::FrameTags* Bindings::frame_tag_slot(std::uint32_t frame,std::uint32_t viewport) noexcept {
    if(auto* f=frame_tags(frame,viewport))return f;
    for(auto& f:frame_tags_){bool fresh=false;for(const auto& g:f.resources)fresh|=g.fresh;
        if(!f.occupied||!fresh){f={};f.occupied=true;f.frame=frame;f.viewport=viewport;return &f;}}
    // Self-configuring, a Present does not expire frame-keyed tags, so a frame
    // that never reached Evaluate could hold a slot forever. Only a slot at
    // least eight frames behind the new one is taken: never an in-flight frame.
    if(self_configure_){FrameTags* oldest=nullptr;std::uint32_t behind=0;
        for(auto& f:frame_tags_){const std::uint32_t d=frame-f.frame;if(d<0x80000000u&&d>=behind){behind=d;oldest=&f;}} // furthest behind `frame`
        if(oldest&&behind>=8u){*oldest={};oldest->occupied=true;oldest->frame=frame;oldest->viewport=viewport;return oldest;}}
    return nullptr; // Never silently evict an in-flight frame's tagged resources.
}
const char* constants_state_name(ConstantsState s) noexcept {
    switch(s){case ConstantsState::current:return "current";case ConstantsState::token_changed:return "token-reissued";
    case ConstantsState::setter_rejected:return "setter-rejected";default:return "never-set";}
}
void Bindings::sync() noexcept {
    const auto p=present_.load(),l=loss_.load();if(p!=seen_present_ || l!=seen_loss_){
        // Present expires global resource tags, not common constants already
        // copied for another explicit frame token. SL accepts constants early
        // and supports multiple frames in flight. A real observation loss still
        // invalidates everything. No resource lifetime is extended here.
        // Self-configuring, a Present expires no tag at all. Expiring at Present
        // is this layer's own caution, not Streamline's rule: a tag's lifecycle
        // is the game's promise about the resource, and Streamline does not drop
        // tags at a Present. An engine with its own submission thread presents
        // frame N-1 between tagging frame N and evaluating it -- A Plague Tale:
        // Resonance does, and expiring there refuses every one of its calls as
        // "fresh-resource-missing-or-revoked". Freshness stays
        // strict without it: every target Evaluate consumes the tags, so each
        // one binds only what was tagged since the previous one; a legacy global
        // tag must still come from the Evaluate's own thread; an OnlyValidNow tag
        // is still refused. The pinned profile keeps expiring at Present.
        if(l!=seen_loss_)clear_cache();
        else {bool fresh=false,copy=false;for(const auto& g:globals_){fresh|=g.fresh;copy|=g.fresh&&g.tag.lab_copy;}
            for(const auto& f:frame_tags_)for(const auto& g:f.resources)fresh|=g.fresh;if(fresh)++present_expiries_;
            // Pinned: the Present ends them. Counted, copies apart.
            if(!self_configure_){if(copy)++present_expiries_copies_;expire_resources();}}
        if(pending_call_){
            // A Present ends the validity of GLOBAL tags. It does not end the
            // validity of a tag passed inline with THIS Evaluate and declared
            // valid until Evaluate: that is this call's own argument and the
            // call has not returned. Frame generation's pacing thread presents
            // repeatedly inside one Evaluate window, so invalidating on every
            // Present skipped frames that carried no resource risk at all.
            bool inline_until_evaluate=true;
            for(unsigned i=0;i<3;++i)
                inline_until_evaluate=inline_until_evaluate&&(self_configure_
                    // Self-configuring: another frame's Present, for any tag this
                    // frame may still use (OnlyValidNow is refused on its own).
                    // A Lab copy taken at the tag call is ours: no Present ends it.
                    ?(pending_.binding.resources[i].lifecycle!=sl::eOnlyValidNow||pending_.binding.resources[i].lab_copy)
                    :pending_.binding.local[i]&&pending_.binding.resources[i].lifecycle==sl::eValidUntilEvaluate);
            if(p!=seen_present_){
                ++pending_.presents_during_call;
                if(!inline_until_evaluate){pending_.rejection=Rejection::stale;pending_.invalidations|=invalidation_present;}
            }
            if(l!=seen_loss_){pending_.rejection=Rejection::stale;pending_.invalidations|=invalidation_loss;}}
        // A missed return must not leave a permanent orphan transaction.
        if(l!=seen_loss_)pending_call_=0;
        seen_present_=p;seen_loss_=l;
    }
}
bool Bindings::live_frame(std::uint32_t& lowest) const noexcept {
    bool any=false;std::uint32_t low=0;
    const auto fold=[&](bool known,std::uint32_t frame){if(!known)return false;
        if(!any||static_cast<std::int32_t>(frame-low)<0)low=frame;any=true;return true;};
    if(pending_call_&&!fold(pending_frame_known_,pending_frame_))return false;
    for(const auto& g:globals_)if(g.fresh&&!fold(g.frame_known,g.frame))return false;
    for(const auto& f:frame_tags_)for(const auto& g:f.resources)if(g.fresh&&!fold(true,f.frame))return false;
    lowest=low;return any;
}
bool Bindings::present_boundary_of_frame(std::uint32_t frame) noexcept {
    ++presents_declared_;
    // The Present thread never waits on the binding lock: a miss is the ordinary
    // conservative boundary, never a spared one.
    std::unique_lock guard(mutex_,std::try_to_lock);
    if(!guard.owns_lock()){++present_attribution_misses_;++present_;return false;}
    sync(); // earlier undeclared Presents take effect first, exactly as they would have
    std::uint32_t live=0;
    if(!live_frame(live)||static_cast<std::int32_t>(frame-live)>=0){++present_;return false;}
    // Declared for an earlier frame than everything a Present could end: that
    // Present ends none of it. Recorded, never hidden.
    if(pending_call_)++pending_.presents_during_call;
    ++present_spared_;return true;
}
Bindings::Token* Bindings::token(void* ptr) noexcept {if(!ptr)return nullptr;for(auto& t:tokens_)if(t.pointer==ptr)return &t;return nullptr;}
Bindings::Common* Bindings::common(std::uint32_t frame,std::uint32_t viewport) noexcept {
    for(unsigned i=0;i<common_limit();++i){auto& c=common_[i];if(c.occupied&&c.frame==frame&&c.viewport==viewport)return &c;}return nullptr;
}
Bindings::Common& Bindings::common_slot(std::uint32_t frame,std::uint32_t viewport) noexcept {
    if(auto* c=common(frame,viewport))return *c;
    auto* oldest=&common_[0];for(unsigned i=0;i<common_limit();++i){auto& c=common_[i];if(!c.occupied){oldest=&c;break;}if(c.call<oldest->call)oldest=&c;}
    *oldest={};oldest->occupied=true;oldest->frame=frame;oldest->viewport=viewport;return *oldest;
}
void Bindings::remember_token(const Call& c) noexcept {
    // Self-configuring, a failed or unreadable fetch is simply not learned: an
    // Evaluate that uses that token is refused by name (token unknown), and
    // every other frame's cache stays as it was.
    if(!c.token || c.result!=sl::Result::eOk || c.inputs.issues || conflicting(c)){
        if(self_configure_)return;
        if(conflicting(c))++concurrency_clears_;clear_cache();return;}
    auto* t=token(c.token);if(!t)for(auto& empty:tokens_)if(!empty.pointer){t=&empty;break;}
    // Self-configuring, a full table gives up the token seen longest ago (never
    // the one a target Evaluate is using right now) instead of clearing all.
    if(!t&&self_configure_)for(auto& old:tokens_)if((!pending_call_||old.pointer!=pending_.binding.token)&&(!t||old.seen<t->seen))t=&old;
    if(t&&self_configure_&&t->pointer!=c.token&&t->pointer)*t={};
    if(!t){clear_cache();if(pending_call_)pending_.rejection=Rejection::capacity;return;}
    if(t->pointer!=c.token || !t->known || !c.frame_index_known || t->index!=c.frame_index){
        if(t->pointer!=c.token)*t={};
        t->constants_state=ConstantsState::token_changed;t->generation=++generation_;
    }
    t->pointer=c.token;t->index=c.frame_index;t->known=c.frame_index_known && c.frame_index!=UINT32_MAX;t->seen=++token_seen_;
}
void Bindings::entering(const Call& c) noexcept {
    auto guard=acquire();if(!guard.owns_lock()){++loss_;return;}sync();
    // A concurrent Evaluate keeps its frame identity for rejection bookkeeping
    // only; the cache itself is still distrusted and cleared below.
    std::uint32_t concurrent_frame=UINT32_MAX;bool concurrent_frame_known=false;
    if(c.concurrent&&!conflicting(c)){if(c.token_only_overlap)++benign_token_overlaps_;else ++tolerated_overlaps_;}
    if(conflicting(c)){if(c.api==Api::evaluate)if(auto* known=token(c.token);known&&known->known){concurrent_frame=known->index;concurrent_frame_known=true;}
        ++concurrency_clears_;clear_cache();if(pending_call_){pending_.rejection=Rejection::overlap;pending_.overlap_sources|=overlap_api_interval;}}
    if(c.api!=Api::evaluate)return;
    // Self-configuring, another upscaler's Evaluate (SR while RR is the target,
    // or any feature that is not ours) consumes nothing of ours, in flight
    // beside our Evaluate or not.
    if(self_configure_&&!target(c.feature))return;
    if(pending_call_){pending_.rejection=Rejection::overlap;pending_.overlap_sources|=overlap_pending_evaluate;return;}
    if(!target(c.feature)){for(auto& g:globals_)g.fresh=false;next_frame_known_=false; // which frame the next globals serve is not derivable
        if(auto* t=token(c.token))if(auto* f=frame_tags(t->index,c.inputs.viewport))for(auto& g:f->resources)g.fresh=false;
        return;}
    pending_call_=c.id;pending_={};auto& b=pending_.binding;
    b.call=c.id;b.feature=c.feature;b.command=c.command;b.token=c.token;b.thread=c.thread;b.viewport=c.inputs.viewport;
    b.present_epoch=seen_present_;
    // Cache data is frozen here, not looked up after the original has run.
    auto fail=[&](Rejection r){if(pending_.rejection==Rejection::not_target)pending_.rejection=r;};
    if(conflicting(c) || c.parent)fail(Rejection::overlap);
    if(conflicting(c))pending_.overlap_sources|=overlap_api_interval;
    if(c.parent)pending_.overlap_sources|=overlap_nested_call;
    // Self-configuring, only a problem with the call as a whole refuses it: a
    // problem confined to one inline tag stays with that tag (refused below if
    // it is one of our three roles, ignored otherwise), and constants passed
    // inline with the Evaluate are this call's own constants.
    const unsigned issues=self_configure_?c.inputs.call_issues:c.inputs.issues;
    if(issues || c.inputs.tag_count>c.inputs.tags.size() || (c.inputs.constants_present&&!self_configure_) || !c.command)fail(Rejection::metadata);
    if(self_configure_&&c.inputs.viewport_present){
        if(!locked_||c.inputs.viewport!=viewport_){
            if(!locked_||++other_streak_>=kSwitchAfter){
                if(locked_)viewport_switches_.fetch_add(1);
                locked_=true;viewport_=c.inputs.viewport;published_viewport_=viewport_;other_streak_=0;settle_=kSettle;
                expire_resources();
                fail(Rejection::viewport_selected);
            }else other_viewport_calls_.fetch_add(1);
        }else other_streak_=0;
    }
    if(!valid_view(c))fail(Rejection::viewport);
    const bool settling=self_configure_&&settle_&&valid_view(c);
    auto* t=token(c.token);
    if(concurrent_frame_known&&(!t||!t->known))b.frame_index=concurrent_frame; // identity only, never admission
    // The frame this snapshot belongs to, for present_boundary_of_frame: the
    // token's index, lowered below to the derived frame of each non-local tag bound.
    pending_frame_known_=t&&t->known;pending_frame_=pending_frame_known_?t->index:0u;
    if(!t || !t->known)fail(Rejection::token);
    else {
        b.token_generation=t->generation;b.frame_index=t->index;
        b.constants_state=t->constants_state;b.constants_call=t->constants_call;
        const auto* value=common(t->index,b.viewport);
        if(self_configure_&&c.inputs.constants_present){
            // Inline constants: marked by constants_call == this call, which a
            // setter call can never be, so return() does not revalidate them
            // against the frame-keyed cache they never came from.
            b.constants=c.inputs.constants;b.constants_call=c.id;b.constants_state=ConstantsState::current;
        }else if(!value||!value->fresh){
            if(value){b.constants_call=value->call;b.constants_state=ConstantsState::setter_rejected;}
            fail(Rejection::constants);
        }else {b.constants=value->constants;b.constants_call=value->call;b.constants_state=ConstantsState::current;
            // Common constants are looked up by frame-token index AND viewport and
            // must be fresh, so the association to THIS Evaluate is already proven
            // by a stronger key than thread identity. An engine that sets its
            // constants on a render thread and evaluates on a submission thread --
            // RE Engine does (Resident Evil Requiem) -- is not a reason
            // to refuse. It is recorded rather than silently accepted.
            if(value->thread!=c.thread)pending_.cross_thread_sources|=cross_thread_constants;}
        // Common constants are a copied per-frame value, not a resource tag
        // consumed by Evaluate. A later successful/failed setter changes their
        // revision; the exact frozen revision is revalidated on return.
    }
    auto* framed=t&&t->known?frame_tags(t->index,b.viewport):nullptr;
    const bool ours=valid_view(c); // role x lifecycle counts describe our viewport only
    for(unsigned i=0;i<3;++i){const Tag* local=nullptr;
        for(unsigned j=0;j<c.inputs.tag_count && j<c.inputs.tags.size();++j)if(role(c.inputs.tags[j].type)==static_cast<int>(i)&&
            (!local||i!=1||!self_configure_||depth_rank(c.inputs.tags[j].type)>=depth_rank(local->type)))local=&c.inputs.tags[j];
        if(local){b.resources[i]=*local;b.local[i]=true;b.tag_calls[i]=c.id;if(ours)count_lifecycle(i,0,local);}
        else {
            const Global missing{};
            // Where the tag came from decides how much thread identity is worth.
            // A per-frame tag is keyed by frame-token index and viewport, so a
            // different source thread adds no doubt and is only recorded. A legacy
            // GLOBAL tag has no frame key at all: thread identity is then the only
            // evidence that it belongs to this call, and it stays mandatory.
            const Global* source=frame_tags_seen_?(framed?&framed->resources[i]:&missing):&globals_[i];bool from_global=!frame_tags_seen_;
            // Self-configuring, a game may use both tagging APIs (middleware that
            // still calls slSetTag): a role its frame-keyed call did not fill falls
            // back to a fresh legacy tag, which keeps the legacy thread rule.
            if(self_configure_&&frame_tags_seen_&&!source->fresh&&globals_[i].fresh){source=&globals_[i];from_global=true;}
            const bool frame_keyed=!from_global&&framed;
            const auto& g=*source;b.resources[i]=g.tag;b.tag_calls[i]=g.call;
            if(!frame_keyed){if(!g.frame_known)pending_frame_known_=false;
                else if(static_cast<std::int32_t>(g.frame-pending_frame_)<0)pending_frame_=g.frame;}
            if(ours)count_lifecycle(i,from_global?2u:1u,g.call?&g.tag:nullptr);
            if(!g.fresh){fail(Rejection::resource);pending_.resource_roles|=1u<<i;pending_.resource_causes|=g.call?2u:1u;}
            if(!seen_present_&&!present_spared_.load())fail(Rejection::present_unknown); // a spared Present proves the wiring too
            if(g.thread!=c.thread){
                if(frame_keyed)pending_.cross_thread_sources|=cross_thread_frame_tag;
                else fail(Rejection::thread);
            }
            // eOnlyValidNow: the resource was promised at the tag call only. It
            // stays refused unless the adapter copied it THERE, as Streamline
            // itself does (an input role only: NR writes the output colour back
            // in place, so a copy of it would receive the result instead of the
            // game). Every other rule above still applies to the copy's tag.
            if(g.tag.lifecycle==sl::eOnlyValidNow&&(i==0||!g.tag.lab_copy)){
                const auto why=i==0?CopyOutcome::output_role:static_cast<CopyOutcome>(g.tag.copy_refusal);
                fail(why==CopyOutcome::none?Rejection::volatile_global:Rejection::volatile_copy);
                pending_.volatile_roles|=1u<<i;pending_.volatile_reasons[i]=static_cast<std::uint8_t>(why);
            }else if(g.tag.lifecycle==sl::eOnlyValidNow&&ours)++copies_bound_[i];
        }
        const auto& r=b.resources[i];
        if(r.issues || r.null_resource || !r.native || r.state==UINT32_MAX){fail(Rejection::resource);pending_.resource_roles|=1u<<i;pending_.resource_causes|=4u;}
    }
    // Another viewport's Evaluate consumes that viewport's inputs in SL, not
    // ours; its own per-frame tags are keyed to it and expire below as usual.
    // The globals stored from now on serve the frame after this one.
    if(!other_view(c)){for(auto& g:globals_)g.fresh=false;next_frame_known_=t&&t->known;next_frame_=next_frame_known_?t->index+1u:0u;}
    if(framed)for(auto& g:framed->resources)g.fresh=false;
    // Inputs a game set before our lock existed are missing, not wrong: during
    // the settling window that is a skip, never the constants bypass.
    if(settling){--settle_;
        if(pending_.rejection==Rejection::constants||pending_.rejection==Rejection::resource||pending_.rejection==Rejection::token)
            pending_.rejection=Rejection::viewport_selected;}
    if(pending_.rejection==Rejection::not_target)pending_.rejection=Rejection::none;
}
Resolution Bindings::frozen_entry(const Call& c) noexcept {
    auto guard=acquire();if(!guard.owns_lock()){++loss_;return {Rejection::overlap,{},0,overlap_freeze_lock};}sync();
    if(c.api!=Api::evaluate || !pending_call_ || pending_call_!=c.id)return {Rejection::mismatch,{}};
    return pending_;
}
Resolution Bindings::returned(const Call& c) noexcept {
    auto guard=acquire();if(!guard.owns_lock()){++loss_;return {Rejection::overlap,{},0,overlap_return_lock};}sync();
    if(conflicting(c)){++concurrency_clears_;clear_cache();if(pending_call_){pending_.rejection=Rejection::overlap;pending_.overlap_sources|=overlap_api_interval;}}
    if(c.api==Api::token){remember_token(c);return {};}
    if(c.api==Api::constants){
        auto* t=token(c.token);
        // Another viewport's constants live under that viewport's own key.
        if(t&&t->known&&other_view(c))return {};
        // An unidentifiable setter cannot safely leave reusable frame data.
        // Self-configuring it is not stored and nothing else is dropped: its
        // Evaluate carries the same unknown token and is refused by name.
        if(!t||!t->known||!valid_view(c)){if(!self_configure_)for(auto& v:common_)v={};return {};}
        auto& value=common_slot(t->index,c.inputs.viewport);value.fresh=false;value.call=c.id;
        t->constants_call=c.id;t->constants_state=ConstantsState::setter_rejected;
        if(t && t->known && c.result==sl::Result::eOk && !conflicting(c) && !c.inputs.issues && valid_view(c) && c.inputs.constants_present){
            value.constants=c.inputs.constants;value.thread=c.thread;value.fresh=true;t->constants_state=ConstantsState::current;
        }
        return {};
    }
    if(c.api==Api::tags){
        if(c.frame_scoped_tags){frame_tags_seen_=true;++frame_tag_calls_;}else ++global_tag_calls_;
        if(other_view(c))return {}; // another viewport's tags: its own key, never ours
        // Self-configuring, only a problem with the call as a whole discards
        // it. A problem confined to one tag -- A Plague Tale: Resonance sends
        // eight, NR uses three -- stays with that tag: a role tag that has one
        // is stored unusable and refused by name at Evaluate, and any other tag
        // is simply not ours. The pinned profile stays strict.
        const unsigned issues=self_configure_?c.inputs.call_issues:c.inputs.issues;
        if(c.result!=sl::Result::eOk || conflicting(c) || issues || c.inputs.tag_count>c.inputs.tags.size() || !valid_view(c)){
            ++tag_drops_[c.result!=sl::Result::eOk?0:conflicting(c)?1:issues||c.inputs.tag_count>c.inputs.tags.size()?2:3];
            // Self-configuring, doubt only what this call could have replaced.
            // A failed setter replaced nothing in SL either; a call before any
            // viewport was selected cannot be ours.
            if(self_configure_){if(c.result==sl::Result::eOk&&valid_view(c))expire_call_target(c);return {};}
            if(conflicting(c))++concurrency_clears_;expire_resources();return {};}
        if(c.inputs.issues&&!issues)++tags_isolated_;
        auto* target=&globals_;
        std::uint32_t keyed_frame=0;
        if(c.frame_scoped_tags){
            auto* t=token(c.token);
            if(!t||!t->known){if(self_configure_){++unkeyed_tag_calls_;return {};}expire_resources();return {};}
            auto* f=frame_tag_slot(t->index,c.inputs.viewport);
            if(!f){if(self_configure_){++unkeyed_tag_calls_;return {};}expire_resources();return {};}
            target=&f->resources;keyed_frame=t->index;
        }else if(frame_tags_seen_&&!self_configure_){expire_resources();return {};} // Do not combine legacy and per-frame APIs.
        // Self-configuring: a setter for the frame and viewport our in-flight
        // Evaluate uses (or a legacy tag, which has no key) returned while it
        // ran. Which of the two values SL consumed is unknowable, so that one
        // Evaluate is skipped; the values are stored for the frames after it.
        if(self_configure_&&pending_call_&&pending_.ready()&&(!c.frame_scoped_tags||
           (keyed_frame==pending_.binding.frame_index&&c.inputs.viewport==pending_.binding.viewport))){
            bool ours=false;for(unsigned i=0;i<c.inputs.tag_count;++i)ours|=role(c.inputs.tags[i].type)>=0;
            if(ours){pending_.rejection=Rejection::overlap;pending_.overlap_sources|=overlap_api_interval;++same_key_overlaps_;}
        }
        for(unsigned i=0;i<c.inputs.tag_count;++i){const auto& tag=c.inputs.tags[i];const int r=role(tag.type);if(r<0)continue;
            auto& slot=(*target)[r];
            // Both depth semantics tagged for this frame: keep hardware depth.
            if(r==1&&self_configure_&&slot.fresh&&depth_rank(slot.tag.type)>depth_rank(tag.type))continue;
            slot={tag,c.id,c.thread,!tag.null_resource && tag.native && !tag.issues};
            slot.frame=c.frame_scoped_tags?keyed_frame:next_frame_;slot.frame_known=c.frame_scoped_tags||next_frame_known_;}
        return {};
    }
    if(c.api!=Api::evaluate || !target(c.feature))return {};
    if(!pending_call_ || c.id!=pending_call_){Resolution m{Rejection::mismatch,{}};count(m,c);return m;}
    auto out=pending_;pending_call_=0;
    const auto* t=token(c.token);
    const auto* value=common(out.binding.frame_index,out.binding.viewport);
    const unsigned issues=self_configure_?c.inputs.call_issues:c.inputs.issues;
    if(c.result!=sl::Result::eOk || issues || c.inputs.tag_count>c.inputs.tags.size())out.rejection=Rejection::metadata;
    if(c.command!=out.binding.command || c.token!=out.binding.token || c.thread!=out.binding.thread)out.rejection=Rejection::mismatch;
    if(out.ready()){
        if(!t || !t->known || t->generation!=out.binding.token_generation)out.invalidations|=invalidation_token;
        const bool inline_constants=out.binding.constants_call==out.binding.call;
        if(!inline_constants&&(!value || !value->fresh || value->call!=out.binding.constants_call))out.invalidations|=invalidation_constants;
        if(out.invalidations)out.rejection=Rejection::stale;
    }
    count(out,c);
    return out;
}
void Bindings::aborted(const Call& c) noexcept {
    auto guard=acquire();if(!guard.owns_lock()){++loss_;return;}
    clear_cache();if(pending_call_==c.id)pending_call_=0;else if(pending_call_){pending_.rejection=Rejection::stale;pending_.invalidations|=invalidation_abort;}
}
}
