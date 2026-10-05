// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_serial_access.hpp"
#include <d3d12.h>
#include <memory>
namespace lab::nr {
// Motion vectors onto the guide grid (ABI23; first needed by Hellblade 2).
//
// NR takes depth and motion on ONE grid. A game may hand its upscaler motion at
// display resolution (NGX created without MVLowRes: Hellblade 2 gives 5120x2142
// motion beside 2970x1243 depth), or a motion resource larger than its image (a
// render subrect). This codec writes this codec's own RG16F guide of
// width x height from the top-left region_width x region_height of the source,
// point-sampling the source pixel under each guide pixel's centre. Values are
// copied unchanged: they stay in pixels of the source, and the caller rescales
// NR's motion scale by width/region_width and height/region_height. A region
// equal to the guide extent is a plain crop.
//
// Same contract as DepthCodec: one in-flight slot; the source is already
// NON_PIXEL_SHADER_RESOURCE; the output rests in NON_PIXEL_SHADER_RESOURCE;
// acknowledge only after the same private GPU completion that retires NR.
class MotionCodec final {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    MotionCodec(ID3D12Device*,unsigned width,unsigned height,SerialCallGate* gate=nullptr);
    ~MotionCodec();
    ID3D12Resource* output() const;
    void validate(ID3D12GraphicsCommandList*,ID3D12Resource* source,unsigned region_width,unsigned region_height) const;
    void record(ID3D12GraphicsCommandList*,ID3D12Resource* source,unsigned region_width,unsigned region_height);
    void acknowledge_completion();
    void discard_recording(); // list Reset before submission: resampling never ran
};
}
