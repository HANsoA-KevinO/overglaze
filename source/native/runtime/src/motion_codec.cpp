// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_motion_codec.hpp"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
namespace lab::nr {
namespace {
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("Motion codec D3D12 failure: "+std::to_string(unsigned(r)));}
ComPtr<IUnknown> identity(IUnknown* p){if(!p)throw std::logic_error("Null motion object");ComPtr<IUnknown> r;hr(p->QueryInterface(IID_PPV_ARGS(&r)));return r;}
// The source pixel under each guide pixel's centre, from the top-left region.
// Integer arithmetic, so a region equal to the target is an exact crop.
constexpr char shader[]=R"(
Texture2D<float4> sourceMotion : register(t0);
RWTexture2D<float2> targetMotion : register(u0);
cbuffer Region : register(b0) { uint regionWidth, regionHeight, reserved0, reserved1; };
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
    uint w,h;targetMotion.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
    uint2 at=uint2(min(regionWidth-1,(id.x*2+1)*regionWidth/(2*w)),min(regionHeight-1,(id.y*2+1)*regionHeight/(2*h)));
    targetMotion[id.xy]=sourceMotion.Load(int3(at,0)).xy;
})";
bool supported(unsigned w,unsigned h){return w&&h&&w<=8192&&h<=8192;}
}
struct MotionCodec::Impl {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12Resource> output,input;ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso;
    ThreadAccess access;unsigned w,h,stride=0;bool pending=false;
    Impl(unsigned x,unsigned y,SerialCallGate* g):access(g),w(x),h(y){}
    ~Impl(){if(pending){(void)device.Detach();(void)output.Detach();(void)input.Detach();(void)command.Detach();
        (void)heap.Detach();(void)root.Detach();(void)pso.Detach();}}
};
MotionCodec::MotionCodec(ID3D12Device* d,unsigned w,unsigned h,SerialCallGate* gate):impl_(std::make_unique<Impl>(w,h,gate)){
    if(!d||!supported(w,h))throw std::logic_error("Invalid motion extent/device");auto& s=*impl_;s.device=d;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R16G16_FLOAT};hr(d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)));
    if(!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))throw std::logic_error("Motion typed store unavailable");
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R16G16_FLOAT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&s.output)));
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    D3D12_ROOT_PARAMETER params[3]{};for(unsigned i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,4};
    D3D12_ROOT_SIGNATURE_DESC rd{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
    hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));hr(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&s.root)));
    blob.Reset();error.Reset();
    const auto result=D3DCompile(shader,sizeof(shader)-1,"lab_motion_codec",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
    if(FAILED(result))throw std::runtime_error(error?std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()):"Motion compile failed");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=s.root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};hr(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&s.pso)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=2;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.heap)));s.stride=d->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R16G16_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    auto handle=s.heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=s.stride;d->CreateUnorderedAccessView(s.output.Get(),nullptr,&uav,handle);
}
MotionCodec::~MotionCodec()=default;
ID3D12Resource* MotionCodec::output() const{return impl_->output.Get();}
void MotionCodec::validate(ID3D12GraphicsCommandList* cmd,ID3D12Resource* source,unsigned rw,unsigned rh) const {
    auto& s=*impl_;s.access.require();
    if(s.pending||!cmd||!source||cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)throw std::logic_error("Motion slot pending or missing input");
    ComPtr<ID3D12Device> actual;hr(cmd->GetDevice(IID_PPV_ARGS(&actual)));if(identity(actual.Get())!=identity(s.device.Get()))throw std::logic_error("Motion command device mismatch");
    actual.Reset();hr(source->GetDevice(IID_PPV_ARGS(&actual)));if(identity(actual.Get())!=identity(s.device.Get())||identity(source)==identity(s.output.Get()))throw std::logic_error("Motion source device/alias mismatch");
    const auto d=source->GetDesc();
    if(!supported(rw,rh)||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Width<rw||d.Height<rh||
       (d.Format!=DXGI_FORMAT_R16G16_FLOAT&&d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)||
       d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||d.SampleDesc.Quality||d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
        throw std::logic_error("Motion source layout mismatch");
}
void MotionCodec::record(ID3D12GraphicsCommandList* cmd,ID3D12Resource* source,unsigned rw,unsigned rh){
    validate(cmd,source,rw,rh);auto& s=*impl_;s.input=source;s.command=cmd;s.pending=true;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Format=source->GetDesc().Format;
    s.device->CreateShaderResourceView(source,&srv,s.heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={s.output.Get(),0,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};cmd->ResourceBarrier(1,&barrier);
    ID3D12DescriptorHeap* heaps[]{s.heap.Get()};cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(s.root.Get());cmd->SetPipelineState(s.pso.Get());
    auto h=s.heap->GetGPUDescriptorHandleForHeapStart();cmd->SetComputeRootDescriptorTable(0,h);h.ptr+=s.stride;cmd->SetComputeRootDescriptorTable(1,h);
    const unsigned region[4]{rw,rh,0,0};cmd->SetComputeRoot32BitConstants(2,4,region,0);cmd->Dispatch((s.w+7)/8,(s.h+7)/8,1);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);cmd->ResourceBarrier(1,&barrier);
}
void MotionCodec::acknowledge_completion(){auto& s=*impl_;s.access.require();if(!s.pending)throw std::logic_error("No motion resampling to retire");s.input.Reset();s.command.Reset();s.pending=false;}
void MotionCodec::discard_recording(){auto& s=*impl_;s.access.require();if(!s.pending)throw std::logic_error("No motion resampling to discard");s.input.Reset();s.command.Reset();s.pending=false;}
}
