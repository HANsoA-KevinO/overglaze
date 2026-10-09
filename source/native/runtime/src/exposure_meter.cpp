// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_exposure_meter.hpp"
#include "lab_game_exposure.hpp"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
namespace lab::nr {
namespace {
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("Exposure meter D3D12 failure: "+std::to_string(unsigned(r)));}
ComPtr<IUnknown> identity(IUnknown* p){if(!p)throw std::logic_error("Null meter object");ComPtr<IUnknown> r;hr(p->QueryInterface(IID_PPV_ARGS(&r)));return r;}
void transition(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,0,a,b};c->ResourceBarrier(1,&v);}
constexpr unsigned kMeterBytes=16;      // int32 sum of group means (12.12 fixed), uint32 group count, 2 pad
constexpr float kFixedScale=4096.f;
constexpr char shader[]=R"(
Texture2D<float4> Source : register(t0);
RWByteAddressBuffer Meter : register(u0);
cbuffer Params : register(b0) { uint Width, Height, Step, Reserved; };
groupshared float partial[256];
groupshared uint counts[256];
[numthreads(16,16,1)] void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    uint2 p = (gid.xy * 16 + gtid.xy) * Step;
    float v = 0; uint n = 0;
    if (p.x < Width && p.y < Height) {
        float3 c = Source.Load(int3(p, 0)).rgb;
        if (all(isfinite(c)) && all(c >= 0)) {
            float y = dot(c, float3(0.2126, 0.7152, 0.0722));
            v = log2(max(y, 1e-5)); n = 1;
        }
    }
    partial[gi] = v; counts[gi] = n;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128; s > 0; s >>= 1) {
        if (gi < s) { partial[gi] += partial[gi + s]; counts[gi] += counts[gi + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0 && counts[0] > 0) {
        float mean = partial[0] / counts[0];
        int fixedMean = (int)round(mean * 4096.0);
        Meter.InterlockedAdd(0, (uint)fixedMean);
        Meter.InterlockedAdd(4, 1u);
        Meter.InterlockedAdd(8, counts[0]);
    }
})";
}
struct ExposureMeter::Impl {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12Resource> meter,zeros,readback,input;
    ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso;
    ThreadAccess access;unsigned w,h,step,stride=0;bool pending=false;
    Impl(unsigned x,unsigned y,SerialCallGate* g):access(g),w(x),h(y),step(std::max(1u,(std::max(x,y)+1023)/1024)){}
    ~Impl(){if(pending){(void)device.Detach();(void)meter.Detach();(void)zeros.Detach();(void)readback.Detach();(void)input.Detach();
        (void)command.Detach();(void)heap.Detach();(void)root.Detach();(void)pso.Detach();}}
};
ExposureMeter::ExposureMeter(ID3D12Device* d,unsigned w,unsigned h,SerialCallGate* gate):impl_(std::make_unique<Impl>(w,h,gate)){
    if(!d||!w||!h||w>8192||h>8192)throw std::logic_error("Invalid meter extent/device");auto& s=*impl_;s.device=d;
    D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=kMeterBytes;b.Height=1;b.DepthOrArraySize=1;b.MipLevels=1;
    b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;b.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&s.meter)));
    b.Flags=D3D12_RESOURCE_FLAG_NONE;heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&s.zeros)));
    {void* mapped=nullptr;D3D12_RANGE none{};hr(s.zeros->Map(0,&none,&mapped));std::memset(mapped,0,kMeterBytes);s.zeros->Unmap(0,nullptr);}
    heap.Type=D3D12_HEAP_TYPE_READBACK;
    hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback)));
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    D3D12_ROOT_PARAMETER params[3]{};for(unsigned i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,4};
    D3D12_ROOT_SIGNATURE_DESC rd{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
    hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));hr(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&s.root)));
    blob.Reset();error.Reset();
    const auto compiled=D3DCompile(shader,sizeof(shader)-1,"lab_exposure_meter",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
    if(FAILED(compiled))throw std::runtime_error(error?std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()):"Meter compile failed");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=s.root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};hr(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&s.pso)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hr(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.heap)));
    s.stride=d->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements=kMeterBytes/4;uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
    auto handle=s.heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=s.stride;d->CreateUnorderedAccessView(s.meter.Get(),nullptr,&uav,handle);
}
ExposureMeter::~ExposureMeter()=default;
bool ExposureMeter::pending() const{impl_->access.require();return impl_->pending;}
unsigned ExposureMeter::step() const{return impl_->step;}
void ExposureMeter::validate(ID3D12GraphicsCommandList* cmd,ID3D12Resource* source) const{
    auto& s=*impl_;s.access.require();
    if(s.pending||!cmd||!source||cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)throw std::logic_error("Meter slot pending or missing input");
    ComPtr<ID3D12Device> actual;hr(cmd->GetDevice(IID_PPV_ARGS(&actual)));if(identity(actual.Get())!=identity(s.device.Get()))throw std::logic_error("Meter command device mismatch");
    actual.Reset();hr(source->GetDevice(IID_PPV_ARGS(&actual)));if(identity(actual.Get())!=identity(s.device.Get()))throw std::logic_error("Meter source device mismatch");
    const auto d=source->GetDesc();
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Width!=s.w||d.Height!=s.h||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||d.SampleDesc.Quality||
       // The meter reads the snapshot of the game's own colour, which carries the
       // game's format; the view below already follows it and a typed load
       // converts to float either way. The packed 11/11/10 float is admitted for
       // the same reason the colour role admits it.
       (d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&d.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT&&
        d.Format!=DXGI_FORMAT_R11G11B10_FLOAT)||(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        throw std::logic_error("Meter source layout mismatch: format="+std::to_string(unsigned(d.Format))+
            " "+std::to_string(d.Width)+"x"+std::to_string(d.Height)+" expected "+std::to_string(s.w)+"x"+std::to_string(s.h));
}
void ExposureMeter::record(ID3D12GraphicsCommandList* cmd,ID3D12Resource* source){
    validate(cmd,source);auto& s=*impl_;s.input=source;s.command=cmd;s.pending=true;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=source->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;s.device->CreateShaderResourceView(source,&srv,s.heap->GetCPUDescriptorHandleForHeapStart());
    transition(cmd,s.meter.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyBufferRegion(s.meter.Get(),0,s.zeros.Get(),0,kMeterBytes);
    transition(cmd,s.meter.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[]{s.heap.Get()};cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(s.root.Get());cmd->SetPipelineState(s.pso.Get());
    auto h=s.heap->GetGPUDescriptorHandleForHeapStart();cmd->SetComputeRootDescriptorTable(0,h);h.ptr+=s.stride;cmd->SetComputeRootDescriptorTable(1,h);
    const unsigned constants[4]{s.w,s.h,s.step,0};cmd->SetComputeRoot32BitConstants(2,4,constants,0);
    const unsigned tile=16*s.step;cmd->Dispatch((s.w+tile-1)/tile,(s.h+tile-1)/tile,1);
    transition(cmd,s.meter.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(s.readback.Get(),0,s.meter.Get(),0,kMeterBytes);
    transition(cmd,s.meter.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void ExposureMeter::discard_recording(){
    auto& s=*impl_;s.access.require();if(!s.pending)throw std::logic_error("No meter recording to discard");
    s.input.Reset();s.command.Reset();s.pending=false; // readback never written; not read
}
ExposureReading ExposureMeter::acknowledge_completion(){
    auto& s=*impl_;s.access.require();if(!s.pending)throw std::logic_error("No meter reading to retire");
    ExposureReading r;void* mapped=nullptr;D3D12_RANGE range{0,kMeterBytes};hr(s.readback->Map(0,&range,&mapped));
    std::int32_t sum=0;std::uint32_t groups=0,samples=0;
    std::memcpy(&sum,mapped,4);std::memcpy(&groups,static_cast<char*>(mapped)+4,4);std::memcpy(&samples,static_cast<char*>(mapped)+8,4);
    D3D12_RANGE none{};s.readback->Unmap(0,&none);
    s.input.Reset();s.command.Reset();s.pending=false;
    if(groups){r.valid=true;r.mean_log2_luminance=static_cast<float>(sum)/kFixedScale/static_cast<float>(groups);r.groups=groups;r.samples=samples;}
    return r;
}
namespace {
constexpr unsigned kExposureBytes=16; // one float, padded
constexpr char exposure_shader[]=R"(
Texture2D<float4> Source : register(t0);
RWByteAddressBuffer Value : register(u0);
[numthreads(1,1,1)] void main() { Value.Store(0, asuint(Source.Load(int3(0, 0, 0)).r)); }
)";
}
struct GameExposureReader::Impl {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12Resource> value,readback,input;
    ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso;
    ThreadAccess access;unsigned stride=0;bool pending=false;
    explicit Impl(SerialCallGate* g):access(g){}
    // Work that may still be in flight is never released (as the meter does).
    ~Impl(){if(pending){(void)device.Detach();(void)value.Detach();(void)readback.Detach();(void)input.Detach();
        (void)command.Detach();(void)heap.Detach();(void)root.Detach();(void)pso.Detach();}}
};
GameExposureReader::GameExposureReader(ID3D12Device* d,SerialCallGate* gate):impl_(std::make_unique<Impl>(gate)){
    if(!d)throw std::logic_error("Invalid exposure reader device");auto& s=*impl_;s.device=d;
    D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=kExposureBytes;b.Height=1;b.DepthOrArraySize=1;b.MipLevels=1;
    b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;b.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&s.value)));
    b.Flags=D3D12_RESOURCE_FLAG_NONE;heap.Type=D3D12_HEAP_TYPE_READBACK;
    hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback)));
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    D3D12_ROOT_PARAMETER params[2]{};for(unsigned i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
    D3D12_ROOT_SIGNATURE_DESC rd{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
    hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));hr(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&s.root)));
    blob.Reset();error.Reset();
    const auto compiled=D3DCompile(exposure_shader,sizeof(exposure_shader)-1,"lab_game_exposure",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
    if(FAILED(compiled))throw std::runtime_error(error?std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()):"Exposure reader compile failed");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=s.root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};hr(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&s.pso)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hr(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.heap)));
    s.stride=d->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements=kExposureBytes/4;uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
    auto handle=s.heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=s.stride;d->CreateUnorderedAccessView(s.value.Get(),nullptr,&uav,handle);
}
GameExposureReader::~GameExposureReader()=default;
bool GameExposureReader::pending() const{impl_->access.require();return impl_->pending;}
void GameExposureReader::validate(ID3D12GraphicsCommandList* cmd,ID3D12Resource* texture,unsigned declared_state) const{
    auto& s=*impl_;s.access.require();
    if(s.pending||!cmd||!texture||cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)throw std::logic_error("Exposure reader slot pending or missing input");
    ComPtr<ID3D12Device> actual;hr(cmd->GetDevice(IID_PPV_ARGS(&actual)));if(identity(actual.Get())!=identity(s.device.Get()))throw std::logic_error("Exposure reader command device mismatch");
    actual.Reset();hr(texture->GetDevice(IID_PPV_ARGS(&actual)));if(identity(actual.Get())!=identity(s.device.Get()))throw std::logic_error("Exposure texture device mismatch");
    if(lab::live::inspect_exposure_texture(texture,declared_state)!=lab::live::ExposureNote::none)throw std::logic_error("Exposure texture was not screened");
}
void GameExposureReader::record(ID3D12GraphicsCommandList* cmd,ID3D12Resource* texture,unsigned declared_state){
    validate(cmd,texture,declared_state);auto& s=*impl_;s.input=texture;s.command=cmd;s.pending=true;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=texture->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;s.device->CreateShaderResourceView(texture,&srv,s.heap->GetCPUDescriptorHandleForHeapStart());
    const auto declared=static_cast<D3D12_RESOURCE_STATES>(declared_state),read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if(declared!=read)transition(cmd,texture,declared,read);
    ID3D12DescriptorHeap* heaps[]{s.heap.Get()};cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(s.root.Get());cmd->SetPipelineState(s.pso.Get());
    auto h=s.heap->GetGPUDescriptorHandleForHeapStart();cmd->SetComputeRootDescriptorTable(0,h);h.ptr+=s.stride;cmd->SetComputeRootDescriptorTable(1,h);
    cmd->Dispatch(1,1,1);
    if(declared!=read)transition(cmd,texture,read,declared);
    transition(cmd,s.value.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(s.readback.Get(),0,s.value.Get(),0,kExposureBytes);
    transition(cmd,s.value.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void GameExposureReader::discard_recording(){
    auto& s=*impl_;s.access.require();if(!s.pending)throw std::logic_error("No exposure read to discard");
    s.input.Reset();s.command.Reset();s.pending=false;
}
GameExposureReading GameExposureReader::acknowledge_completion(){
    auto& s=*impl_;s.access.require();if(!s.pending)throw std::logic_error("No exposure read to retire");
    GameExposureReading r;void* mapped=nullptr;D3D12_RANGE range{0,kExposureBytes};hr(s.readback->Map(0,&range,&mapped));
    std::memcpy(&r.value,mapped,4);D3D12_RANGE none{};s.readback->Unmap(0,&none);
    s.input.Reset();s.command.Reset();s.pending=false;r.read=true;return r;
}
}
