// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_serial_access.hpp"
#include <d3d12.h>
#include <cstdint>
#include <memory>
namespace lab::nr {
// One bounded GPU reading of the working RGB the host is about to hand to NR:
// mean log2 luminance over a fixed subsampled grid, excluding non-finite or
// negative pixels. Group means are averaged with equal weight (edge groups
// count the same as full groups). It measures the frame; it does not decide
// the exposure and never writes any game or NR resource.
struct ExposureReading {
    bool valid=false;
    float mean_log2_luminance=0;
    unsigned samples=0,groups=0;
};
class ExposureMeter final {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    ExposureMeter(ID3D12Device*,unsigned width,unsigned height,SerialCallGate* gate=nullptr);
    ~ExposureMeter();
    ExposureMeter(const ExposureMeter&)=delete;
    ExposureMeter& operator=(const ExposureMeter&)=delete;
    bool pending() const;
    unsigned step() const;
    // Source must be a 2D RGBA16F/RGBA32F texture of the fixed extent, already
    // in NON_PIXEL_SHADER_RESOURCE; it is left in that state. One slot only.
    void validate(ID3D12GraphicsCommandList*,ID3D12Resource* source) const;
    void record(ID3D12GraphicsCommandList*,ID3D12Resource* source);
    // Owner calls after the same private GPU completion used to retire NR.
    ExposureReading acknowledge_completion();
    void discard_recording(); // list Reset before submission: no reading exists
    static constexpr std::uint64_t footprint_bytes() noexcept {return 3*16;}
};
}
