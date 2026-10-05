// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_completion_timeline.hpp"
#include "lab_platform.hpp"
#include "lab_insertion_bindings.hpp"
#include <wrl/client.h>
namespace lab {
// One enrolled *live* RR recording, not a global event log. Enrollment comes
// from the matched successful outer RR return before the host rebinds state.
// No inferred Reset/creation history: the enrollment sequence is explicitly local.
class LiveSubmission final {
    struct Impl;Impl* impl_;
    explicit LiveSubmission(Impl* p):impl_(p){}
public:
    static LiveSubmission* install(ID3D12CommandQueue* native_seed,HMODULE verified_reshade);
    InsertionBindings* enable_binding_preservation(); // worker, before first enrollment
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> resolve(IUnknown*) const;
    // Probe one real submission without Signal or injected GPU work. Subsequent
    // enrollments may use different lists, pinned only through their last use.
    bool enroll(ID3D12GraphicsCommandList*,bool probe);
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> observed_queue() const;
    bool arm(CompletionTimeline&,CompletionTimeline::Ticket);
    bool release_completed();
    // The game Reset the enrolled list before executing it (proven in the
    // Reset hook): an ARMED NR recording is then discarded, never executed.
    // The owner retires the discarded ticket first, then releases the lease.
    bool release_discarded();
    const char* first_failure() const noexcept; // lock-free; for fault text
    // Times a completion Signal was retried because the owner held the
    // timeline lock. Nonzero means the contention that used to terminate NR
    // is happening and is being absorbed; it is not an error count.
    std::uint64_t signal_retries() const noexcept;
    // Only after the old recording and completion pointer have been retired.
    bool forget_queue_for_rebuild();
    // OFF-only candidate discarded before any execution; never NR recovery.
    bool retry_discarded_probe();
    void abandon() noexcept;
    void stop() noexcept;
    json snapshot() const;
    void uninstall_for_test(); // only after all fixture callers/GPU work quiesce
};
}
