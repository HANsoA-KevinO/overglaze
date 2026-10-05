// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <d3d12.h>
#include <functional>
#include "lab_binding_boundary.hpp"

namespace lab {
// Source-integrated binding block: compute AND graphics root arguments,
// PSO/RT PSO, shared heaps, fixed/dynamic graphics bindings and predication.
// Uses the source-derived host/binding_state.hpp, not a ReShade runtime/add-on.
// Not resource barriers/history or a full command-stream replay; unknown
// work-graph/meta/protected state and active render passes refuse insertion.
// Tracks at most 256 concurrent recordings on the seed device, from observed
// CreateCommandList/Reset through Close/destruction. Recording tracking is weak;
// the explicit game-call checkpoint pins one list only until its matching return.
// Bundles, unknown indirect signatures and active render passes refuse insertion. No pixels,
// shader extraction, resource catalogue, disk writes or per-frame event log.
class InsertionBindings final {
    struct Impl;Impl* impl_;
    explicit InsertionBindings(Impl* p):impl_(p){}
public:
    struct HeapOrderStats {std::uint64_t permutations=0,affected_insertions=0;};
    HeapOrderStats heap_order_stats() const noexcept;
    diagnostic::BindingSnapshot boundary_snapshot(ID3D12GraphicsCommandList*) const noexcept;
    static InsertionBindings* install(ID3D12GraphicsCommandList* method,ID3D12GraphicsCommandList* reset_method);
    bool watch(ID3D12GraphicsCommandList*); // validate; never invent prior recording state
    std::string watch_error() const; // bounded reason; not a merged capacity/ABI guess
    json inspect(ID3D12GraphicsCommandList*) const; // explicit bounded diagnostics only
    // Forwarded by LiveSubmission's existing hooks; no duplicate Reset detour.
    void reset(ID3D12GraphicsCommandList*,HRESULT,ID3D12PipelineState*) noexcept;
    void close(ID3D12GraphicsCommandList*) noexcept;
    bool ready(ID3D12GraphicsCommandList*) const noexcept;
    // Null means ready. Static reason strings, no allocation, disk I/O or GPU work.
    const char* blocker(ID3D12GraphicsCommandList*) const noexcept;
    std::string readiness_error(ID3D12GraphicsCommandList*) const;
    void preserving(ID3D12GraphicsCommandList*,const std::function<void()>&);
    // Explicit foreign-call envelope. Snapshot GAME state before SL changes it;
    // OFF only records/releases CPU metadata and never restores GPU bindings.
    // One serialized call, exact list/recording/thread; no fallback to post-SL state.
    void begin_game_call(std::uint64_t call,std::uint64_t frame,ID3D12GraphicsCommandList*);
    void end_game_call(std::uint64_t call) noexcept;
    const char* game_call_blocker(std::uint64_t call,std::uint64_t frame,ID3D12GraphicsCommandList*) const noexcept;
    void preserving_game_call(std::uint64_t call,std::uint64_t frame,ID3D12GraphicsCommandList*,const std::function<void()>&);
    json status() const;
    // Late attach (seen with RE9): an ExecuteIndirect whose command signature
    // carries no layout -- it was created before any creation hook of ours was
    // installed -- cannot be modelled exactly. Strict (default, the research
    // host and every early-loaded path): that recording is refused
    // ("indirect-signature-unobserved"). Tolerant (the controller host on a
    // late-loaded game only): the signature is assumed to have left every
    // binding untouched and each such call is counted. The only divergence
    // possible is that a binding the signature did touch -- which D3D12 resets
    // to zero/null and the game has to set again before any use -- is restored
    // to its pre-indirect value instead.
    void tolerate_unknown_indirect(bool) noexcept;
    struct UnknownIndirect {bool tolerated=false;std::uint64_t calls=0;};
    UnknownIndirect unknown_indirect() const noexcept;
    void stop() noexcept;
    void uninstall_for_test(); // only after callers and GPU quiesce
};
}
