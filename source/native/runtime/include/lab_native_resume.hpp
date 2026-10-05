// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_compute_bindings.hpp"
#include "lab_completion_timeline.hpp"
#include "lab_platform.hpp"
#include <utility>

namespace lab::resume {
// Process-lifetime, opt-in hook context, ONE native list and queue. List/queue
// are pinned; host owns all other GPU objects/descriptors until completion.
// Root layouts must come from verified creation bytes/host creation contract.
// No proxy offsets, logs, readbacks, dynamic list discovery or graphics claims.
class Context final {
    struct Impl;Impl* impl_;
    explicit Context(Impl* p):impl_(p){}
    static Context* install_impl(ID3D12GraphicsCommandList*,ID3D12CommandQueue*,bool completion_only);
    ~Context()=default; // Intentionally process-pinned, not a hot-unload object.
    struct Snapshot {ComputeBindings bindings;std::uint64_t generation=0;};
    Snapshot begin();void finish(const Snapshot&);void abort() noexcept;
public:
    Context(const Context&)=delete;Context& operator=(const Context&)=delete;
    // Once per process. Failure retains any enabled trampolines and context.
    // Explicit host contract, NOT a user toggle: this recording has no graphics,
    // render-pass or extension state requiring restoration. Default is refusal.
    static Context* install(ID3D12GraphicsCommandList*,ID3D12CommandQueue*,bool owned_compute_only_host=false);
    // Observe only Reset/Close/Execute. No setter hooks, saved bindings or
    // graphics restrictions/claims. Caller must supply restoration separately.
    static Context* install_completion_only(ID3D12GraphicsCommandList*,ID3D12CommandQueue*);
    bool completion_only() const noexcept;
    bool recording_ready() const;
    void register_root(ID3D12RootSignature*,ComputeBindings::Layout);
    std::uint64_t generation() const;
    bool ready() const;
    template<class Work> auto preserving(Work&& work){
        const auto saved=begin();
        try {auto result=std::forward<Work>(work)();finish(saved);return result;}
        catch(...){abort();throw;}
    }
    // Before Close: every scratch/borrowed NR last-use must be on this recording.
    // Explicit host ownership contract; NOT discovered via resource heuristics.
    void arm_last_use(CompletionTimeline&,CompletionTimeline::Ticket,std::uint64_t generation);
    // Owner calls after coordinator retirement. No resource lifetime shortcuts.
    void release_completed();
    void stop() noexcept;
    json status() const; // Worker/test only, never render callback.
};
}
