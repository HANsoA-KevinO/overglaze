// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The live runtime's per-frame resource rules, as a pure pre-check a frame
// source runs BEFORE it hands a frame over.
//
// The runtime (nr_live_core.hpp validate) refuses a frame that breaks one of
// these by stopping NR for good. That is the right last line: a frame that got
// that far with a wrong resource is our bug. But a frame source that can SEE
// the problem first -- a texture array, a multisampled guide, a format the
// runtime has no path for -- should skip that one frame by name and keep the
// user's ON gate, not end NR for the session. Both routes (Streamline, NGX
// direct) run this before delivery.
//
// Keep it in step with validate(): a rule added there belongs here too. Only
// what a frame source knows is checked: the runtime's device identity and its
// own mip-copy paths stay the runtime's.
#include "lab_nr_live_api.hpp"
#include "lab_depth_projection.hpp"
#include <d3d12.h>
#include <cmath>

namespace lab::live {
inline const char* prescreen(const Frame& f) noexcept {
    if(!f.command||!f.color||!f.depth||!f.motion)return "Frame is missing the command list, colour, depth or motion";
    if(f.color==f.depth||f.color==f.motion||f.depth==f.motion)return "Two roles are the same resource";
    if(!nr::guide_extent_supported(f.width,f.height,f.guide_width,f.guide_height))return "Guide extent larger than the output or outside 1..8192";
    if(f.depth_encoding==nr::DepthEncoding::view_z_magnitude&&!f.packed_guides)return "Linear depth needs a typed depth guide";
    if(f.region_crop>1||(f.motion_width==0)!=(f.motion_height==0)||f.motion_width>8192||f.motion_height>8192)return "Frame regions are malformed";
    // ABI23: motion is measured against its own region (display-resolution
    // motion is another grid than depth); with region_crop a resource may be
    // larger than its region, whose top-left part is the image.
    const unsigned motion_w=f.motion_width?f.motion_width:f.guide_width,motion_h=f.motion_height?f.motion_height:f.guide_height;
    const ID3D12Resource* textures[]{f.color,f.depth,f.motion};
    static constexpr const char* array_msg[]{"Output colour is a texture array or not 2D","Depth is a texture array or not 2D","Motion is a texture array or not 2D"};
    static constexpr const char* msaa_msg[]{"Output colour is multisampled","Depth is multisampled","Motion is multisampled"};
    static constexpr const char* read_msg[]{"Output colour denies shader reads","Depth denies shader reads","Motion denies shader reads"};
    static constexpr const char* size_msg[]{"Output colour extent differs from the frame region","Depth extent differs from the frame region","Motion extent differs from its region"};
    for(unsigned i=0;i<3;++i){
        const auto d=const_cast<ID3D12Resource*>(textures[i])->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.DepthOrArraySize!=1)return array_msg[i];
        if(d.SampleDesc.Count!=1||d.SampleDesc.Quality!=0)return msaa_msg[i];
        if(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)return read_msg[i];
        // Colour and depth are met through level 0 (colour handoff copy_top,
        // depth codec mip-0 copy); motion is handed to NR directly.
        if(i==2&&d.MipLevels!=1)return "Motion carries a mip chain";
        const unsigned ew=i==0?f.width:i==1?f.guide_width:motion_w,eh=i==0?f.height:i==1?f.guide_height:motion_h;
        if(f.region_crop?(d.Width<ew||d.Height<eh):(d.Width!=ew||d.Height!=eh))return size_msg[i];
        if(i==0&&d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&d.Format!=DXGI_FORMAT_R11G11B10_FLOAT)
            return "Colour format is not RGBA16F or R11G11B10F";
        if(i==1){
            const bool typed=d.Format==DXGI_FORMAT_R32_FLOAT||d.Format==DXGI_FORMAT_R16_FLOAT;
            const bool typeless=d.Format==DXGI_FORMAT_R32_TYPELESS||
                (d.Format==DXGI_FORMAT_R32G8X24_TYPELESS&&f.depth_encoding==nr::DepthEncoding::hardware);
            if(f.packed_guides?!typed:!typeless)return f.packed_guides
                ?"Depth format is not R32F or R16F (typed guides)"
                :"Depth format is not R32 typeless or D32S8 typeless (hardware depth)";
        }
        if(i==2&&d.Format!=DXGI_FORMAT_R16G16_FLOAT&&d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)
            return "Motion format is not RG16F or RGBA16F";
        if(i==0&&!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return "Output colour is not UAV-capable";
    }
    constexpr unsigned read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto guide_ok=[](unsigned state,bool depth){
        const unsigned allowed=read|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|(depth?D3D12_RESOURCE_STATE_DEPTH_READ:0u);
        return state==D3D12_RESOURCE_STATE_COMMON||state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS||((state&read)&&!(state&~allowed));};
    if(!guide_ok(f.depth_state,true))return "Depth guide declared in a state NR cannot read (e.g. DEPTH_WRITE)";
    if(!guide_ok(f.motion_state,false))return "Motion guide declared in a state NR cannot read";
    if(f.color_state!=D3D12_RESOURCE_STATE_UNORDERED_ACCESS&&f.color_state!=D3D12_RESOURCE_STATE_COMMON)
        return "Output colour not declared UAV or COMMON";
    if(!std::isfinite(f.motion_x)||!std::isfinite(f.motion_y))return "Motion scale not finite";
    return nullptr;
}
}
