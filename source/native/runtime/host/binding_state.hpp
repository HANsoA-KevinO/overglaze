/* SPDX-FileCopyrightText: 2022 Patrick Mours
 * SPDX-FileCopyrightText: 2026 HANsoA-KevinO
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * State-block structure adapted from ReShade v6.8.0
 * examples/utils/state_tracking.{hpp,cpp}, Copyright (C) 2022 Patrick Mours.
 * Changes: native D3D12 values, both root argument banks, explicit defaults and
 * bounded storage. Not a resource-state snapshot or command-stream replay.
 * Provenance and licence: SOURCE.md and LICENSE-ReShade.txt in this directory.
 */
#pragma once
#include <d3d12.h>
#include <array>
#include <bitset>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include "indirect_layout.hpp"

namespace lab::host {
struct RootArguments {
    enum Kind {unset,table,constants,cbv,srv,uav};
    struct Value {Kind kind=unset;UINT64 address=0;std::array<UINT,64> words{};std::bitset<64> known;};
    ID3D12RootSignature* root=nullptr;
    std::array<Value,64> args{};
    bool valid=true;
    const char* invalid_reason=nullptr;
    void reject(const char* reason){valid=false;if(!invalid_reason)invalid_reason=reason;}
    void signature(ID3D12RootSignature* r){if(root!=r){root=r;args={};}}
    Value* value(UINT i,Kind k){if(!root||i>=64){reject(!root?"argument-before-signature":"root-index-out-of-range");return nullptr;}auto& a=args[i];
        if(a.kind!=unset&&a.kind!=k){reject("root-argument-kind-conflict");return nullptr;}a.kind=k;return &a;}
    void address(UINT i,Kind k,UINT64 v){if(auto* a=value(i,k))a->address=v;}
    void constant(UINT i,UINT n,const void* v,UINT offset){
        if(!n)return;
        if(!v||offset>=64||n>64-offset){reject("root-constant-range-invalid");return;}
        if(auto* a=value(i,constants))for(UINT j=0;j<n;++j){a->words[offset+j]=static_cast<const UINT*>(v)[j];a->known.set(offset+j);}}
    void invalidate_tables(){for(auto& a:args)if(a.kind==table)a={};}
    void apply(ID3D12GraphicsCommandList* c,bool graphics)const{
        if(graphics)c->SetGraphicsRootSignature(root);else c->SetComputeRootSignature(root);
        for(UINT i=0;i<64;++i){const auto& a=args[i];switch(a.kind){
        case table:if(graphics)c->SetGraphicsRootDescriptorTable(i,{a.address});else c->SetComputeRootDescriptorTable(i,{a.address});break;
        case cbv:if(graphics)c->SetGraphicsRootConstantBufferView(i,a.address);else c->SetComputeRootConstantBufferView(i,a.address);break;
        case srv:if(graphics)c->SetGraphicsRootShaderResourceView(i,a.address);else c->SetComputeRootShaderResourceView(i,a.address);break;
        case uav:if(graphics)c->SetGraphicsRootUnorderedAccessView(i,a.address);else c->SetComputeRootUnorderedAccessView(i,a.address);break;
        case constants:
            for(UINT j=0;j<64;){if(!a.known[j]){++j;continue;}const UINT first=j;while(j<64&&a.known[j])++j;
                if(graphics)c->SetGraphicsRoot32BitConstants(i,j-first,a.words.data()+first,first);
                else c->SetComputeRoot32BitConstants(i,j-first,a.words.data()+first,first);}break;
        default:break;}}
    }
};

struct BindingState {
    RootArguments compute,graphics;
    ID3D12PipelineState* pso=nullptr;ID3D12StateObject* rt=nullptr;
    std::array<ID3D12DescriptorHeap*,2> heaps{};UINT heap_count=0;
    ID3D12Resource* predicate=nullptr;UINT64 predicate_offset=0;D3D12_PREDICATION_OP predicate_op{};
    std::array<D3D12_VIEWPORT,16> viewports{};UINT viewport_count=0;
    std::array<D3D12_RECT,16> scissors{};UINT scissor_count=0;
    D3D12_PRIMITIVE_TOPOLOGY topology=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::array<FLOAT,4> blend{1,1,1,1};UINT front_stencil=0,back_stencil=0;
    std::array<D3D12_VERTEX_BUFFER_VIEW,32> vertex{};
    std::array<D3D12_STREAM_OUTPUT_BUFFER_VIEW,4> stream_output{};
    D3D12_INDEX_BUFFER_VIEW index{};bool has_index=false;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,8> targets{};UINT target_count=0;
    D3D12_CPU_DESCRIPTOR_HANDLE depth_target{};bool has_depth=false,targets_known=true;
    float min_depth=0,max_depth=1;
    std::array<D3D12_SAMPLE_POSITION,256> samples{};UINT samples_per_pixel=0,sample_pixels=0;
    UINT view_mask=0;
    D3D12_SHADING_RATE shading_rate=D3D12_SHADING_RATE_1X1;
    std::array<D3D12_SHADING_RATE_COMBINER,2> combiners{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
    ID3D12Resource* shading_image=nullptr;
    float depth_bias=0,depth_bias_clamp=0,slope_bias=0;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut=D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    // Only restore optional-feature setters actually observed on the caller.
    // Calling unsupported dynamic-state APIs merely to restore a default is invalid.
    bool depth_bounds_seen=false,samples_seen=false,view_mask_seen=false,shading_seen=false,shading_image_seen=false;
    bool split_stencil_seen=false,depth_bias_seen=false,strip_cut_seen=false;
    bool reset_seen=false,create_seen=false,closed=true,invalid=false,render_pass=false;
    const char* invalid_reason=nullptr;
    std::uint64_t generation=0;
    std::uint64_t heap_order_permutations=0;
    void reject(const char* reason){invalid=true;if(!invalid_reason)invalid_reason=reason;}
    const char* blocker()const{
        if(!reset_seen&&!create_seen)return "recording-start-unobserved";
        if(closed)return "recording-closed";
        if(invalid)return invalid_reason?invalid_reason:"unsupported-state";
        if(render_pass)return "active-render-pass";
        if(!compute.valid)return "compute-root-arguments-invalid";
        if(!graphics.valid)return "graphics-root-arguments-invalid";
        if(!compute.root)return "compute-root-unobserved";
        if(!pso&&!rt)return "pipeline-state-unobserved";
        return nullptr;
    }
    bool ready()const{return blocker()==nullptr;}
    void set_heaps(UINT n,ID3D12DescriptorHeap* const* h){
        if(n>2||(n&&!h)){reject("descriptor-heap-range-invalid");return;}
        // Descriptor heaps are a set (one resource heap, one sampler heap),
        // not positional slots. Permuting the same set is a redundant bind;
        // D3D12 keeps both banks of descriptor tables valid in that case.
        // Losing them here is invisible during an empty restore, but a real
        // injected heap/root change would then leave the caller's tables unset.
        bool changed=n!=heap_count;
        for(UINT i=0;i<n;++i){
            if(!h[i]){reject("null-descriptor-heap");return;}
            if(i&&h[i]==h[0]){reject("duplicate-descriptor-heap");return;}
            bool found=false;for(UINT j=0;j<heap_count;++j)found|=h[i]==heaps[j];
            changed|=!found;
        }
        if(!changed&&n==2&&heaps[0]!=h[0])++heap_order_permutations;
        heaps={};for(UINT i=0;i<n;++i)heaps[i]=h[i];heap_count=n;
        if(changed){compute.invalidate_tables();graphics.invalidate_tables();}}
    template<class T,std::size_t N> void range(std::array<T,N>& out,UINT first,UINT n,const T* values){
        if(first>N||n>N-first){reject("fixed-state-range-invalid");return;}
        for(UINT i=0;i<n;++i)out[first+i]=values?values[i]:T{};
    }
    void render_targets(UINT n,const D3D12_CPU_DESCRIPTOR_HANDLE* r,BOOL contiguous,const D3D12_CPU_DESCRIPTOR_HANDLE* d,UINT increment){
        // The native observer must replace these handles with owned descriptor
        // snapshots before returning to the caller: RTV/DSV source descriptor
        // heaps are allowed to be recycled immediately after the bind call.
        if(n>targets.size()||(n&&!r)){reject("render-target-range-invalid");return;}target_count=n;targets={};targets_known=true;
        for(UINT i=0;i<n;++i)targets[i]=contiguous?D3D12_CPU_DESCRIPTOR_HANDLE{r[0].ptr+SIZE_T(i)*increment}:r[i];
        has_depth=d!=nullptr;depth_target=d?*d:D3D12_CPU_DESCRIPTOR_HANDLE{};
    }
    void after_indirect(const indirect::Layout& layout){
        if(layout.error!=indirect::Error::none||!layout.count||layout.count>layout.arguments.size()){reject("indirect-signature-unsupported");return;}
        const D3D12_COMMAND_SIGNATURE_DESC desc{layout.stride,layout.count,layout.arguments.data(),0};
        const auto validated=indirect::describe(&desc,reinterpret_cast<ID3D12RootSignature*>(layout.root));
        if(validated.error!=indirect::Error::none||validated.graphics!=layout.graphics){reject("indirect-signature-unsupported");return;}
        auto& bank=layout.graphics?graphics:compute;
        if(layout.root&&layout.root!=reinterpret_cast<std::uint64_t>(bank.root)){reject("indirect-root-identity-mismatch");return;}
        const std::array<UINT,64> zeros{};
        // D3D12 resets only signature-touched bindings to zero/null after the
        // call, even for zero draws. Preserve every untouched argument.
        for(UINT i=0;i+1<layout.count;++i){const auto& a=layout.arguments[i];switch(a.Type){
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:vertex[a.VertexBuffer.Slot]={};break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:index={};has_index=false;break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:bank.constant(a.Constant.RootParameterIndex,a.Constant.Num32BitValuesToSet,zeros.data(),a.Constant.DestOffsetIn32BitValues);break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:bank.address(a.ConstantBufferView.RootParameterIndex,RootArguments::cbv,0);break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:bank.address(a.ShaderResourceView.RootParameterIndex,RootArguments::srv,0);break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:bank.address(a.UnorderedAccessView.RootParameterIndex,RootArguments::uav,0);break;
        default:reject("indirect-signature-unsupported");return;
        }}
    }
    void restore(ID3D12GraphicsCommandList* c)const{
        // Order matters: heaps before root tables, signatures before arguments.
        c->SetDescriptorHeaps(heap_count,heaps.data());
        compute.apply(c,false);graphics.apply(c,true);
        if(pso)c->SetPipelineState(pso);else static_cast<ID3D12GraphicsCommandList4*>(c)->SetPipelineState1(rt);
        // RTV/DSV are undefined after EndRenderPass, not inherited from before
        // BeginRenderPass (Microsoft D3D12 Render Passes specification).
        if(targets_known)c->OMSetRenderTargets(target_count,targets.data(),FALSE,has_depth?&depth_target:nullptr);
        c->RSSetViewports(viewport_count,viewports.data());c->RSSetScissorRects(scissor_count,scissors.data());
        if(topology!=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)c->IASetPrimitiveTopology(topology);
        c->OMSetBlendFactor(blend.data());c->OMSetStencilRef(front_stencil);
        c->IASetIndexBuffer(has_index?&index:nullptr);c->IASetVertexBuffers(0,UINT(vertex.size()),vertex.data());
        c->SOSetTargets(0,UINT(stream_output.size()),stream_output.data());
        auto* v1=static_cast<ID3D12GraphicsCommandList1*>(c);
        if(depth_bounds_seen)v1->OMSetDepthBounds(min_depth,max_depth);
        if(samples_seen)v1->SetSamplePositions(samples_per_pixel,sample_pixels,samples_per_pixel?const_cast<D3D12_SAMPLE_POSITION*>(samples.data()):nullptr);
        if(view_mask_seen)v1->SetViewInstanceMask(view_mask);
        auto* v5=static_cast<ID3D12GraphicsCommandList5*>(c);
        if(shading_seen)v5->RSSetShadingRate(shading_rate,combiners.data());
        if(shading_image_seen)v5->RSSetShadingRateImage(shading_image);
        if(split_stencil_seen)static_cast<ID3D12GraphicsCommandList8*>(c)->OMSetFrontAndBackStencilRef(front_stencil,back_stencil);
        auto* v9=static_cast<ID3D12GraphicsCommandList9*>(c);
        if(depth_bias_seen)v9->RSSetDepthBias(depth_bias,depth_bias_clamp,slope_bias);
        if(strip_cut_seen)v9->IASetIndexBufferStripCutValue(strip_cut);
        c->SetPredication(predicate,predicate_offset,predicate_op);
    }
};
}
