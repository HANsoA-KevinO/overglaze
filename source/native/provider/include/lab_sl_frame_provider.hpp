// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_sl_admission_sink.hpp"
#include "lab_research_observer.hpp"
#include "lab_frame_receiver.hpp"
#include "lab_platform.hpp"
#include <atomic>
#include <mutex>
namespace lab {
// The per-game facts the translation needs. They come from the validated
// installation, never from an executable-name comparison at this layer.
struct FrameProfile {
    std::uint32_t viewport=0;
    bool linear_depth=false;
    // SELF-CONFIGURATION (controller host). Goal: a game nobody adapted by hand
    // works once its path is added. The two facts above are the ones a
    // Streamline package would otherwise have to carry -- Alan Wake 2, for
    // example, uses viewport 1 and linear depth, and neither can be guessed. The
    // binding layer selects both from the game's calls (lab_sl_bindings.hpp);
    // with this set the translation takes them from the resolved call and:
    //   * the guide layout follows the depth resource's FORMAT rather than its
    //     kind (typed R32F/R16F -> typed, R32/D32S8 typeless -> legacy), so a game
    //     storing hardware depth as R32_FLOAT is not refused for it;
    //   * every format the runtime would refuse later is refused HERE, by role
    //     and by name, as a skip that keeps the ON gate -- not as the runtime's
    //     terminal format fault after the Feature is built;
    //   * the call may be RR or, where the binding layer targets it, DLSS
    //     super resolution: both put their upscaled output in the output-colour
    //     role and their render-resolution guides beside it, and both packets
    //     are decoded into the same option fields;
    //   * HDR is required and flips are refused, but any finite positive
    //     pre-exposure/exposure scale is accepted: brightness is the auto meter's
    //     job, and "exactly 1" was a per-game fact in disguise.
    // Off (the research host): nothing changes. The facts above are pinned and
    // every mismatch is refused exactly as before.
    bool self_configure=false;
};
// Translate one admitted RR call into a complete live::Frame.
//
// Returns nullptr on success; otherwise the reason the call cannot become a
// frame, as a static string suitable for the rejection record and the panel.
// Pure: it schedules nothing, takes no lease and touches no global state, so
// the WARP fixture can drive every refusal path.
const char* translate_rr_frame(const slboundary::Resolution&,const rr::Packet&,void* command,
                               const FrameProfile&,live::Frame& out) noexcept;

// The one place the SL boundary becomes an NR frame. Both hosts own one of these; the research host also
// passes an observer, the controller host passes none.
class SlFrameProvider final:public SlAdmissionSink {
    live::IFrameReceiver& receiver_;
    IResearchObserver* observer_;
    FrameProfile profile_;
    std::atomic<const char*> blocked_{nullptr};
    // What self-configuration read from the last resolved call.
    mutable std::mutex self_mutex_;
    std::uint64_t self_calls_=0;
    json observed_=nullptr;
    void self_configure(const slboundary::Resolution&,const rr::Packet&,FrameProfile&) noexcept;
public:
    SlFrameProvider(live::IFrameReceiver& receiver,FrameProfile profile,IResearchObserver* observer=nullptr) noexcept
        :receiver_(receiver),observer_(observer),profile_(profile){}
    // Why the last RR call did not become a frame; null once one did.
    const char* blocked()const noexcept{return blocked_.load();}
    // The facts self-configuration read from the game's own RR calls (what a
    // package would otherwise carry). Null until the first call, or when not enabled.
    json observed()const;
    void game_binding_enter(const slboundary::Resolution&,void*) noexcept override;
    void game_binding_exit(std::uint64_t) noexcept override;
    void enter(const slboundary::Resolution&,const rr::Packet&,void* command) noexcept override;
    void boundary_returned(std::uint64_t call,bool success) noexcept override;
    void aborted(std::uint64_t call) noexcept override;
    void unavailable() noexcept override;
    void unavailable(const RejectedCall&) noexcept override;
};
}
