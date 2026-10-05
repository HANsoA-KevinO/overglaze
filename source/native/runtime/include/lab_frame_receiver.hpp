// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_nr_live_api.hpp"
namespace lab::live {
// Where a translated RR frame is delivered. No Streamline, NGX or game type
// appears here: the provider translates the SL boundary into live::Frame, and
// everything downstream (the bridge client in either track) sees only this.
//
// It exists so the controller host and the research host share one
// translation. Both implement it through LiveRuntimeClient.
struct IFrameReceiver {
    virtual void frame(const Frame&) noexcept=0;
    // Matched outer API return, NOT proof of downstream rebind/GPU/Present.
    virtual void boundary_returned(std::uint64_t call,bool success) noexcept=0;
    virtual void aborted(std::uint64_t call) noexcept=0;
    // An RR call that our own checks refused before anything was recorded.
    virtual void rejected(const RejectedCall&) noexcept=0;
    // Render-state checkpoint scope around the game's own call.
    virtual void game_binding_enter(std::uint64_t call,std::uint64_t frame_index,void* command) noexcept {}
    virtual void game_binding_exit(std::uint64_t call) noexcept {}
protected:~IFrameReceiver()=default;
};
}
