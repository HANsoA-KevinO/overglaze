// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_color_handoff.hpp"
#include <wrl/client.h>
#include <array>
#include <thread>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace lab::nr {
namespace {
void hr(HRESULT r) { if(FAILED(r))throw std::runtime_error("Color handoff D3D12 failure"); }
ComPtr<IUnknown> identity(IUnknown* p) {
    if(!p)throw std::logic_error("Missing color handoff object");
    ComPtr<IUnknown> id;hr(p->QueryInterface(IID_PPV_ARGS(&id)));return id;
}
// Subresource 0 of `src` into subresource 0 of `dst`. A single-mip pair keeps
// CopyResource exactly as before; a game target with a mip chain (A Plague
// Tale: Resonance hands DLSS a 13-level output) meets our
// single-mip scratch through its top level only -- the only level DLSS wrote
// and the only one NR reads or writes. The rest of the chain is never touched.
// A cropped target (ABI23) is larger than the scratch: the top-left region of
// the scratch's extent is copied in either direction, at the origin, and the
// rest of the target is neither read nor written.
void copy_top(ID3D12GraphicsCommandList* c,ID3D12Resource* dst,ID3D12Resource* src) {
    const auto d=dst->GetDesc(),s=src->GetDesc();
    if(d.MipLevels==1&&s.MipLevels==1&&d.Width==s.Width&&d.Height==s.Height){c->CopyResource(dst,src);return;}
    D3D12_TEXTURE_COPY_LOCATION to{},from{};
    to.pResource=dst;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=0;
    from.pResource=src;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=0;
    const D3D12_BOX region{0,0,0,static_cast<UINT>(d.Width<s.Width?d.Width:s.Width),d.Height<s.Height?d.Height:s.Height,1};
    c->CopyTextureRegion(&to,0,0,0,&from,&region);
}
void transition(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    if(a==b)return;
    D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,0,a,b};c->ResourceBarrier(1,&v);
}
}
struct ColorHandoff::Impl {
    ComPtr<ID3D12Device> device;
    std::array<ComPtr<ID3D12Resource>,2> scratch;
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    D3D12_RESOURCE_DESC layout{};
    D3D12_RESOURCE_STATES target_state{};
    enum class Stage { idle, snapshot, returned, pending } stage=Stage::idle;
    bool crop=false;
    ThreadAccess access;
    DeviceIdentity device_identity;
    explicit Impl(SerialCallGate* gate,DeviceIdentity policy):access(gate),device_identity(policy){}
    std::uint64_t last_token=0,future=0,snapshots=0,returns=0;
    void check_owner() const {access.require();}
    void check_device(ID3D12DeviceChild* p) const {
        if(!p)throw std::logic_error("Missing handoff device child");
        ComPtr<ID3D12Device> d;hr(p->GetDevice(IID_PPV_ARGS(&d)));
        if(!device_identity.same(d.Get(),device.Get()))throw std::logic_error("Handoff device mismatch");
    }
    // The game's target may carry a mip chain; the scratch pair never does.
    void check_texture(ID3D12Resource* r,bool target=false) const {
        check_device(r);const auto d=r->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || !d.Width || d.Width>8192 || !d.Height || d.Height>8192 ||
           (target&&crop?(d.Width<layout.Width || d.Height<layout.Height):(d.Width!=layout.Width || d.Height!=layout.Height)) ||
           d.Format!=layout.Format ||
           // Both handoff images and the game's target share one format, because
           // CopyResource demands it. That format is the GAME's: the packed
           // 11/11/10 float is admitted here for the same reason the colour role
           // admits it, and the internal working images are not part of this pair.
           (d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT && d.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT &&
            d.Format!=DXGI_FORMAT_R11G11B10_FLOAT) ||
           d.DepthOrArraySize!=1 || (target?d.MipLevels<1:d.MipLevels!=1) || d.SampleDesc.Count!=1 || d.SampleDesc.Quality ||
           d.Layout!=D3D12_TEXTURE_LAYOUT_UNKNOWN || (d.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
            throw std::logic_error("Unsupported full-color handoff texture");
    }
    ~Impl() {
        // Unknown GPU completion: host must isolate this context. Do not release
        // resources or a command list behind GPU work, and do not block DllMain.
        if(stage!=Stage::idle){for(auto& r:scratch)(void)r.Detach();(void)target.Detach();
            (void)commands.Detach();(void)fence.Detach();(void)device.Detach();}
    }
};
ColorHandoff::ColorHandoff(ID3D12Device* device,const HandoffInputs& input,SerialCallGate* gate,DeviceIdentity policy):impl_(std::make_unique<Impl>(gate,policy)) {
    if(!device || !input.snapshot || !input.composite || !input.states_and_distinct_allocations_established)
        throw std::logic_error("Handoff scratch contract missing");
    auto& s=*impl_;s.device=device;s.layout=input.snapshot->GetDesc();s.scratch={input.snapshot,input.composite};s.crop=input.target_region_crop;
    for(const auto& r:s.scratch){s.check_texture(r.Get());if(r->GetDesc().Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)throw std::logic_error("Unreadable handoff scratch");}
    if(identity(input.snapshot)==identity(input.composite))throw std::logic_error("Handoff scratch aliases refused");
}
ColorHandoff::~ColorHandoff()=default;
void ColorHandoff::validate_snapshot(ID3D12GraphicsCommandList* commands,ID3D12Resource* target,D3D12_RESOURCE_STATES state,std::uint64_t token,bool contract) const {
    auto& s=*impl_;s.check_owner();
    if(s.stage!=Impl::Stage::idle || !commands || !target || !contract || !token || token<=s.last_token || commands->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)
        throw std::logic_error("Handoff busy or target/recording contract missing");
    s.check_device(commands);s.check_texture(target,true);
    for(const auto& r:s.scratch)if(identity(target)==identity(r.Get()))throw std::logic_error("Target aliases handoff scratch");
    const auto flags=target->GetDesc().Flags;
    const bool read=state==D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE || state==(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if(!(state==D3D12_RESOURCE_STATE_COMMON || read || (state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS && (flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
         (state==D3D12_RESOURCE_STATE_RENDER_TARGET && (flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))) ||
       (read && (flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)))throw std::logic_error("Unverified target state/access");
}
void ColorHandoff::snapshot(ID3D12GraphicsCommandList* commands,ID3D12Resource* target,D3D12_RESOURCE_STATES state,std::uint64_t token,bool contract) {
    validate_snapshot(commands,target,state,token,contract);auto& s=*impl_;
    // All preflight checks precede the first recording/mutation.
    s.commands=commands;s.target=target;s.target_state=state;s.last_token=token;s.stage=Impl::Stage::snapshot;
    transition(commands,target,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(commands,s.scratch[0].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    copy_top(commands,s.scratch[0].Get(),target);
    transition(commands,s.scratch[0].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(commands,target,D3D12_RESOURCE_STATE_COPY_SOURCE,state);++s.snapshots;
}
void ColorHandoff::return_output(ID3D12GraphicsCommandList* commands) {
    auto& s=*impl_;s.check_owner();
    if(s.stage!=Impl::Stage::snapshot || identity(commands)!=identity(s.commands.Get()))throw std::logic_error("Handoff return requires same recording after snapshot");
    transition(commands,s.scratch[1].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(commands,s.target.Get(),s.target_state,D3D12_RESOURCE_STATE_COPY_DEST);
    copy_top(commands,s.target.Get(),s.scratch[1].Get());
    transition(commands,s.target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,s.target_state);
    transition(commands,s.scratch[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    s.stage=Impl::Stage::returned;++s.returns;
}
void ColorHandoff::preserve_target(ID3D12GraphicsCommandList* commands) {
    auto& s=*impl_;s.check_owner();
    if(s.stage!=Impl::Stage::snapshot || identity(commands)!=identity(s.commands.Get()))throw std::logic_error("Handoff preserve requires same recording after snapshot");
    s.stage=Impl::Stage::returned; // Lifetime still retained until the same fence.
}
void ColorHandoff::bind_completion(ID3D12Fence* fence,std::uint64_t future) {
    auto& s=*impl_;s.check_owner();
    if(s.stage!=Impl::Stage::returned || !fence || !future || future==UINT64_MAX)throw std::logic_error("Handoff completion contract invalid");
    s.check_device(fence);
    if(fence->GetCompletedValue()>=future)throw std::logic_error("Handoff requires a future fence value");
    s.fence=fence;s.future=future;s.stage=Impl::Stage::pending;
}
bool ColorHandoff::retire_if_complete() {
    auto& s=*impl_;s.check_owner();if(s.stage!=Impl::Stage::pending)throw std::logic_error("Handoff completion not bound");
    const auto value=s.fence->GetCompletedValue();if(value==UINT64_MAX)throw std::runtime_error("Handoff device removed");
    if(value<s.future)return false;
    hr(s.device->GetDeviceRemovedReason());s.target.Reset();s.commands.Reset();s.fence.Reset();s.stage=Impl::Stage::idle;return true;
}
void ColorHandoff::discard_recorded() {
    auto& s=*impl_;s.check_owner();if(s.stage!=Impl::Stage::pending)throw std::logic_error("No handoff recording to discard");
    s.target.Reset();s.commands.Reset();s.fence.Reset();s.future=0;s.stage=Impl::Stage::idle;
}
bool ColorHandoff::ready() const {impl_->check_owner();return impl_->stage==Impl::Stage::idle;}
std::uint64_t ColorHandoff::snapshot_count() const {impl_->check_owner();return impl_->snapshots;}
std::uint64_t ColorHandoff::return_count() const {impl_->check_owner();return impl_->returns;}
}
