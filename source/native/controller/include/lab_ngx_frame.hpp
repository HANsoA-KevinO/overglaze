// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// NGX-direct ray reconstruction -> NR frame. The counterpart of
// translate_rr_frame on the Streamline route, and deliberately the same shape:
// pure, schedules nothing, takes no lease, touches no global state, and sees no
// NGX type, so a WARP test can drive every refusal path.
//
// Every fact below was MEASURED on Halo: Campaign Evolved (an observation run
// of about ten thousand evaluates) unless marked as an
// inference; see the resource-state note, which is the one place this layer
// states something the observation could not see.
#include "lab_frame_receiver.hpp"
#include "lab_live_frame_prescreen.hpp"
#include "lab_game_exposure.hpp"
#include <d3d12.h>
#include <cmath>
#include <cstdint>

namespace lab::ngx {

constexpr std::uint32_t super_sampling = 1;       // NVSDK_NGX_Feature_SuperSampling (DLSS SR)
constexpr std::uint32_t frame_generation = 11;    // NVSDK_NGX_Feature_FrameGeneration (DLSS-G): never a candidate
constexpr std::uint32_t ray_reconstruction = 13;  // NVSDK_NGX_Feature_RayReconstruction
// NVSDK_NGX_DLSS_Feature_Flags, read from the game's CreateFeature parameters.
constexpr int flag_is_hdr = 1 << 0, flag_mv_low_res = 1 << 1, flag_mv_jittered = 1 << 2,
              flag_depth_inverted = 1 << 3, flag_auto_exposure = 1 << 6;

// One ray-reconstruction EvaluateFeature call, in plain values. The observer
// fills it from the game's own parameter block BEFORE forwarding the call.
struct Evaluation {
    std::uint64_t call = 0, frame = 0;
    ID3D12GraphicsCommandList* command = nullptr;
    // Create-time facts. Only meaningful when create_observed: a late attach
    // arrives after the game created its feature, and the depth direction is
    // ONLY in the create flags -- so an unobserved create is refused, never guessed.
    bool create_observed = false;
    std::uint32_t feature_id = 0;
    int create_flags = 0;
    ID3D12Resource* output = nullptr;  // RR output: NR's colour input, written back in place
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    bool mv_scale_present = false;
    float mv_scale_x = 0, mv_scale_y = 0;
    int reset = 0;
    bool render_subrect_present = false;
    unsigned render_width = 0, render_height = 0;
    // Create-time output extent (OutWidth/OutHeight): the image the upscaler
    // writes, which may be smaller than its output resource -- Hellblade 2's
    // 2.39:1 frame is 5120x2142 inside a 5120x2144 texture. 0: unknown.
    unsigned output_width = 0, output_height = 0;
    // Some DLSS input or output subrect base was set and is not the origin.
    bool subrect_base_offset = false;
    // True when ANOTHER live feature of this game is ray reconstruction. Policy:
    // RR when the game has it, SR only when it
    // does not -- so an SR call is skipped while an RR feature exists.
    bool rr_alive_elsewhere = false;
    // The game's exposure (Live ABI24, auto exposure only): the ExposureTexture
    // it handed NGX, null when none, and DLSS.Pre.Exposure / DLSS.Exposure.Scale,
    // 1 when absent. A feature created with AutoExposure meters itself, so its
    // texture (normally absent) is not used.
    ID3D12Resource* exposure = nullptr;
    float pre_exposure = 1, exposure_scale = 1;
};

// Whether one NGX Evaluate is a CANDIDATE for NR at all, decided BEFORE anything
// of ours sees it. The evaluate hook sits on the loader's single D3D12 entry, so
// it receives every feature the game drives through NGX -- not only SR and RR
// but DLSS frame generation (FrameGeneration=11, one Evaluate per generated
// frame, from its own thread under MFG) and anything else the loader serves.
//
// A non-candidate is forwarded untouched: it is never offered to the runtime,
// takes no call/frame number, enters no binding scope and counts as no skip.
// Offering them as named skips is what stopped NR on Hellblade 2 with frame
// generation on (measured in its NR fault records): each one
// reset NR history, each one advanced the call/frame counter the runtime reads
// continuity from, and one arriving from another thread while an SR frame sat
// between its recording and its return fell through the runtime's skip rule
// and ended NR outright.
//
//  * create observed: the feature id decides, SR or RR only;
//  * create NOT observed (late attach, or no free record): the id is unknown.
//    SR and RR both take an Output; frame generation's parameters are DLSSG.*
//    keys only (nvsdk_ngx_defs_dlssg.h) and carry none. Without an Output
//    translate() can never produce a frame, so the call is not a candidate. With
//    one it stays a candidate, so its refusal can still say "change a quality
//    setting once" -- the one thing that turns it into a usable frame.
enum class Candidacy : unsigned { candidate, other_feature, unobserved_without_output };
inline Candidacy candidacy(bool create_observed, std::uint32_t feature_id, bool output_present) noexcept {
    if (create_observed)
        return feature_id == super_sampling || feature_id == ray_reconstruction ? Candidacy::candidate
                                                                                : Candidacy::other_feature;
    return output_present ? Candidacy::candidate : Candidacy::unobserved_without_output;
}

// Returns nullptr on success, otherwise why this call cannot become a frame.
// The feature-id refusal below stays as the backstop for any caller that offers
// a non-candidate anyway; the NGX observer filters with candidacy() first.
inline const char* translate(const Evaluation& e, live::Frame& f) noexcept {
    if (!e.create_observed)
        return "RR feature was created before we attached; change any DLSS quality setting once so the create is observed";
    // Both features take the same four roles and the same create flags; NR
    // goes on the upscaled OUTPUT either way. SR's output is not denoised the
    // way RR's is, which changes what NR has to work on, not where it goes.
    if (e.feature_id != ray_reconstruction && e.feature_id != super_sampling)
        return "Not a DLSS super-resolution or ray-reconstruction feature";
    if (e.feature_id == super_sampling && e.rr_alive_elsewhere)
        return "An RR feature is alive; SR is used only when the game has no RR";
    // Our colour preparation is written for a linear HDR working image. An SR
    // feature created without IsHDR hands us display-referred colour; that is
    // refused rather than run through a pipeline built for something else.
    if (!(e.create_flags & flag_is_hdr)) return "Upscaler colour is not declared HDR";
    // No MVLowRes requirement. The flag says whether
    // motion is at render or display resolution; what NR needs is that depth
    // and motion share one extent no larger than the output, and that is
    // checked on the resources themselves below and in the prescreen.
    if (e.create_flags & flag_mv_jittered) return "Jittered motion vectors are unsupported";
    if (!e.command || !e.output || !e.depth || !e.motion) return "RR call is missing the command list, output, depth or motion";
    // NGX accepts a compute list; our colour handoff and the barriers around it
    // are direct-list work. A UE title may record RR on async compute, so this
    // is a named skip here rather than an exception deep in the runtime.
    if (e.command->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return "RR is recorded on a non-direct (e.g. async compute) command list; NR insertion needs a direct list";
    if (!e.mv_scale_present || !std::isfinite(e.mv_scale_x) || !std::isfinite(e.mv_scale_y))
        return "RR motion-vector scale missing or not finite";
    if (e.subrect_base_offset) return "A DLSS subrect is not at the origin";
    const auto out = e.output->GetDesc(), d = e.depth->GetDesc(), m = e.motion->GetDesc();
    if (out.Width > 8192 || out.Height > 8192 || !out.Width || !out.Height) return "RR output extent unsupported";
    // REGIONS (Live ABI 23, Hellblade 2's shape). The image is the top-left part
    // of each resource: the output the upscaler was created for, the render
    // subrect in the depth. Both were measured smaller than their resources.
    unsigned ow = static_cast<unsigned>(out.Width), oh = out.Height;
    if (e.output_width && e.output_height) {
        if (e.output_width > out.Width || e.output_height > out.Height) return "Upscaler output extent is larger than its output resource";
        ow = e.output_width; oh = e.output_height;
    }
    unsigned gw = static_cast<unsigned>(d.Width), gh = d.Height;
    if (e.render_subrect_present) {
        if (e.render_width > d.Width || e.render_height > d.Height) return "Render subrect is larger than the depth resource";
        gw = e.render_width; gh = e.render_height;
    }
    // Motion on the depth grid, or -- created without MVLowRes -- at display
    // resolution covering the output region. Hellblade 2 measured 5120x2142
    // motion beside 2972x1256 depth; the runtime resamples it onto the guide
    // grid. Anything else is refused by name rather than guessed at.
    unsigned mw = 0, mh = 0;
    // Render-resolution motion is on the depth grid even when the two
    // resources are padded differently: Wuthering Waves measured 1705x720
    // motion beside 1708x720 depth, render subrect 1705x720. What NR needs is
    // that the motion covers the guide region and is no larger than the depth
    // resource (larger would be display-resolution motion mislabelled as
    // render-resolution); both are cropped to the guide region.
    const bool covers_guides = (e.create_flags & flag_mv_low_res) && m.Width >= gw && m.Height >= gh &&
                               m.Width <= d.Width && m.Height <= d.Height;
    if ((m.Width == d.Width && m.Height == d.Height) || covers_guides) {
        // same grid as depth, cropped with it
    } else if (!(e.create_flags & flag_mv_low_res) && m.Width >= ow && m.Height >= oh) {
        mw = ow; mh = oh;
    } else {
        return (e.create_flags & flag_mv_low_res) ? "Motion declared render-resolution (MVLowRes) but smaller than the render region"
                                                  : "Display-resolution motion is smaller than the output region";
    }
    const bool crop = ow != out.Width || oh != out.Height || gw != d.Width || gh != d.Height ||
                      (mw ? (m.Width != mw || m.Height != mh) : (m.Width != gw || m.Height != gh));
    // NGX wrote the output through a UAV and read the guides as shader
    // resources on this very command list; a resource that could do neither
    // cannot be what NGX just consumed.
    if (!(out.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return "RR output is not UAV-capable";
    if ((d.Flags | m.Flags) & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) return "RR guides are not shader-readable";

    f = {};
    f.call = e.call;
    f.frame = e.frame;
    f.command = e.command;
    f.color = e.output;
    f.depth = e.depth;
    f.motion = e.motion;
    f.width = ow;
    f.height = oh;
    f.guide_width = gw;
    f.guide_height = gh;
    f.region_crop = crop ? 1u : 0u;
    f.motion_width = mw;
    f.motion_height = mh;
    // RESOURCE STATES -- the one inference in this layer. NGX's Evaluate takes
    // no states and the controller track observes no game barrier, so they
    // follow from D3D12 rules rather than from a tag:
    //  * output: NGX's compute pass writes it through a UAV, which for a
    //    non-simultaneous texture is legal only in UNORDERED_ACCESS (a texture
    //    cannot be promoted from COMMON to UAV), and NGX leaves application
    //    resource states as it found them. The colour handoff transitions it
    //    UAV -> COPY_SOURCE/COPY_DEST -> UAV around our copies.
    //  * depth/motion: declared NON_PIXEL_SHADER_RESOURCE, which is exactly
    //    the state NR reads them in, so guide_transitions issues NO barrier on
    //    the game's guides at all -- correct for any shader-readable state NGX
    //    itself just read them in.
    // What this does not cover: a game driving these resources with enhanced
    // barriers. That case is unverified, not assumed away.
    f.color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    f.depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    f.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // The Streamline route computes mvecScale x guide extent; Streamline's DLSS
    // plugin hands NGX exactly that product as MV.Scale, so NGX's value is
    // already NR's unit for motion on the guide grid. For display-resolution
    // motion it is the unit of the motion resource as given; the runtime takes
    // it onto the guide grid when it resamples. Signs pass through untouched.
    f.motion_x = e.mv_scale_x;
    f.motion_y = e.mv_scale_y;
    f.depth_inverted = (e.create_flags & flag_depth_inverted) ? 1u : 0u;
    f.reset = e.reset ? 1u : 0u;
    // Guide layout follows the depth resource's own format. Halo measured
    // typed R32_FLOAT; a game whose depth is the
    // typeless R32 or D32S8 spelling goes the runtime's hardware-depth path
    // (two-plane depth is CONVERTED there), instead of being declared typed
    // and stopping NR at the runtime's format check.
    const auto depth_format = d.Format;
    f.packed_guides = (depth_format == DXGI_FORMAT_R32_TYPELESS || depth_format == DXGI_FORMAT_R32G8X24_TYPELESS) ? 0u : 1u;
    f.depth_encoding = nr::DepthEncoding::hardware;
    // The game's exposure, never a reason to skip. Its state is inferred like
    // the guides': NGX read it as a shader resource on this list, so it is
    // declared NON_PIXEL_SHADER_RESOURCE and read there without a barrier.
    f.pre_exposure = e.pre_exposure;
    f.exposure_scale = e.exposure_scale;
    if (e.create_flags & flag_auto_exposure) f.exposure_note = static_cast<unsigned>(live::ExposureNote::dlss_auto_exposure);
    else if (!e.exposure) f.exposure_note = static_cast<unsigned>(live::ExposureNote::no_texture);
    else { f.exposure = e.exposure; f.exposure_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; f.exposure_note = 0; }
    live::screen_exposure(f);
    // Every per-frame rule the runtime would otherwise end NR on, as a skip.
    return live::prescreen(f);
}
}
