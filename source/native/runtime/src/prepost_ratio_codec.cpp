// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_prepost_ratio_codec.hpp"
#include "lab_prepost_ratio_shader.hpp"
#include "lab_nr_settings.hpp"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <thread>
#include <string>
#include <stdexcept>
#include <cmath>
using Microsoft::WRL::ComPtr;
namespace lab::nr {
namespace {
void hr(HRESULT v) {if(FAILED(v))throw std::runtime_error("Pre/post color D3D12 failure: "+std::to_string(static_cast<unsigned>(v)));}
// What a colour texture may be. The working images must hold the full working
// range, so they stay 16- or 32-bit float per channel. A game-facing image only
// has to round-trip the game's own data, so it may also be the packed 11/11/10
// float some games choose for their RR output -- a format with no alpha and no
// sign bit, which is a decision that game already made about its own buffer.
bool supported_color_format(DXGI_FORMAT f,bool game_facing) noexcept {
    if(f==DXGI_FORMAT_R16G16B16A16_FLOAT||f==DXGI_FORMAT_R32G32B32A32_FLOAT)return true;
    return game_facing&&f==DXGI_FORMAT_R11G11B10_FLOAT;
}
// The root constants: the caller's constants, then the codec's own block
// (cbuffer Extrapolate + padding to a whole 16-byte register).
struct ShaderConstants {
    PrePostColorConstants base{};
    float extrapolate=1;std::uint32_t pad[3]{};
};
static_assert(sizeof(ShaderConstants)==144);
constexpr UINT shader_constant_count=sizeof(ShaderConstants)/4;
constexpr unsigned srv_count=5,uav_count=3,descriptors_per_pass=srv_count+uav_count;
ComPtr<IUnknown> identity(IUnknown* v) {
    if(!v)throw std::logic_error("Null pre/post codec object");
    ComPtr<IUnknown> result;hr(v->QueryInterface(IID_PPV_ARGS(&result)));return result;
}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,0,a,b};list->ResourceBarrier(1,&v);
}
void validate(const PrePostColorConstants& c,unsigned w,unsigned h) {
    if(c.width!=w || c.height!=h || c.pre_post>1 || c.tonemap_operator>4 || c.reserved ||
       !std::isfinite(c.exposure) || c.exposure<=0 || !std::isfinite(c.sigma) || c.sigma<=0 ||
       !std::isfinite(c.gamma) || c.gamma<=0)throw std::logic_error("Explicit finite pre/post color parameters required");
    for(unsigned r=0;r<3;++r)for(unsigned j=0;j<4;++j) {
        if(!std::isfinite(c.pre_matrix[r][j]) || !std::isfinite(c.post_matrix[r][j]) ||
           (j==3 && (c.pre_matrix[r][j]!=0 || c.post_matrix[r][j]!=0)))throw std::logic_error("Invalid color matrix");
    }
    // Refuse a mismatched inverse rather than silently computing/replacing it.
    for(unsigned r=0;r<3;++r)for(unsigned col=0;col<3;++col) {
        double v=0;for(unsigned i=0;i<3;++i)v+=static_cast<double>(c.post_matrix[r][i])*c.pre_matrix[i][col];
        if(!std::isfinite(v) || std::abs(v-(r==col?1.:0.))>1e-5)throw std::logic_error("Post matrix is not the pre inverse");
    }
}
}
struct PrePostColorCodec::Impl {
    ComPtr<ID3D12Device> device;
    std::array<ComPtr<ID3D12Resource>,6> textures;
    // The game's own colour format, carried by roles 0 and 3 only.
    DXGI_FORMAT game_format=DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,2> pipelines;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ThreadAccess access;
    DeviceIdentity device_identity;
    explicit Impl(SerialCallGate* gate,DeviceIdentity policy):access(gate),device_identity(policy){}
    enum class Stage { idle, prepared, recorded, pending } stage=Stage::idle;
    unsigned width=0,height=0,stride=0;
    bool first_prepare=true,first_composite=true,compare_split=false;unsigned guard_flags=0;
    float extrapolation=1.f; // set_extrapolation; recorded into the next prepare
    std::uint64_t fence_value=0,pre_count=0,post_count=0;
    ShaderConstants constants{};
    void check_owner() const {access.require();}
    void dispatch(unsigned pass) {
        ID3D12DescriptorHeap* heaps[]{heap.Get()};commands->SetDescriptorHeaps(1,heaps);
        commands->SetComputeRootSignature(root.Get());commands->SetPipelineState(pipelines[pass].Get());
        auto h=heap->GetGPUDescriptorHandleForHeapStart();h.ptr+=pass*UINT64(descriptors_per_pass)*stride;
        commands->SetComputeRootDescriptorTable(0,h);h.ptr+=UINT64(srv_count)*stride;commands->SetComputeRootDescriptorTable(1,h);
        commands->SetComputeRoot32BitConstants(2,shader_constant_count,&constants,0);commands->Dispatch((width+7)/8,(height+7)/8,1);
    }
    ~Impl() {
        // Incomplete/abandoned submission is a fatal host-context error. Keep
        // potentially GPU-referenced objects alive; never pretend recovery.
        if(stage!=Stage::idle) {
            for(auto& t:textures)(void)t.Detach();for(auto& p:pipelines)(void)p.Detach();
            (void)root.Detach();(void)heap.Detach();(void)commands.Detach();(void)fence.Detach();(void)device.Detach();
        }
    }
};
PrePostColorCodec::PrePostColorCodec(ID3D12Device* device,const PrePostColorInputs& input,SerialCallGate* gate,DeviceIdentity policy):impl_(std::make_unique<Impl>(gate,policy)) {
    if(!device || !input.states_and_distinct_allocations_established ||
       (!input.finite_nonnegative_working_rgb_established && !input.guard_invalid_rgb))
        throw std::logic_error("Pre/post state/allocation/linear color-domain contract missing");
    auto& s=*impl_;s.device=device;
    s.textures={input.original,input.prepared,input.neural,input.composite,input.working_hdr,input.sdr_linear};
    std::array<ComPtr<IUnknown>,6> ids;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    for(unsigned i=0;i<6;++i) {
        ids[i]=identity(s.textures[i].Get());for(unsigned j=0;j<i;++j)if(ids[i]==ids[j])throw std::logic_error("Pre/post resource aliases refused");
        ComPtr<ID3D12Device> d;hr(s.textures[i]->GetDevice(IID_PPV_ARGS(&d)));if(!s.device_identity.same(d.Get(),device))throw std::logic_error("Pre/post resource device mismatch");
        const auto v=s.textures[i]->GetDesc();if(i==0){s.width=static_cast<unsigned>(v.Width);s.height=v.Height;}
        // Roles 0 and 3 are the only ones that meet the game: 0 receives a copy of
        // its colour and 3 is copied back. CopyResource demands identical formats,
        // so those two carry the GAME's format while the internal working images
        // stay at the pipeline's own. Everything the model sees is internal.
        const bool game_facing=i==0||i==3;
        if(i==0)s.game_format=v.Format;
        if(i==1)format=v.Format; // the first internal image fixes the working format
        const auto expected=game_facing?s.game_format:format;
        if(v.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || !v.Width || v.Width>8192 || !v.Height || v.Height>8192 ||
           v.Width!=s.width || v.Height!=s.height || v.DepthOrArraySize!=1 || v.MipLevels!=1 || v.SampleDesc.Count!=1 || v.SampleDesc.Quality ||
           v.Format!=expected || !supported_color_format(v.Format,game_facing) ||
           (v.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || (i && !(v.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
            throw std::logic_error("Unsupported native pre/post color texture contract");
    }
    // The guard follows the WORKING format, because the range it guards is the
    // one the internal images are stored in, not the game's.
    s.guard_flags=input.guard_invalid_rgb?(format==DXGI_FORMAT_R16G16B16A16_FLOAT?3u:1u):0u;
    for(const auto f:{format,s.game_format}){
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{f};hr(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)));
        if(!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD) || !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))throw std::logic_error("Pre/post format support missing");
    }
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,srv_count,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,uav_count,0,0,0}};
    D3D12_ROOT_PARAMETER params[3]{};
    for(unsigned i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,shader_constant_count};
    D3D12_ROOT_SIGNATURE_DESC rd{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
    hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&s.root)));
    const char* entries[]{"Prepare","Composite"};
    for(unsigned i=0;i<2;++i) {
        blob.Reset();error.Reset();auto rv=D3DCompile(prepost_ratio_shader,sizeof(prepost_ratio_shader)-1,"lab_prepost_ratio_shader",nullptr,nullptr,
            entries[i],"cs_5_1",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
        if(FAILED(rv))throw std::runtime_error(error?std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()):"Pre/post shader compile failed");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=s.root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
        hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&s.pipelines[i])));
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=2*descriptors_per_pass;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.heap)));s.stride=device->GetDescriptorHandleIncrementSize(hd.Type);
    auto h=s.heap->GetCPUDescriptorHandleForHeapStart();
    // Each view carries its own texture's format; roles 0 and 3 may differ from
    // the rest. A typed SRV load and a typed UAV store both convert to and from
    // float4 in the shader, so the shader maths is untouched by this.
    auto role_format=[&](unsigned i){return i==0||i==3?s.game_format:format;};
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    for(unsigned pass=0;pass<2;++pass) {
        const unsigned inputs[]{0,2,4,5};for(auto i:inputs){srv.Format=role_format(i);device->CreateShaderResourceView(s.textures[i].Get(),&srv,h);h.ptr+=s.stride;}
        // t4: the prepared NR input C, read by Composite only for extrapolation.
        // Prepare writes that texture as u0, so its own table holds a null view.
        srv.Format=role_format(1);device->CreateShaderResourceView(pass?s.textures[1].Get():nullptr,&srv,h);h.ptr+=s.stride;
        const unsigned outputs[]{pass?3U:1U,4,5};
        for(unsigned i=0;i<3;++i){uav.Format=role_format(outputs[i]);device->CreateUnorderedAccessView(pass&&i?nullptr:s.textures[outputs[i]].Get(),nullptr,&uav,h);h.ptr+=s.stride;}
    }
}
PrePostColorCodec::~PrePostColorCodec()=default;
void PrePostColorCodec::validate_prepare(ID3D12GraphicsCommandList* commands,const PrePostColorConstants& c) const {
    auto& s=*impl_;s.check_owner();
    if(s.stage!=Impl::Stage::idle || !commands || commands->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)throw std::logic_error("Pre/post slot busy or command invalid");
    validate(c,s.width,s.height);ComPtr<ID3D12Device> actual;hr(commands->GetDevice(IID_PPV_ARGS(&actual)));
    if(!s.device_identity.same(actual.Get(),s.device.Get()))throw std::logic_error("Pre/post command device mismatch");
}
void PrePostColorCodec::prepare(ID3D12GraphicsCommandList* commands,const PrePostColorConstants& c) {
    validate_prepare(commands,c);auto& s=*impl_;
    // Extrapolation is entered only for a factor other than 1, so 1 (and off)
    // runs exactly the plain composite.
    s.constants.base=c;s.constants.base.reserved=s.guard_flags|(s.compare_split?4u:0u)|(s.extrapolation!=1.f?8u:0u);
    s.constants.extrapolate=s.extrapolation;s.commands=commands;s.stage=Impl::Stage::prepared;
    for(unsigned i:{1U,4U,5U})if(!s.first_prepare)transition(commands,s.textures[i].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.dispatch(0);
    for(unsigned i:{1U,4U,5U})transition(commands,s.textures[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    s.first_prepare=false;++s.pre_count;
}
void PrePostColorCodec::finish_prepare_only(ID3D12GraphicsCommandList* commands) {
    auto& s=*impl_;s.check_owner();
    if(s.stage!=Impl::Stage::prepared || identity(commands)!=identity(s.commands.Get()))throw std::logic_error("Prepare-only finish requires same prepared list");
    s.stage=Impl::Stage::recorded;
}
void PrePostColorCodec::composite(ID3D12GraphicsCommandList* commands) {
    auto& s=*impl_;s.check_owner();
    if(s.stage!=Impl::Stage::prepared || identity(commands)!=identity(s.commands.Get()))throw std::logic_error("Pre/post composite must follow prepare on same list");
    transition(commands,s.textures[2].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(!s.first_composite)transition(commands,s.textures[3].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.dispatch(1);transition(commands,s.textures[3].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(commands,s.textures[2].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.first_composite=false;s.stage=Impl::Stage::recorded;++s.post_count;
}
void PrePostColorCodec::bind_completion(ID3D12Fence* fence,std::uint64_t value) {
    auto& s=*impl_;s.check_owner();
    if(s.stage!=Impl::Stage::recorded || !fence || !value || value==UINT64_MAX || fence->GetCompletedValue()>=value)throw std::logic_error("Future GPU completion required");
    ComPtr<ID3D12Device> d;hr(fence->GetDevice(IID_PPV_ARGS(&d)));if(!s.device_identity.same(d.Get(),s.device.Get()))throw std::logic_error("Pre/post fence device mismatch");
    s.fence=fence;s.fence_value=value;s.stage=Impl::Stage::pending;
}
bool PrePostColorCodec::retire_if_complete() {
    auto& s=*impl_;s.check_owner();if(s.stage!=Impl::Stage::pending)throw std::logic_error("No pre/post completion contract");
    const auto value=s.fence->GetCompletedValue();if(value==UINT64_MAX)throw std::runtime_error("Pre/post device removed; context cannot be reused");
    if(value<s.fence_value)return false;
    hr(s.device->GetDeviceRemovedReason());s.commands.Reset();s.fence.Reset();s.stage=Impl::Stage::idle;return true;
}
void PrePostColorCodec::discard_recorded() {
    auto& s=*impl_;s.check_owner();if(s.stage!=Impl::Stage::pending)throw std::logic_error("No pre/post recording to discard");
    // The owner Reset the command list before any submission: this frame's
    // prepare/composite never ran. No completion is claimed.
    s.commands.Reset();s.fence.Reset();s.fence_value=0;s.stage=Impl::Stage::idle;
}
void PrePostColorCodec::set_compare_split(bool v){impl_->check_owner();if(impl_->stage!=Impl::Stage::idle)throw std::logic_error("Compare split changes only between frames");impl_->compare_split=v;}
bool PrePostColorCodec::compare_split() const {impl_->check_owner();return impl_->compare_split;}
void PrePostColorCodec::set_extrapolation(float factor){impl_->check_owner();
    if(!std::isfinite(factor)||factor<Settings::min_extrapolate_factor||factor>Settings::max_extrapolate_factor)throw std::logic_error("Extrapolation factor must be finite 1..4");
    if(impl_->stage!=Impl::Stage::idle)throw std::logic_error("Extrapolation changes only between frames");impl_->extrapolation=factor;}
float PrePostColorCodec::extrapolation() const {impl_->check_owner();return impl_->extrapolation;}
std::uint64_t PrePostColorCodec::prepare_count() const {impl_->check_owner();return impl_->pre_count;}
std::uint64_t PrePostColorCodec::composite_count() const {impl_->check_owner();return impl_->post_count;}
}
