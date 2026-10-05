// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// Copy-at-tag-time for legacy Streamline tags declared eOnlyValidNow
// (Cyberpunk 2077 with DLSS-G on declares them: without a copy every RR
// Evaluate is refused "global-only-valid-now-expired" and NR never starts).
//
// Streamline's contract for eOnlyValidNow: the resource is valid only at the
// slSetTag call, so Streamline copies it THERE, on the command buffer the game
// passed to slSetTag, and its Evaluate reads the copy. NR is inserted after the
// Evaluate, when the game's resource may already hold something else. This
// class does what Streamline does, for NR's two INPUT roles only (depth,
// motion): at the tag call's return, on that same command list, the game's
// resource is copied from its declared state into a Lab-owned texture, and the
// tag handed to the binding layer names the copy. The output colour is never
// copied: NR writes its result back into it in place.
//
// Rules:
//   * nothing happens unless the host worker said frames are wanted (NR ON,
//     the runtime waiting for a frame, or an armed research capture): no copy,
//     no allocation, no Call copy -- one scan of the call's tags;
//   * the render thread never allocates. A missing copy texture is requested
//     from the worker (service()) and the tag is refused by name meanwhile;
//   * each tag is copied or refused on its own, by name (CopyOutcome), and the
//     refusal travels with the tag to the Evaluate that would have used it;
//   * per role and call, only the tag the binding layer keeps is copied (the
//     last of the role; self-configuring, a hardware depth over a linear one):
//     the other is "superseded", never copied;
//   * one pool of textures per semantic -- hardware depth, motion, linear depth
//     -- so a game tagging both depth semantics never makes one pool flip
//     shapes. A pool changes shape at most once per kHoldMs; a tag of another
//     shape meanwhile is "copy-texture-awaiting-reshape";
//   * subresource 0 only, from the declared state to COPY_SOURCE and back, into
//     a single-mip texture of the same format left in NON_PIXEL_SHADER_RESOURCE.
//     Unknown state, MSAA, arrays, two-plane depth/stencil, block-compressed,
//     planar or unknown formats, simultaneous access, a non-default layout, no
//     command buffer or one that is not a DIRECT D3D12 list on the resource's
//     device: refused, never guessed;
//   * a copy texture is reused only when no Lab holder references it (the
//     binding lease, the runtime's in-flight frame, a research capture: each
//     holds a COM reference until its GPU work retired), it is not the newest
//     copy of its role, and it is not being recorded; up to three per pool;
//   * at the Evaluate the adapter verifies the copy is still the one taken for
//     that tag call (verify()); otherwise the call is refused by name;
//   * textures leave service only through a hold of at least 16 Presents and
//     2 s and once no Lab holder references them: a copy recorded on a game list
//     that has not executed yet must never lose its destination. A full hold
//     queue keeps a texture in its slot (counted), never abandons or frees it, and
//     no texture is allocated into a slot that is still occupied;
//   * backoff: kWastedStreak consecutive target Evaluates
//     that bound copies but were refused for a reason no copy can fix (the
//     output colour itself OnlyValidNow, another thread, a stale snapshot...) stop
//     the copies -- "not-copied-backoff-after-refused-evaluates" -- except for
//     one probe window per kProbeMs, which spans exactly one Evaluate interval.
//     The first admitted Evaluate ends the backoff; so does NR going unwanted.
//     A waiting runtime that can never be admitted then costs the game one set
//     of copies per second, not two per frame.
// What it relies on and cannot prove: that the game executes the tag's list
// before the Evaluate's list (Streamline's own copy relies on the same), and
// that a list holding a copy is not executed after a later copy into the same
// texture recorded at least two tag rounds later.
#include "lab_sl_bindings.hpp"
#include "lab_platform.hpp"
#include <sl_core_api.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <optional>

namespace lab::slboundary {
class VolatileCopies final {
public:
    static constexpr unsigned kSlots=3;                 // per pool
    static constexpr unsigned kHoldPresents=16,kHoldMs=2000,kRetired=32;
    static constexpr unsigned kWastedStreak=64,kProbeMs=1000;
    static constexpr D3D12_RESOURCE_STATES kRestState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // Pools: [1] hardware depth (or the pinned profile's one depth semantic),
    // [2] motion, [3] linear depth. [0] unused (the output is never copied).
    static constexpr unsigned kPools=4;
    static unsigned pool_of(unsigned role,sl::BufferType type) noexcept {return role==2?2u:type==sl::kBufferTypeLinearDepth?3u:1u;}
    VolatileCopies()=default;
    VolatileCopies(const VolatileCopies&)=delete;
    VolatileCopies& operator=(const VolatileCopies&)=delete;
    ~VolatileCopies(); // process-pinned owner; textures of unknown completion are kept
    // Render thread, at a tag call's return (the original slSetTag has run).
    // True: `out` holds a copy of `c` whose eOnlyValidNow role tags name either
    // a Lab copy (lab_copy) or why not (copy_refusal); bind `out` instead of c.
    bool tag_returned(const Call& c,const Bindings&,PFun_slGetNativeInterface* native,std::optional<Call>& out) noexcept;
    // Adapter, at a target Evaluate's entry: the frozen tag of `role` that names
    // a Lab copy. none: still that very copy; otherwise the refusal. `list` is
    // the Evaluate's native list (identity only, for the same-list count).
    CopyOutcome verify(unsigned role,const Tag& tag,std::uint64_t tag_call,const void* list) noexcept;
    // Adapter, at every target Evaluate's return: the binding it resolved and
    // whether the frame was handed to the runtime. Drives the backoff.
    void evaluated(const Resolution& r,bool admitted) noexcept;
    // Host worker, every poll: whether frames are wanted now. Allocates what the
    // render thread asked for, retires what is no longer wanted. Never throws.
    void service(bool wanted,std::uint64_t now_ms) noexcept;
    void present() noexcept {++presents_;}
    void stop() noexcept {wanted_=false;stopped_=true;}
    bool active() const noexcept {return active_.load();}
    bool wanted() const noexcept {return wanted_.load();}
    bool backing_off() const noexcept {return backoff_.load();}
    json describe() const; // worker only
private:
    using Texture=Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Slot {Texture texture;D3D12_RESOURCE_DESC desc{};std::uint64_t serial=0,tag_call=0;const void* list=nullptr;bool claimed=false;};
    // shape: what the pool serves (shaped); changed: when it last took a shape.
    struct Pool {std::array<Slot,kSlots> slots{};std::uint64_t latest=0,changed=0;D3D12_RESOURCE_DESC needed{},shape{};unsigned want=0;bool need=false,shaped=false;};
    struct Retired {Texture texture;std::uint64_t present=0,tick=0;};
    static constexpr unsigned kProbeNone=0,kProbeRequested=1,kProbeOpen=2;
    std::array<Pool,kPools> pools_{};
    std::array<Retired,kRetired> retired_{};
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    mutable std::mutex mutex_;
    std::atomic<bool> wanted_{false},active_{false},stopped_{false},backoff_{false};
    std::atomic<unsigned> probe_{kProbeNone},wasted_streak_{0};
    std::atomic<std::uint64_t> presents_{0},serial_{0};
    std::uint64_t unwanted_since_=0,last_probe_=0; // worker only
    std::array<std::atomic<std::uint64_t>,static_cast<unsigned>(CopyOutcome::count)> outcomes_{};
    std::array<std::atomic<std::uint64_t>,kPools> copied_{};
    std::atomic<std::uint64_t> same_list_{0},other_list_{0},allocations_{0},allocation_failures_{0},releases_{0},retire_deferred_{0},allocated_bytes_{0};
    std::atomic<std::uint64_t> reshape_holds_{0},reshapes_{0},wasted_{0},backoffs_{0},probes_{0};
    std::unique_lock<std::mutex> lock() const noexcept;
    static bool idle(const Slot&) noexcept;
    CopyOutcome copy(const Call&,Tag&,unsigned role,ID3D12GraphicsCommandList*) noexcept;
    // Under mutex_. Moves `t` into the hold queue; false (t untouched) when full.
    bool retire(Texture& t,std::uint64_t now) noexcept;
};
}
