// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <memory>
#include "lab_serial_access.hpp"
#include "lab_device_identity.hpp"

namespace lab::nr {
// Self-authored pre/post ratio colour wrapper around NR: prepare encodes the
// (optionally matrixed, exposed, tone-compressed) working RGB as the sRGB NR
// input; composite reapplies the NR change to the working RGB as a bounded
// per-channel ratio and undoes matrix and exposure.
// NOT an official SDK, a discovered game input contract, or an NR implementation.
struct PrePostColorConstants {
    std::array<std::array<float,4>,3> pre_matrix{};
    std::array<std::array<float,4>,3> post_matrix{};
    float exposure=0, sigma=0, gamma=0;
    std::uint32_t tonemap_operator=0;
    std::uint32_t width=0,height=0,pre_post=0,reserved=0;
};
static_assert(sizeof(PrePostColorConstants)==128);
// All six allocations must be distinct and remain valid through last consumers.
// Finite nonnegative working RGB must be established by the host, not inferred
// from an FP texture format. Matrices/exposure are supplied explicitly per game.
struct PrePostColorInputs {
    ID3D12Resource* original=nullptr;     // NPSR throughout
    ID3D12Resource* prepared=nullptr;     // initially UAV -> NPSR (encoded NR input)
    ID3D12Resource* neural=nullptr;       // UAV -> temporarily NPSR -> UAV
    ID3D12Resource* composite=nullptr;    // initially UAV -> NPSR
    ID3D12Resource* working_hdr=nullptr;  // initially UAV -> NPSR (H, not raw X)
    ID3D12Resource* sdr_linear=nullptr;   // initially UAV -> NPSR (S before clamp)
    bool states_and_distinct_allocations_established=false;
    bool finite_nonnegative_working_rgb_established=false;
    // Explicit LAB extension, NOT part of the official shader equivalence claim.
    // Unknown/negative/nonfinite RGB is withheld from NR and passed through at
    // composite. This does not establish gamut, exposure, or a linear encoding.
    bool guard_invalid_rgb=false;
};
class PrePostColorCodec final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    PrePostColorCodec(ID3D12Device*,const PrePostColorInputs&,SerialCallGate* gate=nullptr,DeviceIdentity identity_policy={});
    ~PrePostColorCodec();
    PrePostColorCodec(const PrePostColorCodec&)=delete;
    PrePostColorCodec& operator=(const PrePostColorCodec&)=delete;
    // OFF: call neither pass. Binding restoration belongs to the host.
    void validate_prepare(ID3D12GraphicsCommandList*,const PrePostColorConstants&) const;
    void prepare(ID3D12GraphicsCommandList*,const PrePostColorConstants&);
    void composite(ID3D12GraphicsCommandList*);
    // Explicit diagnostic: retire Prepare without reading neural/compositing.
    // No GPU commands here. Still requires the usual future completion fence.
    void finish_prepare_only(ID3D12GraphicsCommandList*);
    // Host-owned GPU-only fence, signaled AFTER the same list and last consumers.
    void bind_completion(ID3D12Fence*,std::uint64_t future_value);
    bool retire_if_complete();
    void discard_recorded(); // list Reset before submission: nothing of this frame ran
    std::uint64_t prepare_count() const;
    std::uint64_t composite_count() const;
    // Diagnostic: composite writes the untouched original on the left half.
    // Recorded into the next prepare's constants; never part of the official recipe.
    void set_compare_split(bool);
    bool compare_split() const;
    // Lab extension, edit extrapolation (Live ABI25): composite reads the NR
    // output as N' = saturate(C + factor * (N - C)) per channel, with C the
    // prepared (encoded) NR input, then transfers it as usual. 1 is the plain
    // composite: the shader branch is not taken. Finite 1..4 only; applied from
    // the next prepare, changed only between frames.
    void set_extrapolation(float factor);
    float extrapolation() const;
};
}
