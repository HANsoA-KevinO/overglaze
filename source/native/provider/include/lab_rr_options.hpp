// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-FileCopyrightText: 2023 NVIDIA CORPORATION
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <sl_core_api.h>
#include <atomic>
#include <mutex>
#include <string_view>

namespace lab::rr {
// Public DLSSDOptions v3 ABI, not a guessed private structure. Raw enum values
// deliberately have no preset/model-name interpretation.
// Layout adapted from NVIDIA Streamline v2.7.2 include/sl_dlss_d.h (MIT).
// Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
struct OptionsV3 : sl::BaseStructure {
    static constexpr sl::StructType type{0x0ad87504,0x774e,0x4bf3,{0x96,0x33,0xa4,0x4d,0x1f,0x7f,0x9c,0xb8}};
    OptionsV3():BaseStructure(type,3){}
    std::uint32_t mode=0,width=0,height=0;
    float sharpness=0,pre_exposure=0,exposure_scale=0;
    sl::Boolean hdr=sl::eInvalid,flip_x=sl::eInvalid,flip_y=sl::eInvalid;
    std::uint32_t normal_roughness=0;
    sl::float4x4 world_to_view{},view_to_world{};
    sl::Boolean alpha=sl::eInvalid;
    std::array<std::uint32_t,6> presets{};
};
static_assert(sizeof(OptionsV3)==0xe0);
static_assert(offsetof(OptionsV3,mode)==0x20 && offsetof(OptionsV3,pre_exposure)==0x30);
static_assert(offsetof(OptionsV3,hdr)==0x38 && offsetof(OptionsV3,presets)==0xc4);
using SetOptions=sl::Result(const sl::ViewportHandle&,const OptionsV3&);
// Public DLSSOptions (DLSS super resolution), adapted under the same MIT notice
// from NVIDIA Streamline include/sl_dlss.h at the pinned public revision
// fbe73ba0dc817da8db877c62038d415816209139, where it is version 3. Streamline
// only ever APPENDS members, so this prefix -- mode through colorBuffersHDR --
// is the same in every version and is all that is read; presets, auto-exposure
// and alpha upscaling are not. SR has no flip fields.
struct SrOptionsPrefix : sl::BaseStructure {
    static constexpr sl::StructType type{0x6ac826e4,0x4c61,0x4101,{0xa9,0x2d,0x63,0x8d,0x42,0x10,0x57,0xb8}};
    SrOptionsPrefix():BaseStructure(type,3){}
    std::uint32_t mode=0,width=0,height=0;
    float sharpness=0,pre_exposure=1,exposure_scale=1;
    sl::Boolean hdr=sl::eInvalid;
};
static_assert(offsetof(SrOptionsPrefix,mode)==0x20 && offsetof(SrOptionsPrefix,pre_exposure)==0x30 && offsetof(SrOptionsPrefix,hdr)==0x38);
inline constexpr std::size_t sr_prefix_bytes=offsetof(SrOptionsPrefix,hdr)+sizeof(sl::Boolean);
using SetSrOptions=sl::Result(const sl::ViewportHandle&,const SrOptionsPrefix&);
enum class Issue { none,unseen,invalid,version,extension,fault,failed,overlap,changed,stopped };
const char* name(Issue) noexcept;
struct Packet {
    // Which upscaler's setter produced it. An SR packet is decoded into the
    // same fields (flips false, alpha false) so one translation reads both.
    sl::Feature feature=sl::kFeatureDLSS_RR;
    std::uint64_t revision=0;
    unsigned viewport=UINT32_MAX,thread=0;
    Issue issue=Issue::unseen;
    OptionsV3 options{};
    bool valid() const noexcept {return issue==Issue::none;}
};
// relaxed (controller, self-configuring): RR options of version 3 OR LATER are
// read through their v3 prefix -- Streamline only appends members -- and a
// chained extension on the options is ignored, since only prefix fields are
// read. Strict (research): exactly v3, no chain.
Packet decode(const sl::ViewportHandle*,const OptionsV3*,bool relaxed=false) noexcept;
Packet decode_sr(const sl::ViewportHandle*,const void* options,bool relaxed=false) noexcept;
// Process-pinned lightweight hook. No files, GPU calls, borrowed pointers or
// automatic request retries. A successful setter is an accepted REQUEST only.
class Watch final {
    friend struct WatchTestAccess;
    mutable std::mutex mutex_;
    // A coherent value snapshot. Readers do not participate in the writer
    // admission mutex: a diagnostic query must not veto a valid RR invocation.
    // This large atomic is not promised to be lock-free.
    std::atomic<Packet> latest_{Packet{}};
    std::atomic<std::uint64_t> revision_{0},loss_{0};
    // Relaxed only: the latest accepted request PER VIEWPORT. A game that sets
    // options for two viewports (or from two threads) does not make each
    // one's value "changed" or "overlapping" for the other.
    static constexpr unsigned kViews=4;
    std::array<std::atomic<Packet>,kViews> views_{};
    std::atomic<unsigned> next_view_{0};
    bool relaxed_=false;
    void store_view(const Packet&) noexcept;
    Packet view(unsigned viewport) const noexcept;
    std::atomic<unsigned> active_{0};
    std::atomic<bool> stopped_{false};
    void* target_=nullptr;
    void* original_=nullptr;
    // One watch per setter: RR (slDLSSDSetOptions) or SR (slDLSSSetOptions).
    const bool super_resolution_=false;
    static std::atomic<Watch*> installed_,installed_sr_;
    static sl::Result hook(const sl::ViewportHandle&,const OptionsV3&);
    static sl::Result hook_sr(const sl::ViewportHandle&,const SrOptionsPrefix&);
    friend struct Invocation;
public:
    explicit Watch(bool super_resolution=false) noexcept:super_resolution_(super_resolution){}
    bool install(void*,json&); // worker; target identity belongs to verified host
    void relax_before_install() noexcept {if(!target_)relaxed_=true;}
    bool relaxed() const noexcept {return relaxed_;}
    Packet freeze() const noexcept;
    // The request in force for this viewport (relaxed), else freeze().
    Packet freeze(unsigned viewport) const noexcept;
    bool unchanged(const Packet&) const noexcept;
    void stop() noexcept {stopped_=true;++revision_;}
    json snapshot() const; // worker
    ~Watch()=default; // installed objects MUST live until process exit
};
json describe(const Packet&); // worker only; invalid packet has no numeric values
}
