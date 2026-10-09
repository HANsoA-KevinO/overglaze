// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_sl_bindings.hpp"
#include "lab_platform.hpp"
#include <d3d12.h>
#include <wrl/client.h>

namespace lab::slboundary {
struct ResourceFacts {
    bool inspected=false,same_device=false,matched_return=false;
    unsigned command_type=UINT32_MAX;
    std::array<D3D12_RESOURCE_DESC,3> desc{};
    const char* reason="not-inspected"; // Only static literals, no borrowed data.
    // A shape or extent refusal names its role, and every role that could be
    // described is kept, so one launch shows what the game actually hands over
    // (in A Plague Tale: Resonance a bare "unsupported-resource-shape" could not
    // say which role, nor whether mips, layers or samples).
    int failed_role=-1;
    std::array<bool,3> described{};
};
// Holds real COM references acquired at Evaluate ENTRY, while tag lifetimes
// still cover the use. No content/state/exclusive-access/GPU completion proof.
// Move to a GPU retirement owner before recording any injected work; destroying
// this CPU-only lease at API return is NOT a GPU resource retirement strategy.
class ResourceLease final {
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,3> resources_;
    // The game's exposure texture, when the binding carries one (Live ABI24).
    // Optional: taking it never decides whether the lease itself holds.
    Microsoft::WRL::ComPtr<ID3D12Resource> exposure_;void* exposure_native_=nullptr;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_;
    Binding identity_{};
    ResourceFacts facts_;
public:
    ResourceLease()=default;
    ResourceLease(const ResourceLease&)=delete;
    ResourceLease& operator=(const ResourceLease&)=delete;
    ResourceLease(ResourceLease&&)=default;
    ResourceLease& operator=(ResourceLease&&)=default;
    // device_command: the list whose device the resources must share. Default
    // the Evaluate's own buffer; for a Streamline proxy list, its native list
    // (the proxy reports Streamline's proxy device, never the resources' one).
    static ResourceLease acquire(const Resolution&,bool allow_mip_chains=false,IUnknown* device_command=nullptr);
    bool matches(const Resolution&) const noexcept;
    bool held() const noexcept {return command_!=nullptr;}
    std::uint64_t call() const noexcept{return identity_.call;}
    ResourceFacts facts() const noexcept{return facts_;}
    ID3D12Resource* resource(unsigned i) const noexcept{return i<3?resources_[i].Get():nullptr;}
    // Whether this lease took a reference to exactly that exposure texture.
    bool holds_exposure(const void* native) const noexcept{return native&&exposure_&&exposure_native_==native;}
    // Uncertain submitted work: retain COM references until process exit,
    // rather than destroying potentially in-flight resources on an error.
    void abandon() noexcept {for(auto& r:resources_)(void)r.Detach();(void)exposure_.Detach();(void)command_.Detach();}
};
json describe(const ResourceFacts&);
}
