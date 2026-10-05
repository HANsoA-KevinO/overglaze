// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_depth_codec.hpp"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <string>
using Microsoft::WRL::ComPtr;
namespace lab::nr {
namespace {
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("Depth codec D3D12 failure: "+std::to_string(unsigned(r)));}
ComPtr<IUnknown> identity(IUnknown* p){if(!p)throw std::logic_error("Null depth object");ComPtr<IUnknown> r;hr(p->QueryInterface(IID_PPV_ARGS(&r)));return r;}
constexpr char shader[]=R"(
Texture2D<float> sourceDepth : register(t0);
RWTexture2D<float> targetDepth : register(u0);
cbuffer Projection : register(b0) { float A,B,C,D; float nearPlane,farPlane; uint inverted,reserved; };
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
    uint w,h;targetDepth.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
    float v=sourceDepth.Load(int3(id.xy,0));
    float result=inverted?0.0:1.0;
    if(isfinite(v)&&v!=0) {
        float z=clamp(abs(v),nearPlane,farPlane)*C;
        result=saturate((z*A+B)/(z*C+D));
    }
    targetDepth[id.xy]=result;
})";
// Depth plane of a depth/stencil resource, copied as it is. The SRV that feeds
// it is R32_FLOAT_X8X24_TYPELESS on plane 0, so the 8-bit stencil and the 24
// padding bits never reach NR; the value is the game's hardware depth unchanged.
constexpr char plane0_shader[]=R"(
Texture2D<float> sourceDepth : register(t0);
RWTexture2D<float> targetDepth : register(u0);
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
    uint w,h;targetDepth.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
    targetDepth[id.xy]=sourceDepth.Load(int3(id.xy,0));
})";
}
struct DepthCodec::Impl {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12Resource> output,input;
    ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso;
    ThreadAccess access;unsigned w,h,stride=0;bool pending=false,crop=false;DepthCodecMode mode;
    Impl(unsigned x,unsigned y,SerialCallGate* g,DepthCodecMode m,bool c):access(g),w(x),h(y),crop(c),mode(m){}
    ~Impl(){if(pending){(void)device.Detach();(void)output.Detach();(void)input.Detach();(void)command.Detach();
        (void)heap.Detach();(void)root.Detach();(void)pso.Detach();}}
};
DepthCodec::DepthCodec(ID3D12Device* d,unsigned w,unsigned h,SerialCallGate* gate,DepthCodecMode mode,bool region_crop):impl_(std::make_unique<Impl>(w,h,gate,mode,region_crop)) {
    if(!d||!guide_extent_supported(w,h,w,h))throw std::logic_error("Invalid depth extent/device");auto& s=*impl_;s.device=d;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R32_FLOAT};hr(d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)));
    if(!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)||!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))throw std::logic_error("Depth typed access unavailable");
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R32_FLOAT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&s.output)));
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    D3D12_ROOT_PARAMETER params[3]{};for(unsigned i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,8};
    D3D12_ROOT_SIGNATURE_DESC rd{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
    hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));hr(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&s.root)));
    blob.Reset();error.Reset();
    // Both copy modes run the same identity shader; only the view differs.
    const bool plane0=mode==DepthCodecMode::depth_stencil_plane0||mode==DepthCodecMode::hardware_mip0_copy;
    const char* source=plane0?plane0_shader:shader;const auto length=plane0?sizeof(plane0_shader)-1:sizeof(shader)-1;
    const auto result=D3DCompile(source,length,plane0?"lab_depth_plane0":"lab_depth_codec",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
    if(FAILED(result))throw std::runtime_error(error?std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()):"Depth compile failed");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=s.root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};hr(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&s.pso)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hr(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.heap)));s.stride=d->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    auto handle=s.heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=s.stride;d->CreateUnorderedAccessView(s.output.Get(),nullptr,&uav,handle);
}
DepthCodec::~DepthCodec()=default;
DepthCodecMode DepthCodec::mode() const{return impl_->mode;}
ID3D12Resource* DepthCodec::output() const{return impl_->output.Get();}
void DepthCodec::validate(ID3D12GraphicsCommandList* cmd,ID3D12Resource* source,const DepthProjection& p) const {
    auto& s=*impl_;s.access.require();
    const bool plane0=s.mode==DepthCodecMode::depth_stencil_plane0;
    const bool mip0=s.mode==DepthCodecMode::hardware_mip0_copy;
    if(!plane0&&!mip0)p.validate(); // the copies do not use a projection
    if(s.pending||!cmd||!source||cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)throw std::logic_error("Depth slot pending or missing input");
    ComPtr<ID3D12Device> actual;hr(cmd->GetDevice(IID_PPV_ARGS(&actual)));if(identity(actual.Get())!=identity(s.device.Get()))throw std::logic_error("Depth command device mismatch");
    actual.Reset();hr(source->GetDevice(IID_PPV_ARGS(&actual)));if(identity(actual.Get())!=identity(s.device.Get())||identity(source)==identity(s.output.Get()))throw std::logic_error("Depth source device/alias mismatch");
    // The game's own linear depth may be single or half precision; both are read
    // through a typed SRV that converts to float, and this codec's private output
    // stays R32_FLOAT either way. Which one a game uses is its own decision about
    // its buffer, not a precision we are choosing.
    // Each mode accepts exactly its own spelling. A two-plane resource offered
    // to the projection mode, or a single-plane one offered to the plane copy,
    // is a caller mix-up and is refused rather than read through a wrong view.
    const auto d=source->GetDesc();const bool format_ok=plane0?d.Format==DXGI_FORMAT_R32G8X24_TYPELESS:
        mip0?(d.Format==DXGI_FORMAT_R32_FLOAT||d.Format==DXGI_FORMAT_R32_TYPELESS||d.Format==DXGI_FORMAT_R16_FLOAT):
        (d.Format==DXGI_FORMAT_R32_FLOAT||d.Format==DXGI_FORMAT_R16_FLOAT);
    // Any mip count: the view below reads level 0 only (Width/Height are level 0's).
    const bool extent_ok=s.crop?(d.Width>=s.w&&d.Height>=s.h):(d.Width==s.w&&d.Height==s.h);
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||!extent_ok||!format_ok||
        d.DepthOrArraySize!=1||d.MipLevels<1||d.SampleDesc.Count!=1||d.SampleDesc.Quality||d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
        throw std::logic_error("Depth source layout mismatch");
}
void DepthCodec::record(ID3D12GraphicsCommandList* cmd,ID3D12Resource* source,const DepthProjection& p){
    validate(cmd,source,p);auto& s=*impl_;s.input=source;s.command=cmd;s.pending=true;
    // The view must carry the source's own format; the shader reads float either way.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    // A typeless depth/stencil resource needs the plane-0 view format and an
    // explicit plane slice; any single-plane source uses its own format.
    if(s.mode==DepthCodecMode::depth_stencil_plane0){srv.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;srv.Texture2D.PlaneSlice=0;}
    else{const auto f=source->GetDesc().Format;srv.Format=f==DXGI_FORMAT_R32_TYPELESS?DXGI_FORMAT_R32_FLOAT:f;}
    srv.Texture2D.MostDetailedMip=0; // level 0 of any chain; MipLevels=1 above
    s.device->CreateShaderResourceView(source,&srv,s.heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={s.output.Get(),0,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};cmd->ResourceBarrier(1,&barrier);
    ID3D12DescriptorHeap* heaps[]{s.heap.Get()};cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(s.root.Get());cmd->SetPipelineState(s.pso.Get());
    auto h=s.heap->GetGPUDescriptorHandleForHeapStart();cmd->SetComputeRootDescriptorTable(0,h);h.ptr+=s.stride;cmd->SetComputeRootDescriptorTable(1,h);
    cmd->SetComputeRoot32BitConstants(2,8,&p,0);cmd->Dispatch((s.w+7)/8,(s.h+7)/8,1);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);cmd->ResourceBarrier(1,&barrier);
}
void DepthCodec::acknowledge_completion(){auto& s=*impl_;s.access.require();if(!s.pending)throw std::logic_error("No depth conversion to retire");s.input.Reset();s.command.Reset();s.pending=false;}
void DepthCodec::discard_recording(){auto& s=*impl_;s.access.require();if(!s.pending)throw std::logic_error("No depth conversion to discard");s.input.Reset();s.command.Reset();s.pending=false;}
}
