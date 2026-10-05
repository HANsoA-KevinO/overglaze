// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_nr_pipeline.hpp"
#include "lab_sl_bindings.hpp"
#include "lab_sl_guide_contract.hpp"

namespace lab::nr {
// These are adapter/host obligations, NOT UI checkboxes or values decoded from
// SL. In particular a texture format cannot establish linear working RGB.
struct SlHostContract {
    bool resource_lifetime_state_and_exclusive_access=false;
    bool command_list_bindings_restored_by_caller=false;
    bool finite_nonnegative_linear_working_rgb=false;
};
inline Frame sl_frame(const slboundary::Binding& b,std::uint64_t sequence,const SlHostContract& proof) {
    if(!proof.resource_lifetime_state_and_exclusive_access || !proof.command_list_bindings_restored_by_caller ||
       !proof.finite_nonnegative_linear_working_rgb || !b.call || !b.command || b.viewport!=0 || b.resources[1].type!=sl::kBufferTypeDepth)
        throw std::logic_error("SL adapter host contract not established");
    Frame f;f.token=sequence;f.viewport=b.viewport;f.commands=static_cast<ID3D12GraphicsCommandList*>(b.command);
    f.color=static_cast<ID3D12Resource*>(b.resources[0].native);f.depth=static_cast<ID3D12Resource*>(b.resources[1].native);f.motion=static_cast<ID3D12Resource*>(b.resources[2].native);
    std::array<D3D12_RESOURCE_DESC,3> desc{};
    for(unsigned i=0;i<3;++i){const auto& tag=b.resources[i];
        if(tag.issues || tag.null_resource || !tag.native || tag.resource_type!=sl::ResourceType::eTex2d)throw std::logic_error("SL tag resource invalid");
        desc[i]=static_cast<ID3D12Resource*>(tag.native)->GetDesc();const auto& d=desc[i];auto extent=tag.extent;
        if(d.Width>8192 || !d.Width || d.Height>8192 || !d.Height)throw std::logic_error("SL texture bound");
        if(!extent.width && !extent.height && !extent.left && !extent.top)extent={0,0,static_cast<unsigned>(d.Width),d.Height};
        if(extent.left || extent.top || extent.width!=d.Width || extent.height!=d.Height)throw std::logic_error("SL partial/nonzero-origin extents not supported");
    }
    if(desc[1].Width!=desc[2].Width || desc[1].Height!=desc[2].Height)throw std::logic_error("Guide extents differ");
    f.width=static_cast<unsigned>(desc[0].Width);f.height=desc[0].Height;f.guide_width=static_cast<unsigned>(desc[1].Width);f.guide_height=desc[1].Height;
    if(desc[1].Format==DXGI_FORMAT_R32_TYPELESS && desc[2].Format==DXGI_FORMAT_R16G16B16A16_FLOAT)f.guide_layout=GuideLayout::r32_typeless_rgba16_float;
    else if(desc[1].Format!=DXGI_FORMAT_R32_FLOAT || desc[2].Format!=DXGI_FORMAT_R16G16_FLOAT)throw std::logic_error("Unverified SL guide layout");
    const auto c=sl_guide_constants(b.constants,f.guide_width,f.guide_height);
    f.motion_scale_x=c.scale_x;f.motion_scale_y=c.scale_y;f.depth_inverted=c.depth_inverted;f.history_reset_requested=c.reset;
    f.states_established=true;f.distinct_allocations_established=true;f.color_domain=ColorDomain::linear_working_rgb;return f;
}
inline Selection record_sl_frame(PrePostFramePipeline& pipeline,Mode mode,const slboundary::Resolution& resolution,
                                std::uint64_t sequence,const SlHostContract& proof,const PrePostColorConstants& constants) {
    if(mode==Mode::off){Frame f;f.token=sequence;f.color=static_cast<ID3D12Resource*>(resolution.binding.resources[0].native);
        return pipeline.record(mode,f,D3D12_RESOURCE_STATE_COMMON,false,{});}
    if(mode!=Mode::on || !resolution.ready())throw std::logic_error("No exact successful SL binding");
    const auto& b=resolution.binding;const Frame f=sl_frame(b,sequence,proof);
    const auto target_state=static_cast<D3D12_RESOURCE_STATES>(b.resources[0].state);
    std::array<D3D12_RESOURCE_BARRIER,2> barriers{};unsigned count=0;
    for(unsigned i=1;i<3;++i){const auto before=static_cast<D3D12_RESOURCE_STATES>(b.resources[i].state);
        const auto allowed=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|
            (i==1?D3D12_RESOURCE_STATE_DEPTH_READ:D3D12_RESOURCE_STATE_COMMON);
        if(!before || (before&~allowed) || !(before&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))throw std::logic_error("Guide read-state contract unsupported");
        if(before!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE){auto& v=barriers[count++];v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            v.Transition={i==1?f.depth:f.motion,0,before,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};}
    }
    pipeline.validate_on_frame(f,target_state,true,constants); // Before any transition.
    if(count)f.commands->ResourceBarrier(count,barriers.data());
    auto selected=pipeline.record(mode,f,target_state,true,constants);
    // Preserve host guide states on success. A failure isolates the context;
    // never pretend a partially recorded/failed NR frame can be rolled back.
    if(!selected.resource || selected.ngx_result!=NVSDK_NGX_Result_Success)return selected;
    for(unsigned i=0;i<count;++i)std::swap(barriers[i].Transition.StateBefore,barriers[i].Transition.StateAfter);
    if(count)f.commands->ResourceBarrier(count,barriers.data());return selected;
}
}
