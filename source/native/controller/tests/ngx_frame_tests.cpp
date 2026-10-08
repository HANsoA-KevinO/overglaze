// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// NGX-direct RR -> NR frame translation, on WARP. Every refusal path, plus the
// accepted frame built from Halo: Campaign Evolved's measured facts
// (create flags 0x43, typed R32_FLOAT depth / R16G16_FLOAT motion, UAV-capable
// RGBA16F output, MV scale 1). No NGX, no game, no NR model.
#include "lab_ngx_frame.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
int checks = 0;
void need(bool ok, const std::string& why) {
    ++checks;
    if (!ok) throw std::runtime_error(why);
}
void hr(HRESULT r, const char* what) {
    if (FAILED(r)) throw std::runtime_error(std::string(what) + " failed");
}

struct Warp {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Warp() {
        ComPtr<IDXGIFactory4> factory;
        hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter> adapter;
        hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
        hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    }
    ComPtr<ID3D12Resource> texture(unsigned w, unsigned h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON,
                                   unsigned array = 1, unsigned mips = 1) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = static_cast<UINT16>(array); d.MipLevels = static_cast<UINT16>(mips);
        d.Format = format; d.SampleDesc.Count = 1; d.Flags = flags;
        ComPtr<ID3D12Resource> r;
        hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "texture");
        return r;
    }
};

// A refusal must happen AND name its cause, so the panel says something true.
void refused(const lab::ngx::Evaluation& e, const char* word, const char* label) {
    lab::live::Frame f;
    const char* why = lab::ngx::translate(e, f);
    need(why != nullptr, std::string(label) + ": expected a refusal");
    need(std::string(why).find(word) != std::string::npos,
         std::string(label) + ": refusal '" + why + "' does not name '" + word + "'");
}
}

int main() try {
    // ---- candidacy: decided before anything of ours sees the call. Only SR
    // and RR are candidates; frame generation is forwarded untouched
    // (Hellblade 2: offered as skips, it ended NR).
    {
        using lab::ngx::Candidacy;
        using lab::ngx::candidacy;
        need(candidacy(true, lab::ngx::super_sampling, true) == Candidacy::candidate, "SR is a candidate");
        need(candidacy(true, lab::ngx::ray_reconstruction, true) == Candidacy::candidate, "RR is a candidate");
        need(candidacy(true, lab::ngx::super_sampling, false) == Candidacy::candidate,
             "an observed SR without Output stays a candidate: translate() names what is missing");
        need(candidacy(true, lab::ngx::frame_generation, false) == Candidacy::other_feature, "frame generation (11) is not");
        need(candidacy(true, lab::ngx::frame_generation, true) == Candidacy::other_feature,
             "frame generation is not a candidate even if its block carried an Output");
        need(candidacy(true, 12, true) == Candidacy::other_feature, "DeepDVC (12) is not");
        need(candidacy(true, 0, true) == Candidacy::other_feature && candidacy(true, 32766, true) == Candidacy::other_feature,
             "reserved and unknown ids are not");
        need(candidacy(false, 0, false) == Candidacy::unobserved_without_output,
             "create unobserved and no Output: can never become a frame, not a candidate");
        need(candidacy(false, 0, true) == Candidacy::candidate,
             "create unobserved with an Output: a candidate, so its refusal can say 'change a quality setting'");
    }
    Warp gpu;
    // Halo's ratio at a size WARP allocates quickly: output 2x the guides.
    const unsigned ow = 512, oh = 216, gw = 256, gh = 108;
    auto output = gpu.texture(ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto depth = gpu.texture(gw, gh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto motion = gpu.texture(gw, gh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    lab::ngx::Evaluation base;
    base.call = base.frame = 7;
    base.command = gpu.list.Get();
    base.create_observed = true;
    base.feature_id = lab::ngx::ray_reconstruction;
    base.create_flags = 0x43;  // measured on Halo: IsHDR | MVLowRes | AutoExposure
    base.output = output.Get(); base.depth = depth.Get(); base.motion = motion.Get();
    base.mv_scale_present = true; base.mv_scale_x = 1.f; base.mv_scale_y = 1.f;
    base.render_subrect_present = true; base.render_width = gw; base.render_height = gh;

    // ---- the accepted frame
    {
        lab::live::Frame f;
        const char* why = lab::ngx::translate(base, f);
        need(why == nullptr, std::string("Halo's measured facts must translate; got: ") + (why ? why : ""));
        need(f.size == sizeof(lab::live::Frame) && f.abi == lab::live::version, "frame ABI header");
        need(f.call == 7 && f.frame == 7 && f.command == gpu.list.Get(), "call, frame and command carried");
        need(f.color == output.Get() && f.depth == depth.Get() && f.motion == motion.Get(), "roles: NR colour is the RR output");
        need(f.width == ow && f.height == oh && f.guide_width == gw && f.guide_height == gh, "extents from the resources");
        need(f.region_crop == 0 && f.motion_width == 0 && f.motion_height == 0, "Halo's regions fill their resources: no crop, motion on the guide grid");
        need(f.color_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "output is declared UAV");
        need(f.depth_state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
             f.motion_state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
             "guides are declared in NR's read state, so no barrier is issued on them");
        need(f.motion_x == 1.f && f.motion_y == 1.f, "NGX MV.Scale passes through as NR's scale");
        need(f.depth_inverted == 0 && f.reset == 0, "Halo: depth not inverted, no reset");
        need(f.packed_guides == 1 && f.depth_encoding == lab::nr::DepthEncoding::hardware, "typed guides, hardware depth");
    }
    // ---- DLSS super resolution, when the game has no RR
    {
        auto e = base; e.feature_id = lab::ngx::super_sampling;
        lab::live::Frame f; const char* why = lab::ngx::translate(e, f);
        need(why == nullptr, std::string("SR with no RR alive must translate; got: ") + (why ? why : ""));
        need(f.color == output.Get() && f.width == ow && f.guide_width == gw, "SR: NR goes on the upscaled output");
        e = base; e.rr_alive_elsewhere = true;
        need(!lab::ngx::translate(e, f), "RR itself is never blocked by the RR-preference rule");
        e = base; e.feature_id = lab::ngx::super_sampling; e.create_flags &= ~lab::ngx::flag_is_hdr;
        refused(e, "not declared HDR", "SR without IsHDR");
    }
    // ---- facts that travel into the frame
    {
        auto e = base; e.create_flags |= lab::ngx::flag_depth_inverted;
        lab::live::Frame f; need(!lab::ngx::translate(e, f) && f.depth_inverted == 1, "DepthInverted flag reaches the frame");
        e = base; e.reset = 1;
        need(!lab::ngx::translate(e, f) && f.reset == 1, "the game's Reset reaches the frame");
        e = base; e.mv_scale_x = -2.5f; e.mv_scale_y = 0.75f;
        need(!lab::ngx::translate(e, f) && f.motion_x == -2.5f && f.motion_y == 0.75f, "MV scale signs are untouched");
        e = base; e.render_subrect_present = false;
        need(!lab::ngx::translate(e, f), "an absent render subrect is not a refusal");
    }
    // ---- guide layout follows the depth format; the runtime's per-frame rules
    // are refused here by name instead of stopping NR there
    {
        auto typeless = gpu.texture(gw, gh, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        auto e = base; e.depth = typeless.Get(); lab::live::Frame f;
        const char* why = lab::ngx::translate(e, f);
        need(!why && f.packed_guides == 0 && f.depth_encoding == lab::nr::DepthEncoding::hardware,
             std::string("R32 typeless depth goes the hardware-depth path; got: ") + (why ? why : ""));
        auto two_plane = gpu.texture(gw, gh, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        e = base; e.depth = two_plane.Get(); why = lab::ngx::translate(e, f);
        need(!why && f.packed_guides == 0, std::string("D32S8 typeless depth is admitted for conversion; got: ") + (why ? why : ""));
        auto unorm = gpu.texture(gw, gh, DXGI_FORMAT_R16_UNORM, D3D12_RESOURCE_FLAG_NONE);
        e = base; e.depth = unorm.Get(); refused(e, "Depth format", "unsupported depth format");
        auto rgba8 = gpu.texture(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        e = base; e.output = rgba8.Get(); refused(e, "Colour format", "LDR output format");
        auto rg32 = gpu.texture(gw, gh, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        e = base; e.motion = rg32.Get(); refused(e, "Motion format", "RG32F motion");
        auto layered = gpu.texture(ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON, 2);
        e = base; e.output = layered.Get(); refused(e, "array", "texture-array output");
        auto motion_mips = gpu.texture(gw, gh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_COMMON, 1, 3);
        e = base; e.motion = motion_mips.Get(); refused(e, "mip chain", "motion with a mip chain");
        auto big = gpu.texture(ow * 2, oh * 2, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        auto big_motion = gpu.texture(ow * 2, oh * 2, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        e = base; e.depth = big.Get(); e.motion = big_motion.Get(); e.render_subrect_present = false;
        refused(e, "Guide extent", "guides larger than the output");
        e = base; e.motion = e.depth; refused(e, "same resource", "one resource in two roles");
    }
    // ---- every refusal names its cause
    {
        auto e = base; e.create_observed = false;
        refused(e, "change any DLSS quality setting", "late attach: create not observed");
        e = base; e.feature_id = 11; refused(e, "super-resolution or ray-reconstruction", "frame-generation feature");
        e = base; e.feature_id = lab::ngx::super_sampling; e.rr_alive_elsewhere = true;
        refused(e, "SR is used only when the game has no RR", "SR while an RR feature is alive");
        e = base; e.create_flags &= ~lab::ngx::flag_is_hdr; refused(e, "HDR", "colour not HDR");
        // MVLowRes is not required: the extents decide.
        {e = base; e.create_flags &= ~lab::ngx::flag_mv_low_res; lab::live::Frame f;
         need(!lab::ngx::translate(e, f), "MVLowRes absent is not a refusal; depth and motion extents decide");}
        e = base; e.create_flags |= lab::ngx::flag_mv_jittered; refused(e, "Jittered", "jittered MVs");
        e = base; e.command = nullptr; refused(e, "missing", "no command list");
        e = base; e.output = nullptr; refused(e, "missing", "no output");
        e = base; e.depth = nullptr; refused(e, "missing", "no depth");
        e = base; e.motion = nullptr; refused(e, "missing", "no motion");
        e = base; e.mv_scale_present = false; refused(e, "scale", "MV scale absent");
        e = base; e.mv_scale_x = std::nanf(""); refused(e, "scale", "MV scale not finite");
        e = base; e.mv_scale_y = INFINITY; refused(e, "scale", "MV scale infinite");
        auto small_motion = gpu.texture(gw / 2, gh / 2, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        e = base; e.motion = small_motion.Get(); refused(e, "MVLowRes", "render-resolution motion smaller than the render region");
        // Wuthering Waves' measured shape, scaled down: SR created IsHDR | MVLowRes |
        // DepthInverted | AutoExposure, render subrect 1705x720, depth padded to
        // 1708x720, motion exactly the render subrect. The motion covers the guide
        // region, so both are cropped to it.
        {auto ww_depth = gpu.texture(gw + 3, gh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
            e = base; e.feature_id = lab::ngx::super_sampling; e.create_flags = 75; e.depth = ww_depth.Get();
            lab::live::Frame f; const char* why = lab::ngx::translate(e, f);
            need(!why, std::string("Wuthering Waves' shape must translate; got: ") + (why ? why : ""));
            need(f.guide_width == gw && f.guide_height == gh && f.region_crop == 1 && f.motion_width == 0 && f.motion_height == 0,
                 "padded depth: guide grid is the render subrect, cropped; motion on that grid");}
        e = base; e.subrect_base_offset = true; refused(e, "not at the origin", "a subrect base away from the origin");
        e = base; e.output_width = ow + 2; e.output_height = oh; refused(e, "larger than its output resource", "output extent beyond its resource");
        e = base; e.render_width = gw + 2; refused(e, "larger than the depth resource", "render subrect beyond the depth resource");
        auto no_uav = gpu.texture(ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        e = base; e.output = no_uav.Get(); refused(e, "UAV", "output not UAV-capable");
        auto unreadable = gpu.texture(gw, gh, DXGI_FORMAT_D32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        e = base; e.depth = unreadable.Get(); refused(e, "shader-readable", "guide denies shader resource");
        ComPtr<ID3D12CommandAllocator> compute_allocator;
        hr(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&compute_allocator)), "compute allocator");
        ComPtr<ID3D12GraphicsCommandList> compute;
        hr(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, compute_allocator.Get(), nullptr, IID_PPV_ARGS(&compute)), "compute list");
        e = base; e.command = compute.Get(); refused(e, "non-direct", "RR on an async compute list");
    }
    // ---- regions (Live ABI 23): Hellblade 2's measured shape. SR,
    // created IsHDR | DepthInverted | AutoExposure (no MVLowRes): the output image
    // is 5120x2142 inside a 5120x2144 texture, the render subrect 2970x1243 inside
    // 2972x1256 depth, and motion at display resolution, 5120x2142. Scaled down.
    {
        const unsigned tw = 512, th = 218, iw = 512, ih = 216, dw = 260, dh = 110, rw = 256, rh = 107;
        auto hb_output = gpu.texture(tw, th, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto hb_depth = gpu.texture(dw, dh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        auto hb_motion = gpu.texture(iw, ih, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        auto e = base;
        e.feature_id = lab::ngx::super_sampling; e.create_flags = 73;
        e.output = hb_output.Get(); e.depth = hb_depth.Get(); e.motion = hb_motion.Get();
        e.output_width = iw; e.output_height = ih; e.render_width = rw; e.render_height = rh;
        lab::live::Frame f; const char* why = lab::ngx::translate(e, f);
        need(!why, std::string("Hellblade 2's shape must translate; got: ") + (why ? why : ""));
        need(f.width == iw && f.height == ih, "the colour region is the upscaler's output extent, not its texture");
        need(f.guide_width == rw && f.guide_height == rh, "the guide grid is the render subrect, not the depth texture");
        need(f.region_crop == 1, "regions smaller than their resources are cropped");
        need(f.motion_width == iw && f.motion_height == ih, "display-resolution motion covers the output region");
        need(f.motion_x == 1.f && f.motion_y == 1.f, "the scale stays that of the motion resource; the runtime rescales it");
        need(f.depth_inverted == 1, "DepthInverted from the create flags");
        // The same motion when the game declared it render-resolution is a contradiction.
        auto low = e; low.create_flags |= lab::ngx::flag_mv_low_res;
        refused(low, "MVLowRes", "display-extent motion declared MVLowRes");
        // Display-resolution motion that does not cover the output region.
        auto short_motion = gpu.texture(iw, ih - 4, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        auto s = e; s.motion = short_motion.Get();
        refused(s, "smaller than the output region", "display-resolution motion short of the output region");
        // Motion on the depth grid, cropped together with the depth.
        auto grid_motion = gpu.texture(dw, dh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        auto g2 = e; g2.motion = grid_motion.Get(); g2.create_flags |= lab::ngx::flag_mv_low_res;
        why = lab::ngx::translate(g2, f);
        need(!why && f.region_crop == 1 && f.motion_width == 0 && f.guide_width == rw,
             std::string("render-resolution motion is cropped with the depth; got: ") + (why ? why : ""));
        // Output extent unknown: the whole texture is the region, and motion that
        // covers less than it cannot be placed -- refused, not stretched.
        auto nc = e; nc.output_width = nc.output_height = 0;
        refused(nc, "smaller than the output region", "unknown output extent with display motion short of the texture");
    }
    std::cout << "PASS " << checks << " NGX frame translation checks on WARP; no NGX, no game, no NR\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL after " << checks << " checks: " << e.what() << "\n";
    return 1;
}
