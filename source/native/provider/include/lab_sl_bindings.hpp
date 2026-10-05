// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_sl_boundary.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace lab::slboundary {
enum class Rejection {
    none, not_target, metadata, viewport, token, constants, resource,
    volatile_global, present_unknown, overlap, mismatch, stale, thread, capacity,
    // Self-configuration only: this call selected (or moved) the viewport, or
    // came within the settling window after it. Nothing was recorded for it;
    // a skip that resets history, never the constants bypass that turns NR OFF.
    viewport_selected,
    // Seen in Cyberpunk 2077 with DLSS-G on: a stored tag the game declared
    // eOnlyValidNow for an NR role that was NOT copied at its tag call, for the
    // CopyOutcome recorded per role: the output colour (NR writes back in
    // place, so it is never copied), or a copy refused at the tag call or at
    // this Evaluate. volatile_global stays the name when no copy was tried.
    volatile_copy
};
const char* rejection_name(Rejection) noexcept;
// What became of an eOnlyValidNow tag of an NR role at its tag call, and of a
// Lab copy at the Evaluate that consumes it (lab_sl_volatile_copy.hpp). The
// value is stored in Tag::copy_refusal and Resolution::volatile_reasons.
// superseded -- another tag of the same role in that call is
// the one the binding keeps (one copy per role per call); awaiting_reshape --
// the role's copy textures have another shape and a shape change is held;
// backoff -- copies bound by a run of Evaluates were all refused for a reason a
// copy cannot fix, so only a probe per second is copied until one is admitted.
enum class CopyOutcome : std::uint8_t {
    none, copied, not_wanted, output_role, frame_scoped, call_not_stored,
    no_command_buffer, command_list_unusable, device_mismatch, invalid_tag, state_unknown,
    not_a_texture, multisample, format_unsupported, simultaneous_access, layout_unsupported,
    awaiting_allocation, slots_busy, lock_contention, replaced_before_evaluate,
    superseded, awaiting_reshape, backoff, count
};
const char* copy_outcome_name(CopyOutcome) noexcept;
// Small diagnostic bitset, not permission to ignore an invalidation. Several
// independent invalidations can happen in one CPU invocation. No event log.
enum Invalidation : std::uint32_t {
    invalidation_present=1u, invalidation_loss=2u, invalidation_token=4u,
    invalidation_constants=8u, invalidation_abort=16u,
    invalidation_adapter_epoch=32u, invalidation_adapter_coverage=64u,
    invalidation_multiple_swapchains=128u
};
std::array<char,144> invalidation_detail(std::uint32_t mask) noexcept;
enum OverlapSource : std::uint32_t {
    overlap_api_interval=1u, overlap_nested_call=2u, overlap_pending_evaluate=4u,
    overlap_freeze_lock=8u, overlap_return_lock=16u
};
std::array<char,144> overlap_detail(std::uint32_t mask) noexcept;
// Which frozen inputs came from a thread other than the one calling Evaluate.
// Only possible where the association is already proven by frame-token index and
// viewport; a legacy global tag from another thread is still refused outright.
// Recorded, never silent: an engine that splits render and submission threads
// (RE Engine, Resident Evil Requiem) is visible in the status.
enum CrossThreadSource : std::uint32_t {
    cross_thread_constants=1u, cross_thread_frame_tag=2u
};
std::array<char,144> cross_thread_detail(std::uint32_t mask) noexcept;
enum class ConstantsState { never_set, current, token_changed, setter_rejected };
const char* constants_state_name(ConstantsState) noexcept;
struct Binding {
    // The upscaler this call evaluated: RR, or SR where SR is a target.
    sl::Feature feature=sl::kFeatureDLSS_RR;
    std::uint64_t call=0,token_generation=0,constants_call=0,present_epoch=0;
    std::uint32_t frame_index=0,viewport=0,thread=0;
    void* token=nullptr;
    void* command=nullptr;
    ConstantsState constants_state=ConstantsState::never_set;
    // Order: output target, depth, motion. Local override never updates global.
    std::array<Tag,3> resources{};
    std::array<std::uint64_t,3> tag_calls{};
    std::array<bool,3> local{};
    sl::Constants constants{};
};
struct Resolution {
    Rejection rejection=Rejection::not_target;
    Binding binding{};
    std::uint32_t invalidations=0;
    std::uint32_t overlap_sources=0;
    // Presents observed between this call's entry and its return. Tolerated
    // only for inline eValidUntilEvaluate tags (see Bindings::sync); recorded
    // either way so a tolerated present is never silently invisible.
    std::uint32_t presents_during_call=0;
    // CrossThreadSource bits. Never a rejection on its own -- see the enum.
    std::uint32_t cross_thread_sources=0;
    // For a resource rejection: which roles (bit 0 output colour, 1 depth,
    // 2 motion) and why (1 never tagged, 2 tagged but consumed or expired,
    // 4 an invalid tag: null, issues or unknown state). Diagnostic only.
    std::uint32_t resource_roles=0,resource_causes=0;
    // Roles (bits as above) whose stored tag was eOnlyValidNow and not a Lab
    // copy, and per role the CopyOutcome that says why. Diagnostic only.
    std::uint32_t volatile_roles=0;
    std::array<std::uint8_t,3> volatile_reasons{};
    bool ready() const noexcept{return rejection==Rejection::none;}
};
// Restricted serial profile: one explicitly selected viewport, full refresh of global tags per
// Evaluate, frame-scoped common constants, publicly issued explicit frame index.
// Fixed memory, nonblocking mutex acquisition, no COM calls or file output.
// Token addresses identify a currently issued index, not the constant storage.
// Explicit reissues of the SAME frame can have different addresses. Only an
// exact frame/viewport cache hit may supply constants; never use the last frame.
// Why target calls did not bind, accumulated; readable from any thread. The
// "latest" record alone cannot say whether one cause dominates.
struct BindingStats {
    std::array<std::uint64_t,16> rejections{}; // by Rejection value, index 0 = ready
    std::array<std::uint64_t,5> overlap_with{};// token, constants, tags, evaluate, unknown
    // present_expiries: pinned profile, Presents that expired fresh tags;
    // self-configuring, Presents that arrived while tags were fresh (tolerated).
    // present_expiries_copies: of those, Presents that met a fresh Lab copy and
    // left it fresh (pinned only; a copy is ours, no Present ends it).
    std::uint64_t benign_token_overlaps=0,concurrency_clears=0,present_expiries=0,present_expiries_copies=0,global_tag_calls=0,frame_tag_calls=0;
    std::uint32_t resource_roles=0,resource_causes=0;
    // Tag calls discarded whole: setter failed, real overlap, a call-level
    // decode problem, another viewport (self-configuring: ignored, not dropped).
    std::array<std::uint64_t,4> tag_drops{};
    std::uint64_t tags_isolated=0; // role tags kept although another tag in the call had a problem
    // Self-configuring only. tolerated_overlaps: calls that overlapped another
    // SL call and were used anyway (their data is keyed by frame and viewport).
    // same_key_overlaps: target Evaluates skipped because a setter for THEIR
    // frame and viewport, or a legacy global tag, returned while they ran.
    // lock_waits: a call met the binding lock held and waited instead of being
    // recorded as an observation loss. unkeyed_tag_calls: frame-scoped tag
    // calls whose token index was unknown or found no slot (not stored).
    std::uint64_t tolerated_overlaps=0,same_key_overlaps=0,lock_waits=0,unkeyed_tag_calls=0;
    // At every target Evaluate on our viewport, the tag each role
    // was bound from, by role x source x declared lifecycle. Index
    // role*10+source*3+lifecycle (source 0 inline, 1 per-frame, 2 legacy global;
    // lifecycle 0 OnlyValidNow, 1 ValidUntilPresent, 2 ValidUntilEvaluate);
    // role*10+9: the role had no tag at all. Counted whatever the verdict.
    std::array<std::uint64_t,30> role_lifecycles{};
    std::array<std::uint64_t,3> copies_bound{}; // Lab copies taken at tag time, by role, that an Evaluate bound
    std::uint32_t volatile_roles=0;std::array<std::uint8_t,3> volatile_reasons{}; // latest refusal
    // With frame generation on (e.g. Cyberpunk 2077, DLSS-G): Presents the game's own latency
    // marker declared for a known frame (presents_declared); of those, Presents
    // that would have expired live tags or invalidated the in-flight snapshot but
    // were declared for an EARLIER frame than those belong to, and so expired
    // nothing (present_expiries_spared_previous_frame); and declared Presents that
    // met the binding lock held and were counted as ordinary boundaries instead.
    std::uint64_t presents_declared=0,present_expiries_spared_previous_frame=0,present_attribution_lock_misses=0;
};
// What self-configuration chose; readable from any thread.
struct ViewportSelection {
    bool automatic=false,locked=false;
    std::uint32_t viewport=0;
    std::uint64_t switches=0,other_viewport_calls=0;
};
class Bindings final {
    std::uint32_t viewport_=0;
    sl::BufferType depth_type_=sl::kBufferTypeDepth;
    // SELF-CONFIGURATION (controller host). A package does not state the
    // viewport or the depth semantic; the game's calls do.
    //   Viewport: locked to the viewport of the first RR Evaluate. It moves only
    //   after kSwitchAfter CONSECUTIVE RR Evaluates on other viewports -- an
    //   engine that renumbers its view at a level change -- so a game that
    //   evaluates two views per frame keeps the first one. Constants and tags a
    //   game sets for another viewport carry that viewport's own key in SL too:
    //   they are ignored instead of flushing ours (the pinned profile flushes,
    //   because there any other viewport is a contract violation). The call that
    //   selects or moves the viewport, and a short settling window after it,
    //   report viewport_selected: its inputs predate the lock.
    //   Depth: both advertised semantics fill the depth role, each keeping its
    //   own type so the runtime converts exactly what the game said. When a game
    //   tags both, hardware depth wins -- it needs no projection assumptions.
    // Off (the research host and the pinned profile): unchanged.
    static constexpr std::uint32_t kSwitchAfter=8,kSettle=4;
    bool self_configure_=false,locked_=true;
    // DLSS super resolution as a second target (controller): NR
    // then runs after SR in a game that does not -- or no longer -- use RR.
    // Off by default: the research host's contract is RR only.
    bool sr_target_=false;
    std::uint32_t other_streak_=0,settle_=0;
    std::atomic<std::int64_t> published_viewport_{0};
    std::atomic<std::uint64_t> viewport_switches_{0},other_viewport_calls_{0};
    int role(sl::BufferType type) const noexcept;
    static int depth_rank(sl::BufferType type) noexcept {return type==sl::kBufferTypeDepth?2:type==sl::kBufferTypeLinearDepth?1:0;}
    bool valid_view(const Call& c) const noexcept {return c.inputs.viewport_present&&locked_&&c.inputs.viewport==viewport_;}
    bool other_view(const Call& c) const noexcept {return self_configure_&&c.inputs.viewport_present&&!valid_view(c);}
    // Bounded wait for the binding lock (self-configuring). Every holder only
    // copies fixed-size data, so a short wait is cheap, and a missed lock is an
    // observation loss that clears EVERY cache -- in an engine that calls SL
    // from several threads that cost far more frames than it protected.
    static constexpr unsigned kLockSpins=256;
    std::unique_lock<std::mutex> acquire() noexcept {
        std::unique_lock guard(mutex_,std::try_to_lock);
        if(!guard.owns_lock()&&self_configure_){++lock_waits_;
            for(unsigned spin=0;spin<kLockSpins&&!guard.try_lock();++spin)std::this_thread::yield();}
        return guard;
    }
    std::array<std::atomic<std::uint64_t>,16> rejections_{};
    std::array<std::atomic<std::uint64_t>,5> overlap_with_{};
    std::atomic<std::uint64_t> benign_token_overlaps_{0},concurrency_clears_{0},present_expiries_{0},present_expiries_copies_{0},global_tag_calls_{0},frame_tag_calls_{0};
    std::atomic<std::uint32_t> resource_roles_{0},resource_causes_{0};
    std::array<std::atomic<std::uint64_t>,4> tag_drops_{};std::atomic<std::uint64_t> tags_isolated_{0};
    std::atomic<std::uint64_t> tolerated_overlaps_{0},same_key_overlaps_{0},lock_waits_{0},unkeyed_tag_calls_{0};
    std::array<std::atomic<std::uint64_t>,30> role_lifecycles_{};
    std::array<std::atomic<std::uint64_t>,3> copies_bound_{};
    std::atomic<std::uint32_t> volatile_roles_{0},volatile_reasons_{0}; // reasons packed one byte per role
    std::atomic<std::uint64_t> presents_declared_{0},present_spared_{0},present_attribution_misses_{0};
    void count_lifecycle(unsigned role,unsigned source,const Tag* tag) noexcept {
        if(!tag){++role_lifecycles_[role*10+9];return;}
        const auto l=static_cast<unsigned>(tag->lifecycle);if(l<3)++role_lifecycles_[role*10+source*3+l];
    }
    void count(const Resolution& r,const Call& c) noexcept {
        ++rejections_[std::min<unsigned>(static_cast<unsigned>(r.rejection),15u)];
        if(r.rejection==Rejection::overlap)++overlap_with_[c.overlapped_api<4?c.overlapped_api:4u];
        if(r.rejection==Rejection::resource){resource_roles_=r.resource_roles;resource_causes_=r.resource_causes;}
        if(r.rejection==Rejection::volatile_global||r.rejection==Rejection::volatile_copy){volatile_roles_=r.volatile_roles;
            volatile_reasons_=r.volatile_reasons[0]|std::uint32_t(r.volatile_reasons[1])<<8|std::uint32_t(r.volatile_reasons[2])<<16;}
    }
    struct Token {
        void* pointer=nullptr;std::uint32_t index=0;
        std::uint64_t generation=0,constants_call=0,seen=0;
        bool known=false;
        ConstantsState constants_state=ConstantsState::never_set;
    };
    struct Common {
        std::uint32_t frame=0,viewport=0,thread=0;
        std::uint64_t call=0;
        bool occupied=false,fresh=false;
        sl::Constants constants{};
    };
    // frame / frame_known: the frame this tag belongs to as far as this layer can
    // derive it (present_boundary_of_frame). A per-frame tag: its key. A legacy
    // global tag: the frame after the one whose target Evaluate last consumed the
    // globals (next_frame_), unknown before that, after a cache clear or after a
    // non-target Evaluate consumed them. The derivation never runs AHEAD of the
    // frame a tag really serves, except for a tag set after an Evaluate for that
    // same Evaluate's frame -- which the profile's one-refresh-per-Evaluate rule
    // already assumes away (every target Evaluate consumes the globals).
    struct Global {Tag tag{};std::uint64_t call=0;std::uint32_t thread=0;bool fresh=false;std::uint32_t frame=0;bool frame_known=false;};
    std::array<Token,16> tokens_{};std::uint64_t token_seen_=0;
    // Matches the reviewed common-constants queue bound. Evicted/missing data
    // is rejected even where SL itself might fall back to last-set constants.
    // Self-configuring, sixteen frames may be in flight: an engine that sets
    // constants further ahead than three frames is no reason to refuse it.
    std::array<Common,16> common_{};
    unsigned common_limit() const noexcept {return self_configure_?16u:3u;}
    std::array<Global,3> globals_{};
    struct FrameTags {std::uint32_t frame=0,viewport=0;bool occupied=false;std::array<Global,3> resources{};};
    std::array<FrameTags,16> frame_tags_{};
    // Atomic only so stores_tag_call() may read it outside the lock; every
    // writer still holds mutex_.
    std::atomic<bool> frame_tags_seen_{false};
    std::mutex mutex_;
    std::atomic<std::uint64_t> present_{0},loss_{0},hard_{0};
    std::uint64_t seen_present_=0,seen_loss_=0,seen_hard_=0,generation_=0;
    Resolution pending_{};
    std::uint64_t pending_call_=0;
    // The frame the in-flight snapshot belongs to: the Evaluate's token index,
    // lowered to the derived frame of any non-local tag it bound; unknown when the
    // token or one of those frames is. And the frame a legacy global tag stored
    // from now on serves (see Global). Both under mutex_.
    std::uint32_t pending_frame_=0,next_frame_=0;
    bool pending_frame_known_=false,next_frame_known_=false;
    // The lowest frame of what a Present could end right now: the in-flight
    // snapshot and every fresh tag. False when nothing is live or any live item's
    // frame is unknown. Under mutex_.
    bool live_frame(std::uint32_t& lowest) const noexcept;
    void clear_cache() noexcept;
    void sync() noexcept;
    Token* token(void*) noexcept;
    Common* common(std::uint32_t frame,std::uint32_t viewport) noexcept;
    Common& common_slot(std::uint32_t frame,std::uint32_t viewport) noexcept;
    void remember_token(const Call&) noexcept;
    FrameTags* frame_tags(std::uint32_t frame,std::uint32_t viewport) noexcept;
    FrameTags* frame_tag_slot(std::uint32_t frame,std::uint32_t viewport) noexcept;
    void expire_resources() noexcept;
    // Self-configuring: after a discarded tag call, doubt only what that call
    // could have replaced -- its own frame slot, or the legacy global set.
    void expire_call_target(const Call&) noexcept;
public:
    // Whether an overlap with another SL call makes this call's data unusable.
    // Pinned profile: any overlap does. Self-configuring: none does by itself.
    // A token fetch only maps a pointer to an index; a setter stores under its
    // own frame and viewport key; the target Evaluate froze its inputs at entry
    // and revalidates the token generation and constants revision at return.
    // What CAN still mislead us -- a setter for the SAME frame and viewport, or
    // a legacy global tag (which has no key), returning while our Evaluate is
    // in flight -- is refused exactly there (returned(), same_key_overlaps).
    // A nested call stays an overlap either way.
    bool conflicting(const Call& c) const noexcept {return c.concurrent&&!self_configure_;}
    // Configure only before attaching callbacks. No runtime viewport migration.
    void select_viewport_before_attach(std::uint32_t value) noexcept {viewport_=value;published_viewport_=value;}
    // Controller host: read the viewport and depth semantic from the game's own
    // calls instead of the package (see the members above). Before attach only.
    void self_configure_before_attach() noexcept {self_configure_=true;locked_=false;published_viewport_=-1;}
    void target_super_resolution_before_attach() noexcept {sr_target_=true;}
    bool targets_super_resolution() const noexcept {return sr_target_;}
    bool self_configured() const noexcept {return self_configure_;}
    bool target(sl::Feature f) const noexcept {return f==sl::kFeatureDLSS_RR||(sr_target_&&f==sl::kFeatureDLSS);}
    BindingStats stats() const noexcept {
        BindingStats s;for(unsigned i=0;i<16;++i)s.rejections[i]=rejections_[i].load();for(unsigned i=0;i<5;++i)s.overlap_with[i]=overlap_with_[i].load();
        s.benign_token_overlaps=benign_token_overlaps_.load();s.concurrency_clears=concurrency_clears_.load();s.present_expiries=present_expiries_.load();
        s.present_expiries_copies=present_expiries_copies_.load();
        s.global_tag_calls=global_tag_calls_.load();s.frame_tag_calls=frame_tag_calls_.load();
        s.resource_roles=resource_roles_.load();s.resource_causes=resource_causes_.load();
        for(unsigned i=0;i<4;++i)s.tag_drops[i]=tag_drops_[i].load();s.tags_isolated=tags_isolated_.load();
        s.tolerated_overlaps=tolerated_overlaps_.load();s.same_key_overlaps=same_key_overlaps_.load();
        s.lock_waits=lock_waits_.load();s.unkeyed_tag_calls=unkeyed_tag_calls_.load();
        for(unsigned i=0;i<30;++i)s.role_lifecycles[i]=role_lifecycles_[i].load();for(unsigned i=0;i<3;++i)s.copies_bound[i]=copies_bound_[i].load();
        s.volatile_roles=volatile_roles_.load();const auto packed=volatile_reasons_.load();
        for(unsigned i=0;i<3;++i)s.volatile_reasons[i]=static_cast<std::uint8_t>(packed>>(8*i));
        s.presents_declared=presents_declared_.load();s.present_expiries_spared_previous_frame=present_spared_.load();
        s.present_attribution_lock_misses=present_attribution_misses_.load();return s;
    }
    // The role a tag type fills for NR (-1 none, 0 output colour, 1 depth, 2
    // motion). Fixed before attach, so safe from any thread.
    int role_of(sl::BufferType type) const noexcept {return role(type);}
    // Self-configuring, the depth semantic a call's tag keeps over another of the
    // depth role (hardware 2 over linear 1), as returned() applies it.
    static int depth_rank_of(sl::BufferType type) noexcept {return depth_rank(type);}
    // Whether a returned tag call is one this layer would store as tags of our
    // viewport -- result, overlap, call-level decode and viewport only, read
    // without the lock. A copy made for a call that is then dropped is wasted,
    // never bound: this is an economy, not an admission rule.
    bool stores_tag_call(const Call&) const noexcept;
    // The adapter's verdict on a Lab copy in this frozen Evaluate (it owns the
    // copies): the binding becomes volatile_copy with these roles and reasons.
    // A missed lock is an observation loss, which refuses the call as stale.
    void refuse_entry(std::uint64_t call,std::uint32_t roles,const std::array<std::uint8_t,3>& reasons) noexcept;
    ViewportSelection viewport_selection() const noexcept {
        ViewportSelection s;s.automatic=self_configure_;const auto v=published_viewport_.load();
        s.locked=v>=0;s.viewport=v>=0?static_cast<std::uint32_t>(v):0u;
        s.switches=viewport_switches_.load();s.other_viewport_calls=other_viewport_calls_.load();return s;
    }
    // Select the exact advertised semantic, never infer depth from format/size
    // or silently substitute linear depth for hardware depth.
    void select_linear_depth_before_attach(bool value) noexcept {depth_type_=value?sl::kBufferTypeLinearDepth:sl::kBufferTypeDepth;}
    // Must be wired to actual host boundaries; not a timer or guessed frame ID.
    // Conservative invalidation also supports a callback from another thread.
    void present_boundary() noexcept {++present_;}
    // A host boundary that is not a Present (swapchain resize or recreation,
    // adapter stop): a Present everywhere, and it also ends a call in flight.
    void hard_boundary() noexcept {++hard_;++present_;}
    // The same boundary, for a Present the
    // game's OWN latency marker declares for `frame` (an open ePresentStart on the
    // Present thread whose token address was seen issued with that index;
    // chain::PresentMarker::declares_frame). With frame generation the game tags
    // frame N -- and may already be inside Evaluate N -- before it presents frame
    // N-1; a Present of an EARLIER frame than everything live (the in-flight
    // snapshot and every fresh tag, live_frame) ends none of it, so it expires
    // nothing, invalidates nothing and returns true (counted; a pending call still
    // counts it in presents_during_call). Anything else -- the same or a later
    // frame, a live item whose frame is unknown, nothing live -- is exactly
    // present_boundary() and returns false. Never waits: a held binding lock makes
    // it an ordinary boundary (counted). Callable from any thread.
    bool present_boundary_of_frame(std::uint32_t frame) noexcept;
    void entering(const Call&) noexcept;
    Resolution frozen_entry(const Call&) noexcept;
    Resolution returned(const Call&) noexcept;
    void aborted(const Call&) noexcept;
};
}
