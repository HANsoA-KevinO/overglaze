// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_sl_bindings.hpp"
#include "lab_rr_options.hpp"
#include "lab_rejected_call.hpp"
namespace lab {
// Where the SL boundary delivers an admitted RR call. Process-pinned optional
// controlled backend: the admission-only adapter stays useful without a
// renderer, and only live, matched callback scope can reach this.
//
// The research-only observation methods live in IResearchObserver instead, so
// the controller can implement admission without any research interface.
struct SlAdmissionSink {
    // A render-state checkpoint scope, separate from read-only diagnostics and
    // resource leases. Exit is guaranteed even when admission later fails.
    virtual void game_binding_enter(const slboundary::Resolution&,void*) noexcept {}
    virtual void game_binding_exit(std::uint64_t) noexcept {}
    virtual void enter(const slboundary::Resolution&,const rr::Packet&,void* command) noexcept=0;
    // Matched outer API return, NOT proof of downstream rebind/GPU/Present.
    virtual void boundary_returned(std::uint64_t call,bool success) noexcept=0;
    virtual void aborted(std::uint64_t call) noexcept=0;
    virtual void unavailable() noexcept=0;
    virtual void unavailable(const RejectedCall&) noexcept {unavailable();}
protected:~SlAdmissionSink()=default;
};
}
