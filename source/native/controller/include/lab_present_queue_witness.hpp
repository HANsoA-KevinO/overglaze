// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <array>
#include <functional>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
namespace lab {
// Retained diagnostic: which queue does a late-adopted swapchain present with?
// The late panel's BatchDraw route below needs only an exact named-buffer
// submission; it does not turn that submission queue into a Present queue.
//
// The panel has to submit on that queue, right before the original Present. A
// host that saw the swapchain created was handed the queue; a late-loaded one
// was not, and a D3D12 swapchain does not answer GetDevice(ID3D12CommandQueue)
// (E_NOINTERFACE: on RE9, and on a plain swapchain in the witness fixture).
// Guessing is not an option: drawing on the
// queue RR ran on ended in a GPU crash on Halo with Streamline frame
// generation loaded.
//
// The evidence used instead is the transition D3D12 itself requires: a back
// buffer must be in PRESENT when its present executes, so the game records a
// barrier into PRESENT for the CURRENT back buffer and executes that list before
// calling Present. The queue that executed it is the candidate. It is accepted
// only when, for a run of consecutive presents,
//   - every present's current back buffer had a fresh such transition,
//   - that transition was executed on the same DIRECT queue of this device,
//   - no second queue executed a transition of the same buffer in between, and
//   - no OTHER direct queue issued a potentially current-frame GPU Wait between
//     that submission and Present. A wait can be excluded only when its private,
//     single-producer monotonic fence was already complete at an observed point
//     on Q accepted BEFORE this transition began. Shared/CPU-written/rewound or
//     unknown fences never qualify. A signal after the transition followed by
//     a wait on P remains the cross-queue shape where work appended to Q races.
// A single miss restarts the run. Nothing is inferred from which thread calls
// what, or from the order queues were first seen.
//
// Observed once while writing the fixture (driver 617.14): a real
// flip swapchain's back buffer transitioned on a second DIRECT queue removed the
// device at the next Present (DXGI_ERROR_ACCESS_DENIED). If that holds generally,
// a game that renders at all transitions its back buffers on the present queue;
// the refusals above guard the case where it does not, and the fixture exercises
// them on ordinary textures standing in for back buffers.
//
// After the decision, every present still needs fresh unambiguous evidence.
// A foreign DIRECT wait without an old-completion exclusion denies this frame's permit,
// but does not by itself prove a changed queue or consume a restart. A changed
// transition queue or conflicting transitions do contradict the decision.
// No permit is returned again until the decision is uncontradicted.
//
// A contradiction is not the end of the evidence (RE9: switching
// DLSS frame generation on in game moved presents onto Streamline's queue, and
// a panel withdrawn for good could not be opened again). restart() drops the
// decision, the candidate run and every unconsumed transition, and the SAME
// rules then have to hold for a fresh run before anything is decided again. It
// is bounded -- at most `restarts` within `restart_window_ms` -- and refuses past
// that, so the caller stays withdrawn instead of flapping. rebind() is the same
// reset for a different followed chain, plus the back buffers are named again
// by its first present; the caller bounds how often that happens.
//
// Hooks: ID3D12GraphicsCommandList::ResourceBarrier, ID3D12GraphicsCommandList7::
// Barrier where the runtime has it, ID3D12CommandQueue::ExecuteCommandLists and
// ::Wait/Signal and ID3D12Fence::Signal, read from our discovery objects on the game's device. Process lifetime.
// Before the buffers are named no back-buffer barrier can qualify. Reset
// clears bounded recording metadata; no game resource is kept referenced.
class PresentQueueWitness final {
public:
    struct Policy {unsigned streak=60,restarts=3;std::uint64_t restart_window_ms=60000;};
    static PresentQueueWitness* install(ID3D12Device* device,Policy policy={});
    // The chain's back buffers as identities only: compared, never dereferenced
    // and never kept referenced (a held reference fails the game's ResizeBuffers).
    struct BackBuffers {std::array<const void*,8> identity{};unsigned count=0,current=0;bool named=false;};
    static BackBuffers name(IDXGISwapChain3* chain)noexcept;
    // At the top of the adopted chain's Present, before the original.
    // A permit for THIS present only. A retained decision is not permission to
    // draw a frame whose transition/wait evidence is missing or ambiguous.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> present(const BackBuffers& buffers)noexcept;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> present(IDXGISwapChain3* chain)noexcept{return present(name(chain));}
    // Submit the panel immediately after the original ExecuteCommandLists call for the
    // game's final back-buffer transition, under a same-queue CPU scope. The submit closure is synchronous,
    // accepts one closed panel list, and must not escape the callback. This route
    // does not infer a Present queue or waive any Present-permit refusal.
    using BatchSubmit=std::function<void(ID3D12CommandList*)>;
    using BatchDraw=std::function<void(ID3D12CommandQueue*,ID3D12Resource*,const BatchSubmit&)>;
    void set_batch_draw(BatchDraw);
    // Fresh current-buffer submission proof for batch-only binding. This says
    // which queue submitted that buffer, NOT which queue presents it.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> batch_queue(const BackBuffers&)noexcept;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> batch_queue()const;
    // The panel's own barriers/submissions must never become game evidence.
    struct OwnWork final {OwnWork()noexcept;~OwnWork();OwnWork(const OwnWork&)=delete;OwnWork& operator=(const OwnWork&)=delete;};
    // The proven queue, or null while the evidence is still being gathered.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> decided()const;
    std::uint64_t contradictions()const noexcept;
    // The current decision has been contradicted since it was made.
    bool contradicted()const noexcept;
    // A fresh decision under the same rules; false once the bound is spent.
    bool restart(std::uint64_t now_ms)noexcept;
    // The followed chain changed: restart, unbounded, and re-name the buffers.
    void rebind()noexcept;
    json status()const;
    // Test-only: MinHook hooks removed, the singleton cleared.
    void uninstall_for_test();
    struct Impl;
private:
    explicit PresentQueueWitness(Impl* impl):impl_(impl){}
    Impl* impl_;
};
namespace latebind {
// ResourceBarrier, ID3D12GraphicsCommandList7::Barrier, ExecuteCommandLists,
// queue Wait/Signal, CPU fence Signal. Defined in late_slots.cpp (CINTERFACE).
std::array<std::size_t,7> witness_slots();
}
}
