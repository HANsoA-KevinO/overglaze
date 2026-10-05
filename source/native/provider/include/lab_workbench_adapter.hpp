// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_sl_bindings.hpp"
#include "lab_platform.hpp"
#include "lab_rr_options.hpp"
#include "lab_sl_host_contract.hpp"
#include "lab_workbench_extension.hpp"
#include "lab_sl_resource_lease.hpp"
#include "lab_sl_volatile_copy.hpp"
#include "lab_sl_admission_sink.hpp"
#include "lab_research_observer.hpp"
#include "lab_latency_markers.hpp"
#include "lab_game_profile.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace lab {
enum class WorkbenchScope { full_metadata, public_topology_only };
// Read-only admission by default; optional controlled receiver is called only
// under the explicit host-rebind contract, after a complete matching Evaluate.
// Optional D3D12 descriptor inspection holds CPU-call COM refs; worker status
// contains POD metadata, never borrowed COM pointers for asynchronous use.
class WorkbenchAdapter final:public slboundary::Sink {
    friend struct WorkbenchAdapterTestAccess;
    slboundary::Bindings bindings_;
    std::uint32_t selected_viewport_=0; // reporting only; admission stays in bindings_
    bool self_configured_=false;         // reporting only; the binding layer decides
    mutable std::mutex mutex_;
    // Every profile: a game thread that meets mutex_ held (the 16 ms status
    // snapshot, or another game thread) waits a bounded moment instead of
    // losing the call. Every holder copies fixed-size data only; snapshot()
    // does no allocation or COM release under it.
    std::unique_lock<std::mutex> game_lock() const noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()){++lock_waits_;
            for(unsigned spin=0;spin<256&&!lock.try_lock();++spin)std::this_thread::yield();}
        return lock;
    }
    mutable std::atomic<std::uint64_t> lock_waits_{0};
    std::atomic<bool> stopped_{false},multiple_chains_{false};
    std::atomic<std::uint64_t> chain_{0},previous_chain_{0},presents_since_switch_{0},presents_{0},epoch_{0},lost_{0};
    std::array<std::atomic<std::uint64_t>,4> calls_{};
    rr::Watch* options_=nullptr; // Set before attaching; process-pinned owner.
    rr::Watch* sr_options_=nullptr; // SR setter watch; only when SR is a target
    rr::Watch* pending_options_watch_=nullptr; // the watch pending_options_ came from
    rr::Packet pending_options_;
    std::uint64_t pending_options_call_=0;
    std::uint64_t pending_loss_epoch_=0; // Loss invalidates its invocation, not every future frame.
    bool inspect_resources_=false;
    std::atomic<PFun_slGetNativeInterface*> native_interface_{nullptr};
    // Default REQUIRES the reviewed inner evidence. Only a host that has
    // positively established that this Streamline build has no reviewed
    // begin/end profile relaxes it to public evidence.
    std::atomic<bool> inner_admission_required_{true};
    std::atomic<const void*> host_flags_address_{nullptr};
    std::atomic<bool> native_evaluate_contract_{false};
    slhost::HostContract pending_host_contract_;
    struct InnerLatest {std::uint64_t call=0;unsigned frame=0,viewport=0;bool sequence=false,metadata=false,native_command=false;
        bool public_command_native=false,restore_matched=false,outer_succeeded=false;unsigned restore_result=UINT32_MAX;
        const char* reason="not-observed";} inner_latest_;
    std::uint64_t inner_candidates_=0,inner_rejected_=0;
    slboundary::ResourceLease pending_resources_;
    // The matching return/abort clears this even when the metadata mutex is
    // busy. Only then may a later entry/worker reclaim the fixed CPU lease.
    std::atomic<std::uint64_t> active_resource_call_{0};
    SlAdmissionSink* live_=nullptr; // fixed before hooks attach, process-pinned
    IResearchObserver* research_=nullptr; // null in the controller: nothing extra is recorded
    // eOnlyValidNow depth/motion copied at their tag call (the class
    // header has the rules). Idle and allocation-free unless the host worker
    // says frames are wanted; full-metadata adapters only.
    slboundary::VolatileCopies copies_;
    // The game's own latency markers (lab_latency_markers.hpp), owned
    // by the host for the life of the process. Fed every successful token return;
    // read at every Present so a Present the game declares for an earlier frame
    // expires nothing of the frame being tagged or evaluated. Null: unchanged.
    std::atomic<chain::LatencyMarkers*> markers_{nullptr};
    // Verify every Lab copy a frozen Evaluate binds; refuse the call by name otherwise.
    void verify_copies(const slboundary::Call& c,void* native_list) noexcept;
    void finish_resource_call(std::uint64_t call) noexcept;
    void binding_boundary(const slboundary::Call&,diagnostic::BindingPoint,const slboundary::InnerCall* =nullptr) noexcept;
    struct Latest {
        std::uint64_t call=0,token_generation=0,constants_call=0,epoch=0,loss_epoch=0;
        unsigned frame=0,viewport=0,thread=0;
        slboundary::Rejection reason=slboundary::Rejection::not_target;
        std::uint32_t invalidations=0,presents_during_call=0;
        // slboundary::CrossThreadSource bits carried through for the status.
        std::uint32_t cross_thread_sources=0;
        std::array<unsigned,3> states{},width{},height{};
        std::array<std::uint64_t,3> tag_calls{};
        std::array<bool,3> local{};
        float mvec_x=0,mvec_y=0,jitter_x=0,jitter_y=0;
        bool constants_copied=false;
        sl::Constants constants;
        std::array<unsigned,3> resource_types{};
        unsigned local_tag_count=0,input_issues=0;
        // What the game actually asked for, so a viewport refusal can be read
        // instead of guessed. Never used to admit anything.
        std::uint32_t observed_viewport=0;bool observed_viewport_present=false;
        // Diagnostic POD only: no resource addresses or pixel readback. Capture
        // even rejected invocations so a missing role does not erase its cause.
        std::array<std::uint32_t,16> local_tag_types{},local_tag_issues{};
        std::uint32_t local_null_mask=0;
        rr::Packet options;
        slboundary::ResourceFacts resources;
        sl::Feature feature=sl::kFeatureDLSS_RR;
        // The admission verdict for this call: which check refused it, or
        // "admitted". Kept because the runtime records nothing while it waits
        // for its first frame, and "pairing unavailable" named no check.
        char admission_stage[48]{},admission_reason[160]{};
        // 0 native list, 1 an SL proxy admitted through its native list, 2 neither.
        unsigned command_kind=2;
        unsigned lab_copies=0; // roles (bit 1 depth, 2 motion) bound as Lab copies taken at the tag call
    } latest_;
    std::uint64_t admitted_calls_=0;
    std::uint64_t candidates_=0,rejected_=0;
    // Public-boundary record of the OTHER upscaler feature (SR, kFeatureDLSS).
    // Kept whether or not SR is a target: where it is not (the research host),
    // an SR Evaluate never takes the resource lease, never freezes tags and
    // never reaches admission, and this record is how a game that runs SR
    // without RR is recognized instead of silently waiting forever. Where SR IS
    // a target (the controller) the same call also goes through admission.
    // Its own mutex keeps the admission path uncontended.
    mutable std::mutex upscaler_mutex_;
    struct TagsSeen { // most recent slSetTag/slSetTagForFrame call, ANY feature
        std::uint64_t call=0;std::uint32_t count=0,viewport=UINT32_MAX,thread=0;
        std::array<std::uint32_t,8> types{},widths{},heights{},states{},issues{},resource_types{},lifecycles{};
        std::uint32_t null_mask=0,call_issues=0,all_issues=0,result=0;
    } tags_;
    struct UpscalerSeen {
        std::uint64_t evaluates=0,succeeded=0,call=0;
        std::uint32_t frame=0,viewport=UINT32_MAX,thread=0,inline_tag_count=0,input_issues=0;
        bool frame_known=false,constants_present=false,command_present=false,result_ok=false;
        TagsSeen tags{}; // last tag call observed BEFORE this Evaluate returned
        bool tags_same_thread=false;
    } upscaler_;
    void note_upscaler_call(const slboundary::Call& c) noexcept;
    void note_tag_call(const slboundary::Call& c) noexcept;
public:
    explicit WorkbenchAdapter(rr::Watch* options=nullptr,bool inspect_resources=false,SlAdmissionSink* live=nullptr,IResearchObserver* research=nullptr)
        :options_(options),inspect_resources_(inspect_resources),live_(live),research_(research){}
    void entering(const slboundary::Call& c) noexcept override;
    void aborted(const slboundary::Call& c) noexcept override;
    void returned(const slboundary::Call& c) noexcept override;
    void inner_returned(const slboundary::Call&,const slboundary::InnerCall&) noexcept override;
    void restore_returned(const slboundary::Call&,const slboundary::RestoreCall&) noexcept override;
    void restore_entering(const slboundary::Call&,const slboundary::RestoreCall&) noexcept override;
    void set_native_interface(PFun_slGetNativeInterface* f) noexcept {native_interface_=f;}
    void select_viewport_before_attach(std::uint32_t value) noexcept {selected_viewport_=value;bindings_.select_viewport_before_attach(value);}
    // The viewport this adapter package admits, kept so a refusal can name both
    // what the game asked for and what we accept. Reporting only.
    std::uint32_t selected_viewport() const noexcept {return selected_viewport_;}
    void select_linear_depth_before_attach(bool value) noexcept {bindings_.select_linear_depth_before_attach(value);}
    void self_configure_before_attach() noexcept {self_configured_=true;bindings_.self_configure_before_attach();
        if(options_)options_->relax_before_install();if(sr_options_)sr_options_->relax_before_install();}
    void target_super_resolution_before_attach(rr::Watch* sr_options) noexcept {sr_options_=sr_options;bindings_.target_super_resolution_before_attach();
        if(sr_options_&&self_configured_)sr_options_->relax_before_install();}
    slboundary::ViewportSelection viewport_selection() const noexcept {return bindings_.viewport_selection();}
    // One rule for both layers (lab_sl_bindings.hpp): self-configuring, an
    // overlap alone never disqualifies a call.
    bool conflicting(const slboundary::Call& c) const noexcept {return bindings_.conflicting(c);}
    // Self-configuring, more than one swapchain is not a reason to refuse: NR is
    // recorded inside the Evaluate on its own command list, whatever presents.
    bool chains_ambiguous() const noexcept {return multiple_chains_&&!self_configured_;}
    // Host discovery sets this once, only after exact common identity/pinning.
    void set_host_flags_address(const void* p) noexcept {host_flags_address_=p;}
    void set_native_evaluate_contract(bool value) noexcept {native_evaluate_contract_=value;}
    // True once the reviewed RR begin/end hooks are installed. Then that
    // stronger evidence stays REQUIRED for admission of an RR call. False (no
    // reviewed profile for this Streamline build) admits on public evidence only.
    // Per feature: a super-resolution call is always admitted on
    // the public proof, because no reviewed private evidence exists for SR.
    void set_inner_admission_required(bool value) noexcept {inner_admission_required_=value;}
    bool inner_admission_required() const noexcept {return inner_admission_required_.load();}
    // Fixed before hooks attach, like the constructor argument it replaces.
    void set_admission_sink(SlAdmissionSink* sink) noexcept {live_=sink;}
    // Fixed before hooks attach; the host owns the instance for the process.
    void set_latency_markers(chain::LatencyMarkers* markers) noexcept {markers_=markers;}
    // Conservative invalidation at the selected host's pre-Present boundary. Not a
    // successful native Present, frame classification, or completion signal.
    // Called on the Present thread itself: with latency markers set, this
    // thread's open ePresentStart decides the frame the Present is declared for
    // (slboundary::Bindings::present_boundary_of_frame).
    void present(std::uint64_t swapchain) noexcept;
    void invalidate() noexcept {++epoch_;bindings_.hard_boundary();}
    void stop() noexcept {stopped_=true;copies_.stop();++epoch_;bindings_.hard_boundary();}
    // Host worker, every poll, both tracks: whether the NR runtime (or an armed
    // research capture) would use an admitted frame now. Only then are
    // eOnlyValidNow input tags copied; otherwise they stay refused by name.
    void service_copies(bool wanted,std::uint64_t now_ms) noexcept {copies_.service(wanted&&inspect_resources_&&!stopped_,now_ms);}
    json snapshot(); // Worker only: short POD copy, JSON/COM release outside lock.
};

// Worker-owned discovery; only fixed already-loaded interposer is admitted.
// No LoadLibrary of SL/NR and no calls into SL during discovery.
class WorkbenchAdapterHost final:WorkbenchResearchHost {
    rr::Watch options_;
    rr::Watch sr_options_{true};
    // The reviewed private callbacks and the common restore hook, or null in
    // the controller, which then admits on public evidence only.
    WorkbenchResearchExtension* research_=nullptr;
    WorkbenchAdapter adapter_;
    WorkbenchScope scope_=WorkbenchScope::full_metadata;
    bool enabled_=false,attempted_=false,installed_=false,stopped_=false;
    bool options_attempted_=false,options_installed_=false;
    bool sr_target_=false,sr_options_attempted_=false,sr_options_installed_=false;
    ULONGLONG next_sr_options_scan_=0;
    json sr_options_module_=nullptr,sr_options_hooks_=json::object();
    std::string sr_options_error_;
    void poll_sr_options();
    ULONGLONG next_options_scan_=0;
    std::string options_resolution_="unresolved";
    json options_module_=nullptr,options_hooks_=json::object();
    bool inner_installed_=false;
    std::string options_error_;
    void poll_options();
    // Public slGetFeatureFunction path. Returns false while the feature is
    // not resolvable yet (keep polling), true once a decision was reached.
    bool attach_public_options();
    // WorkbenchResearchHost: what the private discovery may report back.
    void set_host_flags_address(const void* flags) noexcept override {adapter_.set_host_flags_address(flags);}
    void set_native_evaluate_contract(bool value) noexcept override {adapter_.set_native_evaluate_contract(value);}
    bool game_native_evaluate_host_rebind()const noexcept override {return facts_.native_evaluate_host_rebind;}
    json module_=nullptr,hooks_=json::object();
    std::string error_;
    std::string game_profile_="cyberpunk2077-rr-v1";
    profiles::Facts facts_=profiles::Facts::from(profiles::games[0]);
public:
    explicit WorkbenchAdapterHost(bool enabled,SlAdmissionSink* live=nullptr,WorkbenchScope scope=WorkbenchScope::full_metadata,IResearchObserver* research=nullptr)
        :adapter_(scope==WorkbenchScope::full_metadata?&options_:nullptr,enabled&&scope==WorkbenchScope::full_metadata,live,research),scope_(scope),enabled_(enabled){}
    void poll();
    // Set by the identity-validated installation before any hook discovery.
    // Compiled row by id (fixtures, legacy) or the installation's runtime facts.
    void set_game_profile(std::string_view profile);
    void set_game_facts(const profiles::Facts& facts);
    // Controller host: the SL viewport and depth semantic come from the game's
    // calls, not the package. Before the first poll()/attach only.
    void self_configure_before_attach();
    slboundary::ViewportSelection viewport_selection() const noexcept {return adapter_.viewport_selection();}
    // Controller host: DLSS super resolution is a target too, so NR runs after
    // SR where a game does not use RR. Before the first poll()/attach only.
    void target_super_resolution_before_attach();
    // Research hosts attach the private discovery before the first poll().
    void set_research_extension(WorkbenchResearchExtension* research) noexcept {research_=research;}
    // Same contract: set before the first poll(), never while hooks are live.
    void set_admission_sink(SlAdmissionSink* sink) noexcept {adapter_.set_admission_sink(sink);}
    // Same contract: the host's process-lifetime marker observer, before the first poll().
    void set_latency_markers(chain::LatencyMarkers* markers) noexcept {adapter_.set_latency_markers(markers);}
    // Only the isolated host validates and invokes this entry. Game discovery
    // never accepts user-supplied SL RVAs or replacement public callbacks.
    bool attach_fixture(const std::array<void*,4>& api,void* options,const std::array<void*,2>& inner,const void* flags,bool frame_tagging=false,void* extra_frame_tag=nullptr,bool native_evaluate_contract=false,PFun_slGetNativeInterface* native_interface=nullptr,void* sr_options=nullptr);
    void present(std::uint64_t chain) noexcept {if(enabled_)adapter_.present(chain);}
    void invalidate() noexcept {if(enabled_)adapter_.invalidate();}
    // Worker: see WorkbenchAdapter::service_copies. Called whether or not the
    // hooks are a fixture's, so the synthetic hosts exercise the same path.
    void service_tag_copies(bool wanted) noexcept {if(enabled_)adapter_.service_copies(wanted&&installed_&&!stopped_,GetTickCount64());}
    void stop(); // worker; sink remains pinned for late API returns
    json snapshot(); // bounded: see bound_adapter_snapshot
};
// The controller publishes the adapter snapshot only up to a fixed byte bound
// (Controller::publish_nr_adapter). A verbose diagnostic -- long module paths,
// a populated SR record, guide constants, install diagnostics -- must never
// turn into a host failure at runtime, so optional detail is trimmed in a
// fixed, least-valuable-first order until the dump fits. Admission keys
// (mode / game_control_available / render_admission / nr_executed / latest
// binding result / admission_mode) are never touched; "trimmed" lists what
// was removed.
inline constexpr std::size_t adapter_snapshot_limit=8*1024;
json bound_adapter_snapshot(json snapshot,std::size_t limit);
}
