// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_sl_bindings.hpp"
#include "lab_binding_boundary.hpp"
#include "lab_nr_live_api.hpp"
namespace lab {
namespace rr { struct Packet; }
// Where in a public Streamline call the adapter hands it to observe_call.
//   entering  before the original API runs (tags/constants not yet consumed)
//   returned  after the adapter has finished with the call, INCLUDING any
//             controlled insertion that ran inside it; result is known
//   aborted   the original API threw; nothing of the call can be trusted
enum class ObservedPoint : unsigned { entering, returned, aborted };
// The research half of the SL boundary, kept apart from SlAdmissionSink. The
// adapter holds it as a nullable pointer: with no observer it
// records nothing extra, which is exactly the controller's behaviour.
//
// None of these methods may schedule GPU work, take a resource lease or affect
// admission. They observe a call that the adapter has already decided about.
struct IResearchObserver {
    virtual bool wants_binding_boundaries()const noexcept{return false;}
    virtual void binding_boundary(const diagnostic::BindingBoundaryEvent&)noexcept{}
    // Optional read-only recording window. Does not grant rendering admission.
    virtual bool wants_access_observation()const noexcept{return false;}
    virtual void observe_entry(const slboundary::Resolution&,void*) noexcept {}
    virtual void observe_return(std::uint64_t,bool) noexcept {}
    // An RR call that translated into a complete NR frame, delivered before the
    // frame itself so a collector can publish its own per-call context first.
    virtual void admitted_frame_context(const slboundary::Resolution&,const rr::Packet&,const live::Frame&)noexcept{}
    // EVERY public boundary call -- token, constants, tags and the Evaluate of
    // any feature -- at entry, at return and on abort, outside every adapter
    // lock. The Call is borrowed metadata, valid only for the duration of this
    // method.
    //
    // Contract: like every method above it
    // may not schedule GPU work, take a resource lease or affect admission. ONE
    // exception, and only this one: at ObservedPoint::entering of an Evaluate,
    // an observer that currently holds an explicit research capture request may
    // take COM references on resources that ITS OWN expiry rule establishes as
    // tagged for exactly this call -- inline in the call, or a legacy tag set on
    // the Evaluate's own thread for its viewport since the previous upscaler
    // Evaluate and the previous Present (the pinned profile's rule). That entry
    // is the only moment the game still guarantees those resources: the original
    // Evaluate has not run. Every reference taken there must be released by the
    // matching returned/aborted point, or handed to an owner that keeps it until
    // the GPU work using it has retired. With no request it AddRefs nothing.
    virtual void observe_call(const slboundary::Call&,ObservedPoint) noexcept {}
protected:~IResearchObserver()=default;
};
}
