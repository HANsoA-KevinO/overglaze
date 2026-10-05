// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_depth_projection.hpp"
#include "lab_serial_access.hpp"
#include <d3d12.h>
#include <memory>
namespace lab::nr {
// What the codec does to the game's depth before NR sees it.
//  * view_z_to_hardware: linear view-space depth -> hardware depth through the
//    projection (the original codec, 007's linear depth).
//  * depth_stencil_plane0: a TWO-PLANE depth/stencil resource
//    (R32G8X24_TYPELESS, RE Engine) -> its depth plane copied unchanged into a
//    single-plane R32_FLOAT. NR is never handed the two-plane resource: doing
//    exactly that crashed the display driver, because NR builds
//    its own single-plane views and transitions on whatever it is given.
// hardware_mip0_copy: a single-plane hardware depth that carries a mip chain
// (a Hi-Z pyramid kept in the same resource, A Plague Tale: Resonance) is
// copied, level 0 only, into this codec's single-mip R32_FLOAT, because NR is
// handed single-mip guides.
enum class DepthCodecMode { view_z_to_hardware, depth_stencil_plane0, hardware_mip0_copy };
// Single in-flight slot. Owner calls acknowledge_completion only after the
// same private GPU completion used to retire NR; no readback/files/queue here.
class DepthCodec final {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    // region_crop: the source may be LARGER than width x height; its top-left
    // width x height region is the image (a render subrect, ABI23). Every mode's
    // shader already reads the source at the target's own coordinates, so the
    // crop is that region and nothing outside it is read. Without it the source
    // must match exactly, as before.
    DepthCodec(ID3D12Device*,unsigned width,unsigned height,SerialCallGate* gate=nullptr,
               DepthCodecMode mode=DepthCodecMode::view_z_to_hardware,bool region_crop=false);
    DepthCodecMode mode() const;
    ~DepthCodec();
    ID3D12Resource* output() const;
    void validate(ID3D12GraphicsCommandList*,ID3D12Resource*,const DepthProjection&) const;
    // Input already NPSR, output starts/ends NPSR. Caller restores its bindings.
    // In depth_stencil_plane0 mode "input" means SUBRESOURCE 0, the depth
    // plane; the stencil plane is neither read nor transitioned. The projection
    // is ignored in that mode.
    void record(ID3D12GraphicsCommandList*,ID3D12Resource*,const DepthProjection&);
    void acknowledge_completion();
    void discard_recording(); // list Reset before submission: conversion never ran
};
}
