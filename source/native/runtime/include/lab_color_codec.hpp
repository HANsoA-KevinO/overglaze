// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_preconvert_reference.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>

namespace lab::nr {
// Lab-owned conversion, not an NVIDIA NR parameter/API. Only the native-sized
// RGBA16F/32F contract is implemented. Host establishes all resource states and
// allocation independence, preserves the original contents until completion,
// and restores its command-list bindings after this module records work.
struct ColorInputs {
    ID3D12Resource* original = nullptr;  // NON_PIXEL_SHADER_RESOURCE throughout
    ID3D12Resource* prepared = nullptr;  // initially UAV; returned shader-readable
    ID3D12Resource* neural = nullptr;    // NR writes UAV; restored UAV after finish
    ID3D12Resource* composite = nullptr;// initially UAV; returned shader-readable
    bool states_and_distinct_allocations_established = false;
};
class ColorCodec final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    ColorCodec(ID3D12Device* device, const ColorInputs& inputs);
    ~ColorCodec();
    ColorCodec(const ColorCodec&) = delete;
    ColorCodec& operator=(const ColorCodec&) = delete;
    // OFF is deliberately outside this API: do not call either pass on OFF.
    void prepare(ID3D12GraphicsCommandList*, const preconvert::Constants&);
    void composite(ID3D12GraphicsCommandList*);
    // Bind BEFORE submission. This must be the host-owned, GPU-only fence that
    // will be signaled AFTER submitting this exact list and its last consumers.
    // This module cannot infer queue ownership from an ID3D12Fence pointer.
    void bind_completion(ID3D12Fence*, std::uint64_t future_value);
    bool retire_if_complete(); // nonblocking; never CPU-waits on the render thread
    std::uint64_t prepare_count() const;
    std::uint64_t composite_count() const;
};
}
