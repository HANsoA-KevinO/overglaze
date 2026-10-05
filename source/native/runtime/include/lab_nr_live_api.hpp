// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <type_traits>
#include "lab_nr_settings.hpp"
#include "lab_rejected_call.hpp"
#include "lab_depth_projection.hpp"
namespace lab::live {
// Our own versioned ABI between the same-build lab host and lab-prefixed NGX
// bridge. No SL/NGX/private game structures, STL containers, JSON or exceptions
// cross this interface. Image data stays on the GPU.
//
// Since ABI20 this carries the NR runtime only: mode,
// settings, extents, completion, rejection, binding preservation. Every
// research collector (frame capture, display pair capture, binding and
// preparation probes, boundary audit) lives in the separately versioned
// lab_nr_live_research_api.hpp, which only the research bridge implements.
// A host asks LabNrLiveCapabilities which variant it loaded before using them.
inline constexpr unsigned version=23; // 23: frame regions (crop) and display-resolution motion; 22: Tone/Structure 0..2, Skin + UseAutoMask in Settings and read receipts; 13: exposure_stops; 14: skip counters; 15: auto exposure; 16: compare split, unlimited skips; 17: history gaps; 18: capture collector availability; 19: discarded recordings, submission skips; 20: research collectors split out, capabilities; 21: render-queue handoff, named completion-signal reasons, signal_retries
enum class State:unsigned {waiting_frame,probing_queue,preparing,ready,failed,stopped,draining,waiting_rebuild_frame};
struct Frame {
    unsigned size=sizeof(Frame),abi=version;
    std::uint64_t call=0,frame=0;
    ID3D12GraphicsCommandList* command=nullptr;
    ID3D12Resource* color=nullptr;ID3D12Resource* depth=nullptr;ID3D12Resource* motion=nullptr;
    unsigned width=0,height=0,guide_width=0,guide_height=0;
    unsigned color_state=0,depth_state=0,motion_state=0;
    float motion_x=0,motion_y=0;
    unsigned depth_inverted=0,reset=0;
    // 0: legacy R32_TYPELESS/RGBA16F guides; 1: typed R32F/RG16F.
    unsigned packed_guides=0;
    nr::DepthEncoding depth_encoding=nr::DepthEncoding::hardware;
    nr::DepthProjection depth_projection;
    // ABI23 (first needed by Hellblade 2). region_crop=1: width x height and
    // guide_width x guide_height name the TOP-LEFT region of the colour and
    // depth resources that holds the image; the resources may be larger (a
    // letterboxed 5120x2142 output in a 5120x2144 texture, a 2970x1243 render
    // subrect in a 2972x1256 depth). The runtime copies that region in and
    // writes the colour back into the same region; nothing outside it is read
    // or written. 0: every extent equals its resource, exactly as before.
    unsigned region_crop=0;
    // ABI23. Motion covering the same view at ANOTHER resolution: the top-left
    // motion_width x motion_height region of the motion resource (display-
    // resolution motion, NGX created without MVLowRes). The runtime resamples it
    // onto the guide grid; motion_x/motion_y stay the scale to pixels of the
    // motion resource as given, and the runtime rescales them to the guide grid.
    // 0: motion is on the guide grid (the region, or the whole resource).
    unsigned motion_width=0,motion_height=0;
};
struct Status {
    unsigned size=sizeof(Status),abi=version;State state=State::waiting_frame;
    // ABI12: modes 0=OFF, 1=execute+select, 2=execute without game writeback.
    unsigned requested_on=0,observed_on=0,pending_gpu=0;
    std::uint64_t frames=0,evaluates=0,off_frames=0,retired=0,waits=0,wait_ms=0;
    std::uint64_t request_revision=0,ack_revision=0,ack_frame=0;
    unsigned ack_success=0,ack_evaluated=0,ack_selected=0;
    unsigned width=0,height=0,guide_width=0,guide_height=0;
    std::uint64_t scratch_bytes=0;
    std::uint64_t feature_generation=0,feature_releases=0,rebuild_serial=0,rebuild_frame=0,bypass_frame=0;
    // Successful RR calls skipped while ON because our input verification
    // missed transiently (nothing recorded; NR history reset on resume).
    // skip_budget 0: skipping never terminates the context; persistence is
    // surfaced through consecutive_skips and the panel instead.
    std::uint64_t skipped_frames=0;unsigned consecutive_skips=0,skip_budget=0;
    // RR frame-index gaps (menu, loading) bridged with an NR history reset on
    // the same Feature instead of a rebuild; the user's ON gate is kept.
    std::uint64_t history_gaps=0;
    // ABI19. discarded_recordings: recorded frames whose command list the game
    // Reset before any submission (proven by the native Reset hook); nothing
    // executed, lease released, NR history reset, ON gate kept. submission_skips:
    // admitted RR frames skipped because the previous recording was still
    // unsubmitted on the game's side; never a fault, ON gate kept.
    std::uint64_t discarded_recordings=0,submission_skips=0;
    // ABI21. Completion signals that had to wait for the timeline lock and
    // then succeeded. Before ABI21 the first such miss terminated NR.
    std::uint64_t signal_retries=0;
    // Host exposure actually applied to the last recorded ON frame (log2 gain,
    // includes the user offset in auto mode) and the GPU meter reading it used.
    float applied_exposure_stops=0,metered_log2_luminance=0;unsigned meter_samples=0,meter_reserved=0;
    char rebuild_reason[64]{};
    nr::Settings requested_settings{},observed_settings{};
    std::uint64_t settings_requested_revision=0,settings_observed_revision=0,settings_frame=0;
    unsigned settings_read_mask=0;
    unsigned settings_style_read=0,settings_reset_requested=0;
    // ABI22. Whether the DLL read SkinStructureStrength / UseAutoMask on the
    // observed Evaluate; optional receipts, never required for ON.
    unsigned settings_skin_read=0,settings_automask_read=0;
    unsigned binding_preservation_enabled=0,binding_preservation_ready=0;
    std::uint64_t binding_restores=0,binding_wait_frames=0;
    // ABI23. The configured frame's regions: 1 when colour/depth are cropped,
    // and the motion region resampled onto the guide grid (0 = not resampled).
    unsigned region_crop=0,motion_width=0,motion_height=0,region_reserved=0;
    RejectedCall rejection;
    char error[256]{};
};
// Which bridge variant the host actually loaded. Queried before Start; the
// controller host demands variant 1, the research host accepts either and
// simply reports research_available=false for a controller bridge.
struct Capabilities {
    unsigned size=sizeof(Capabilities),abi=version;
    unsigned variant=0; // 1 = controller (runtime only), 2 = research
    unsigned supports_binding_preservation=0,supports_settings=0,supports_compute_only=0;
    unsigned research_exports=0; // number of research entry points this module exports
    char build[64]{};
};
static_assert(std::is_trivially_copyable_v<Frame> && std::is_trivially_copyable_v<Status> && std::is_trivially_copyable_v<Capabilities>);
// ABI20 keeps the runtime structures free of every research collector type.
// These sizes are asserted again in live_abi_layout_tests and by
// tests/controller/test_live_abi_header.py, which forbids capture includes here.
static_assert(sizeof(Frame)<=256 && sizeof(Capabilities)<=128);
using Start=bool(__cdecl*)(ID3D12CommandQueue*,HMODULE,const wchar_t*,const wchar_t*,void**);
using GetCapabilities=bool(__cdecl*)(Capabilities*);
using EnableBindingPreservation=bool(__cdecl*)(void*);
// Separately named V1 exports, no change to the ABI12 frame/status layout.
using GameBindingEntry=void(__cdecl*)(void*,std::uint64_t call,std::uint64_t frame,ID3D12GraphicsCommandList*);
using GameBindingExit=void(__cdecl*)(void*,std::uint64_t call);
using Poll=bool(__cdecl*)(void*,Status*);
// Optional, separately versioned read-only diagnostic. ABI12 frame/status layout
// stays unchanged. A sample belongs to an actual observed RR callback, not Present.
struct BindingStatus {
    unsigned size=sizeof(BindingStatus),abi=3,enabled=0,ready=0;
    std::uint64_t frame=0,call=0,consecutive_ready=0,blocked_frames=0,reason_first_frame=0;
    char reason[256]{};
    // Metadata counters only. A hit is not proof of the game's visual root cause.
    std::uint64_t heap_order_permutations=0,heap_permuted_insertions=0;
    // abi 3: ExecuteIndirect calls with a signature of unknown
    // layout that the tolerant late-attach policy let through, and whether that
    // policy is on. See InsertionBindings::tolerate_unknown_indirect.
    std::uint64_t unknown_indirect_calls=0;unsigned unknown_indirect_tolerated=0,reserved=0;
};
static_assert(std::is_trivially_copyable_v<BindingStatus>);
using PollBindings=bool(__cdecl*)(void*,BindingStatus*);
// Optional, separately named export (V1): flags bit 0 = tolerate command
// signatures of unknown layout (late attach). Absent in older bridges.
using BindingPolicy=bool(__cdecl*)(void*,unsigned);
using Request=bool(__cdecl*)(void*,std::uint64_t,unsigned);
using Configure=bool(__cdecl*)(void*,std::uint64_t,const nr::Settings*);
using Apply=bool(__cdecl*)(void*,std::uint64_t,unsigned,const nr::Settings*);
// One safe transaction for staged OFF settings + subsequent ON. The legacy
// Configure entry alone cannot turn NR on and must not be used for this case.
inline bool apply_request(Status& s,bool ready,std::uint64_t revision,unsigned on,const nr::Settings* settings){
    if(!ready||!revision||on>2||revision<=s.request_revision||(settings&&!settings->valid())||(!on&&settings))return false;
    s.request_revision=revision;s.requested_on=on;
    if(settings){s.requested_settings=*settings;s.settings_requested_revision=revision;}return true;
}
using Enter=void(__cdecl*)(void*,const Frame*);
// Acknowledges recording at the matching successful host-call boundary only.
using BoundaryReturned=void(__cdecl*)(void*,std::uint64_t,unsigned);
using Stop=void(__cdecl*)(void*);
using Reject=void(__cdecl*)(void*,const RejectedCall*);
// ABI21. The DIRECT queue the game itself used to submit the command list an
// admitted RR call was recorded on -- observed and re-verified by the submission
// router (device, type and ExecuteCommandLists address all checked), never a
// queue of ours. A late-loaded host has no other way to learn it: it missed
// swapchain creation, and D3D12 exposes no way to ask a swapchain which queue
// presents it. Returns false until an RR submission has actually been observed.
// The caller owns one reference. That this queue is ALSO the presenting one is
// an inference from the game using a single graphics queue, not a guarantee.
using RenderQueue=bool(__cdecl*)(void*,ID3D12CommandQueue**);
// Names of the research-only entry points. They are listed here, in the runtime
// ABI, so a host, the capability report and the boundary test can reason about
// which exports belong to the research variant without including any research
// header. Their signatures live in lab_nr_live_research_api.hpp.
inline constexpr const char* research_entry_points[]={
    "LabNrLiveResearchFrameContext","LabNrLivePollCapture","LabNrLiveCapture",
    "LabNrLiveProbeBindings","LabNrLivePollBindingProbe",
    "LabNrLiveProbePreparation","LabNrLivePollPreparationProbe",
    "LabNrLiveInspectBindingBoundaries","LabNrLiveBindingBoundary","LabNrLivePollBindingBoundaries",
    "LabNrLiveDisplayCapture","LabNrLiveDisplayPresent","LabNrLiveDisplayReturned","LabNrLiveDisplayResize",
    // One chain capture (same-call pair + reconstructor inputs + the following
    // Presents). Its Present copies ride
    // the existing DisplayPresent/DisplayReturned exports; no Frame/Status change.
    "LabNrLiveChainCapture"};
inline constexpr unsigned research_entry_point_count=sizeof(research_entry_points)/sizeof(research_entry_points[0]);
}
