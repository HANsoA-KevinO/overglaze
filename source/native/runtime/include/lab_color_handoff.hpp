// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <memory>
#include <cstdint>
#include "lab_serial_access.hpp"
#include "lab_device_identity.hpp"

namespace lab::nr {
// In-place return to a host-owned target. The two scratch resources must be
// independent allocations. Snapshot starts shader-readable; composite must be
// shader-readable when return_output is called (e.g. after ColorCodec).
struct HandoffInputs {
    ID3D12Resource* snapshot = nullptr;
    ID3D12Resource* composite = nullptr;
    bool states_and_distinct_allocations_established = false;
    // ABI23: the game's target may be LARGER than the scratch pair. Its top-left
    // region of the scratch's extent is the image (a letterboxed output); only
    // that region is copied in and written back, the rest is never touched.
    bool target_region_crop = false;
};
class ColorHandoff final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    ColorHandoff(ID3D12Device*, const HandoffInputs&,SerialCallGate* gate=nullptr,DeviceIdentity identity_policy={});
    ~ColorHandoff();
    ColorHandoff(const ColorHandoff&) = delete;
    ColorHandoff& operator=(const ColorHandoff&) = delete;
    // OFF never calls this module. Host has established target's exact state,
    // no split barrier/render pass, non-aliasing and exclusive ordered access.
    // Both calls use the same recording. No submit/wait/readback/allocation or
    // pipeline binding is performed here. The target's state is restored.
    void validate_snapshot(ID3D12GraphicsCommandList*, ID3D12Resource* target,
                  D3D12_RESOURCE_STATES target_state, std::uint64_t frame_token,
                  bool target_contract_established) const;
    void snapshot(ID3D12GraphicsCommandList*, ID3D12Resource* target,
                  D3D12_RESOURCE_STATES target_state, std::uint64_t frame_token,
                  bool target_contract_established);
    void return_output(ID3D12GraphicsCommandList*);
    // Diagnostic completion path: snapshot already restored the target state.
    // No COPY_DEST transition or write to the game target is recorded.
    void preserve_target(ID3D12GraphicsCommandList*);
    // Bind before submission; the private host GPU fence must be signaled only
    // AFTER this recording and all consumers that use retained resources.
    // A fence pointer cannot prove these host obligations or internal CUDA work.
    void bind_completion(ID3D12Fence*, std::uint64_t future_value);
    bool retire_if_complete(); // Poll only. False means keep all references.
    void discard_recorded(); // list Reset before submission: snapshot/return never ran
    bool ready() const;
    std::uint64_t snapshot_count() const;
    std::uint64_t return_count() const;
};
}
