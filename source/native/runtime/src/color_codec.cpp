// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_color_codec.hpp"
#include "lab_color_shader.hpp"
#include <d3dcompiler.h>
#include <thread>
#include <string>
#include <limits>
using Microsoft::WRL::ComPtr;
namespace lab::nr {
namespace {
void hr(HRESULT v) { if(FAILED(v))throw std::runtime_error("Color codec D3D12 failure: "+std::to_string(static_cast<unsigned>(v))); }
ComPtr<IUnknown> identity(IUnknown* v) { ComPtr<IUnknown> result;if(!v)throw std::logic_error("Null codec object");hr(v->QueryInterface(IID_PPV_ARGS(&result)));return result; }
void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    if(a==b)return;D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={resource,0,a,b};list->ResourceBarrier(1,&v);
}
}
struct ColorCodec::Impl {
    ComPtr<ID3D12Device> device;
    std::array<ComPtr<ID3D12Resource>,4> textures;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,2> pipelines;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    const std::thread::id owner=std::this_thread::get_id();
    enum class Stage { idle, prepared, recorded, pending } stage=Stage::idle;
    unsigned width=0,height=0,stride=0;
    bool first=true;
    std::uint64_t fence_value=0,pre_count=0,post_count=0;
    preconvert::Constants constants{};
    void check_owner() const { if(owner!=std::this_thread::get_id())throw std::logic_error("Color codec render-thread owner mismatch"); }
    void dispatch(unsigned pass) {
        ID3D12DescriptorHeap* heaps[]{heap.Get()};commands->SetDescriptorHeaps(1,heaps);
        commands->SetComputeRootSignature(root.Get());commands->SetPipelineState(pipelines[pass].Get());
        auto h=heap->GetGPUDescriptorHandleForHeapStart();h.ptr+=pass*5ULL*stride;
        commands->SetComputeRootDescriptorTable(0,h);h.ptr+=4ULL*stride;commands->SetComputeRootDescriptorTable(1,h);
        commands->SetComputeRoot32BitConstants(2,16,&constants,0);commands->Dispatch((width+15)/16,(height+15)/16,1);
    }
    ~Impl() {
        // No fence completion => never silently release GPU-referenced objects.
        // Game host must stop/isolate this failed context, not retry it.
        if(stage!=Stage::idle){for(auto& t:textures)(void)t.Detach();for(auto& p:pipelines)(void)p.Detach();
            (void)root.Detach();(void)heap.Detach();(void)commands.Detach();(void)fence.Detach();(void)device.Detach();}
    }
};
ColorCodec::ColorCodec(ID3D12Device* device,const ColorInputs& input):impl_(std::make_unique<Impl>()) {
    if(!device || !input.states_and_distinct_allocations_established)throw std::logic_error("Codec state/allocation contract missing");
    auto& s=*impl_;s.device=device;s.textures={input.original,input.prepared,input.neural,input.composite};
    std::array<ComPtr<IUnknown>,4> ids;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    for(unsigned i=0;i<4;++i){
        ids[i]=identity(s.textures[i].Get());for(unsigned j=0;j<i;++j)if(ids[i]==ids[j])throw std::logic_error("Codec resource aliases refused");
        ComPtr<ID3D12Device> d;hr(s.textures[i]->GetDevice(IID_PPV_ARGS(&d)));if(identity(d.Get())!=identity(device))throw std::logic_error("Codec device mismatch");
        const auto v=s.textures[i]->GetDesc();if(i==0){s.width=static_cast<unsigned>(v.Width);s.height=v.Height;format=v.Format;}
        if(v.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || !v.Width || v.Width>8192 || !v.Height || v.Height>8192 ||
           v.Width!=s.width || v.Height!=s.height || v.DepthOrArraySize!=1 || v.MipLevels!=1 || v.SampleDesc.Count!=1 || v.SampleDesc.Quality ||
           v.Format!=format || (format!=DXGI_FORMAT_R16G16B16A16_FLOAT && format!=DXGI_FORMAT_R32G32B32A32_FLOAT) ||
           (v.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || (i && !(v.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
            throw std::logic_error("Unsupported native color texture contract");
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};hr(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)));
    if(!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD) || !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))throw std::logic_error("Codec format lacks shader load/UAV store");
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    D3D12_ROOT_PARAMETER params[3]{};for(unsigned i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,16};
    D3D12_ROOT_SIGNATURE_DESC rd{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
    hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&s.root)));
    const char* entries[]{"Prepare","Composite"};for(unsigned i=0;i<2;++i){blob.Reset();error.Reset();
        const auto compiled=D3DCompile(color_shader,sizeof(color_shader)-1,"lab_color_shader",nullptr,nullptr,entries[i],"cs_5_1",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
        if(FAILED(compiled))throw std::runtime_error(error?std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()):"Codec shader compile failed");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=s.root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&s.pipelines[i])));
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=10;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.heap)));
    s.stride=device->GetDescriptorHandleIncrementSize(hd.Type);auto handle=s.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    for(unsigned pass=0;pass<2;++pass){const unsigned mapping[]{0,1,2,0};for(unsigned i=0;i<4;++i){device->CreateShaderResourceView(s.textures[mapping[i]].Get(),&srv,handle);handle.ptr+=s.stride;}
        device->CreateUnorderedAccessView(s.textures[pass?3:1].Get(),nullptr,&uav,handle);handle.ptr+=s.stride;}
}
ColorCodec::~ColorCodec()=default;
void ColorCodec::prepare(ID3D12GraphicsCommandList* commands,const preconvert::Constants& c) {
    auto& s=*impl_;s.check_owner();
    if(s.stage!=Impl::Stage::idle || !commands || commands->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)throw std::logic_error("Codec slot busy or invalid command list");
    preconvert::validate(c,s.width,s.height);
    if(c.size[0]!=s.width || c.size[1]!=s.height || c.source_size[0]!=s.width || c.source_size[1]!=s.height ||
       c.proxy_size[0]!=s.width || c.proxy_size[1]!=s.height || c.source_base[0] || c.source_base[1] || c.hdr>1 ||
       !std::isfinite(c.paper_white) || c.paper_white<=0 || !std::isfinite(c.transfer_strength) || c.transfer_strength<0 || c.transfer_strength>1 ||
       !std::isfinite(c.color_strength) || c.color_strength<0 || c.color_strength>1)
        throw std::logic_error("Codec only admits native extents and finite bounded constants");
    ComPtr<ID3D12Device> actual;hr(commands->GetDevice(IID_PPV_ARGS(&actual)));if(identity(actual.Get())!=identity(s.device.Get()))throw std::logic_error("Codec command device mismatch");
    s.constants=c;s.commands=commands;s.stage=Impl::Stage::prepared;
    if(!s.first)barrier(commands,s.textures[1].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.dispatch(0);barrier(commands,s.textures[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);++s.pre_count;
}
void ColorCodec::composite(ID3D12GraphicsCommandList* commands) {
    auto& s=*impl_;s.check_owner();if(s.stage!=Impl::Stage::prepared || identity(commands)!=identity(s.commands.Get()))throw std::logic_error("Codec post pass must follow prepare on the same command list");
    barrier(commands,s.textures[2].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(!s.first)barrier(commands,s.textures[3].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.dispatch(1);barrier(commands,s.textures[3].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(commands,s.textures[2].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.first=false;s.stage=Impl::Stage::recorded;++s.post_count;
}
void ColorCodec::bind_completion(ID3D12Fence* fence,std::uint64_t value) {
    auto& s=*impl_;s.check_owner();
    if(s.stage!=Impl::Stage::recorded || !fence || !value || value==UINT64_MAX || fence->GetCompletedValue()>=value)throw std::logic_error("A future completion fence is required before submit");
    ComPtr<ID3D12Device> actual;hr(fence->GetDevice(IID_PPV_ARGS(&actual)));if(identity(actual.Get())!=identity(s.device.Get()))throw std::logic_error("Codec fence device mismatch");
    s.fence=fence;s.fence_value=value;s.stage=Impl::Stage::pending;
}
bool ColorCodec::retire_if_complete() {
    auto& s=*impl_;s.check_owner();if(s.stage!=Impl::Stage::pending)throw std::logic_error("Codec has no submitted completion contract");
    const auto value=s.fence->GetCompletedValue();if(value==UINT64_MAX)throw std::runtime_error("Codec device removed; context cannot be reused");
    if(value<s.fence_value)return false;
    hr(s.device->GetDeviceRemovedReason());s.commands.Reset();s.fence.Reset();s.stage=Impl::Stage::idle;return true;
}
std::uint64_t ColorCodec::prepare_count() const {impl_->check_owner();return impl_->pre_count;}
std::uint64_t ColorCodec::composite_count() const {impl_->check_owner();return impl_->post_count;}
}
