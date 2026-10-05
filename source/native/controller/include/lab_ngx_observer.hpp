// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The NGX loader (nvngx.dll) exposes the Core ABI. lab_nr_session.hpp drives the
// identically NAMED snippet ABI of the verified model DLL, whose Init and
// CreateFeature prototypes differ. lab_nr_parameters.hpp defines
// NGX_SNIPPET_BUILD inside a HEADER, so a stray include order could silently
// hand this translation unit the wrong prototypes and corrupt the game's own
// calls. This guard is load-bearing, and the NGX types stay behind the pimpl so
// that including this header cannot drag either ABI into the host.
#ifdef NGX_SNIPPET_BUILD
#error The NGX observer forwards the loader's Core ABI, not the verified snippet ABI.
#endif
#include "lab_platform.hpp"
#include <memory>

namespace lab::live { struct IFrameReceiver; }
namespace lab {
// Observation-only surface for the routes that drive DLSS and ray
// reconstruction through the NGX loader directly instead of through Streamline
// (adapter package routes "ngx-rr" and "ngx-sr"). Without a receiver this
// surface admits nothing; it exists to produce the facts admission is designed
// from, the same way the Streamline adapter's observation preceded its
// admission.
//
// The contract, which the adapter package's manifest also states to the user:
//   * no frame is admitted and no GPU work of ours is produced;
//   * no resource, command list, descriptor or NGX feature of ours is created;
//   * no NGX entry is ever called except to forward the game's own call, with
//     its own arguments, exactly once.
// The observer reads parameters the game already populated. It never writes one.
class NgxObserver final {
public:
    NgxObserver();
    ~NgxObserver();
    NgxObserver(const NgxObserver&) = delete;
    NgxObserver& operator=(const NgxObserver&) = delete;

    // Adopts the loader only if the GAME has already mapped it, and only when
    // the mapped path is the one the driver's registry entry names. Returns
    // false while the loader is absent so the caller can poll; a genuine
    // refusal (identity mismatch, missing export, hook conflict) is terminal
    // and recorded. Never loads a library.
    bool attach() noexcept;
    bool attached() const noexcept;
    // Terminal refusal reason, empty while attaching is merely pending.
    bool refused() const noexcept;
    // ADMISSION (route ngx-rr only). With no receiver this surface
    // stays exactly the observation-only one described above. With one, each
    // ray-reconstruction Evaluate is offered to it on the Streamline route's
    // sequence: checkpoint before the game's call, then -- still inside the
    // detour, after a successful return -- one translated frame or a named
    // skip. Setting null returns to observation; the host does so before it
    // stops the receiver.
    void set_receiver(live::IFrameReceiver*) noexcept;
    // Why the last CANDIDATE call did not become a frame; null once one did.
    // Only SR and RR are candidates (lab_ngx_frame.hpp, candidacy): frame
    // generation and every other NGX feature are forwarded untouched, never
    // offered, and never change this.
    const char* blocked() const noexcept;
    json report() const;
    // The admission counters alone -- offered/admitted/skipped/aborted and the
    // non-candidate evaluates that were never offered, by feature id. Small on
    // purpose: the host publishes it as its own status key, which survives the
    // size clipping that drops the largest key first (a Hellblade 2 fault
    // record once lost ngx_observation that way).
    json admission() const;

    // TEST SEAM. Hooks these three entries -- the D3D12 CreateFeature,
    // EvaluateFeature and ReleaseFeature, in that order -- with the same
    // detours attach() installs, skipping loader discovery and identity. It is
    // how the admission tests drive the REAL detours against a fake loader in
    // their own executable; attach() ends in the same install. No product path
    // calls it. Same one-active-observer rule; false if already attached.
    bool attach_entries_for_test(void* create, void* evaluate, void* release) noexcept;

    // Named here only so the detours in the implementation can reach it. The
    // definition stays in the .cpp, so the NGX ABI still cannot escape.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};
}
