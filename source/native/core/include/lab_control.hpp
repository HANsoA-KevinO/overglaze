// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include "lab_manual_capture.hpp"
#include "lab_nr_mode.hpp"
#include <map>
#include <mutex>
#include <optional>
#include <functional>

namespace lab {
// No rendering/NGX calls here. Capture delivery callbacks only submit work to
// the bridge; backend acknowledgements arrive separately on frame boundaries.
class Controller {
public:
    explicit Controller(bool synthetic = false);
    json handle(const json& request, std::uint64_t now_ms);
    // In-process only. Pipe requests cannot impersonate the embedded writer.
    void enable_embedded_control();
    json handle_embedded(const std::string& method,const json& params,std::uint64_t now_ms);
    void frame_boundary(std::uint64_t frame, std::uint64_t now_ms);
    json status() const;
    void diagnostic(const std::string& text);
    // Internal observer publication only. Never accepted from a control request.
    void publish_observer(json snapshot);
    // Read-only admission diagnostics, never a capability-enabling request.
    void publish_nr_adapter(json snapshot);
    void publish_nr_runtime(json snapshot);
    void interrupt_nr_for_rebuild(std::uint64_t serial,std::uint64_t frame);
    void publish_host(json snapshot);
    // Explicit diagnostic process: no NR preparation/ON, no texture capture.
    void enable_post_inspection(std::function<bool(bool cancel,std::uint64_t)>);
    void publish_post_inspection(json);
    void enable_access_inspection(std::function<bool(bool cancel,std::uint64_t)>);
    void publish_access_inspection(json);
    void enable_binding_probe(std::function<bool(bool)>);
    void publish_binding_probe(json);
    void enable_preparation_probe(std::function<bool(bool)>);
    void publish_preparation_probe(json);
    void enable_binding_boundary_inspection(std::function<bool(bool)>);
    void publish_binding_boundaries(json);
    // Explicit product preparation, separate from observer sampling and NR ON.
    void enable_nr_preparation(bool integrated=false);
    // Per-game default for the host colour-preparation exposure (log2 stops).
    // Only before any settings request; it is a wrapper default, not a model value.
    void set_default_exposure_stops(float stops);
    // The identity-validated installation's profile id (V3 data-driven games
    // have no compiled row). Once per process; never from a pipe request.
    void accept_game_profile(const std::string& id);
    void enable_nr_compute_only(); // Internal backend capability; never a pipe request.
    bool take_nr_preparation(std::uint64_t now_ms);
    // Late loading only. The panel must draw on the game's own queue, and
    // this process can only learn that queue from the runtime's submission
    // probe, so there is no panel until the runtime has been prepared once.
    // Injection is itself the operator's explicit action, so the late host
    // asks once, on its own, and NR stays OFF. Returns false unless the
    // preparation is sitting in standby with no runtime and no ON intent.
    bool request_late_attach_preparation();
    // Explicit one-shot measurement arming, not NR/game settings control.
    void enable_observer_sampling();
    std::optional<json> take_observer_sampling_request(std::uint64_t now_ms);
    void acknowledge_observer_sampling(json applied);
    void enable_manual_capture(const std::filesystem::path& root,const std::string& origin);
    void observe_present(std::uint64_t swapchain) noexcept;
    void stop_manual_capture(const std::string& reason);
    // A real, externally acknowledged NR frame backend, never the synthetic
    // timer-based ApplyConfig path. First consumer is an isolated real-NR host.
    void enable_nr_frame_control(const std::string& origin,bool settings=false,bool pair=false);
    // Immediate-accept fixtures only. Production uses dispatch_* below, never
    // take-then-store/retry in another queue (which defeats cancellation).
    std::optional<json> take_pair_request(std::uint64_t now_ms);
    // Production delivery stays in this mailbox until the non-blocking bridge
    // accepts it. Callback must not reenter Controller, render or wait for GPU.
    // false means NOT handled (no work started), not a failed capture to retry.
    bool dispatch_pair_request(std::uint64_t now_ms,const std::function<bool(const json&)>&);
    void publish_pair(json snapshot);
    // Called when the research bridge exports a chain capture (one same-call pair plus
    // the reconstructor inputs and the Presents after it). Lets CapturePair
    // carry an optional scope; never a pipe request.
    void enable_chain_capture();
    void enable_display_capture();
    std::optional<json> take_display_capture_request(std::uint64_t now_ms);
    bool dispatch_display_capture_request(std::uint64_t now_ms,const std::function<bool(const json&)>&);
    void publish_display_pair(json snapshot);
    std::optional<json> take_nr_mode_request(std::uint64_t now_ms);
    void acknowledge_nr_mode(std::uint64_t revision,std::uint64_t frame,bool success,
                             bool nr_evaluated,bool nr_output_selected,const std::string& error = {});
private:
    json handle_impl(json request,std::uint64_t now_ms,bool embedded);
    void release_owner_locked(const char* reason);
    json status_locked() const;
    void expire(std::uint64_t now_ms);
    void fail_nr_locked(const std::string& reason);
    bool dispatch_capture_locked(bool display,const std::function<bool(const json&)>&);
    void queue_nr_intent_locked(const char* reason);
    void set_nr_intent_locked(unsigned on,const std::string& id);
    // The only two places display capture may write NR preparation state.
    void display_pair_prepare_locked(const json& request_id);
    void display_pair_release_preparation_locked();
    mutable std::mutex mutex_;
    const bool synthetic_;
    const std::string session_;
    std::string owner_, state_ = "idle", last_error_, accepted_profile_;
    std::uint64_t revision_ = 0, applied_revision_ = 0, applied_frame_ = 0, last_seen_ = 0;
    json requested_ = {{"nr_mode", "off"}}, observed_ = {{"nr_mode", "unknown"}};
    std::optional<json> pending_;
    json observer_ = nullptr;
    json nr_adapter_ = nullptr;
    json nr_runtime_ = nullptr;
    json host_ = nullptr;
    json post_inspection_=nullptr;
    std::function<bool(bool,std::uint64_t)> post_inspection_action_;
    json access_inspection_=nullptr;
    std::function<bool(bool,std::uint64_t)> access_inspection_action_;
    std::function<bool(bool)> binding_probe_action_;
    json binding_probe_=nullptr;
    std::function<bool(bool)> preparation_probe_action_;
    json preparation_probe_=nullptr;
    std::function<bool(bool)> binding_boundary_action_;
    json binding_boundaries_=nullptr;
    json binding_boundary_summary_=nullptr; // No rows on the frequently polled control channel.
    bool binding_probe_consumed_=false;
    json nr_preparation_ = nullptr;
    bool nr_integrated_=false,nr_compute_only_=false;
    unsigned nr_desired_on_=0; // Mode value, not a Boolean.
    bool embedded_enabled_=false;
    std::uint64_t embedded_sequence_=0,settings_revision_=0;
    json desired_settings_={{"tone",1.0f},{"structure",1.0f},{"style",0u},{"exposure_stops",0.0f},{"exposure_auto",0u},{"compare_split",0u},{"skin",1.0f},{"automask",0u},
        {"extrapolate",0u},{"extrapolate_factor",2.0f}};
    bool nr_intent_dirty_=false;
    std::shared_ptr<ManualCapture> capture_;
    std::string nr_origin_;
    bool nr_settings_enabled_=false;
    bool nr_pair_enabled_=false,chain_capture_enabled_=false;
    json pair_status_=nullptr;
    std::optional<json> pair_pending_;
    bool display_pair_enabled_=false;
    json display_pair_status_=nullptr;
    std::optional<json> display_pair_pending_;
    bool nr_terminal_=false; // Sticky until a new Controller/backend session.
    std::uint64_t nr_rebuild_serial_=0;
    std::string nr_terminal_reason_;
    std::optional<json> nr_pending_,nr_inflight_;
    json nr_status_ = nullptr;
    bool observer_sampling_enabled_=false,observer_sampling_consumed_=false;
    std::optional<json> observer_sampling_pending_;
    json observer_sampling_={{"state","disabled"}};
    std::map<std::string, std::pair<std::string, json>> responses_;
};
}
