// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// Which game frame a native Present belongs to, in the game's own words -- its
// Streamline latency markers (lab_latency_markers.hpp). Plain, trivially copyable
// records. Shared: the binding layer of both hosts reads them at
// every Present (lab_sl_bindings.hpp present_boundary_of_frame), and the research
// chain capture carries them across its bridge ABI (lab_chain_types.hpp).
// Values are the public PCL/Reflex marker numbers.
#include <cstdint>
#include <type_traits>
namespace lab::chain {
enum class MarkerSource : unsigned {none=0,pcl=1,reflex=2};
inline const char* marker_source_name(MarkerSource s) noexcept {return s==MarkerSource::pcl?"slPCLSetMarker":s==MarkerSource::reflex?"slReflexSetMarker":"none";}
enum class PresentMarkerState : unsigned {
    unavailable=0,  // no marker function hooked in this process
    bound=1,        // this thread's newest Present marker is an ePresentStart, inside the window
    no_marker=2,    // this thread's newest Present marker is an ePresentEnd, or it has none in the scan
    other_thread=3, // only another thread has an open ePresentStart inside the window
    stale=4,        // this thread's open ePresentStart is older than the window
    lost=5          // the marker ring was overwritten while it was being read
};
inline const char* present_marker_state_name(PresentMarkerState s) noexcept {
    switch(s){case PresentMarkerState::unavailable:return "no-game-latency-markers";case PresentMarkerState::bound:return "bound";
        case PresentMarkerState::no_marker:return "no-game-marker-on-this-thread";case PresentMarkerState::other_thread:return "open-marker-on-another-thread";
        case PresentMarkerState::stale:return "stale-present-start";case PresentMarkerState::lost:return "marker-ring-lost";}
    return "unknown";
}
inline constexpr unsigned kMarkerPresentStart=4,kMarkerPresentEnd=5;
struct PresentMarker {
    PresentMarkerState state=PresentMarkerState::unavailable;
    MarkerSource source=MarkerSource::none;
    // The bound (or, for other_thread, the other thread's) ePresentStart: its frame
    // index when the token address was seen issued, the marker's thread and QPC.
    // no_marker closed by this thread's ePresentEnd: that ePresentEnd (marker 5,
    // its ring sequence and QPC; frame unused). frame_known: 0 unknown, 1 known,
    // 2 ambiguous -- the token address was reissued before the frame it carried
    // before had its ePresentStart (token aliasing).
    unsigned marker=UINT32_MAX,frame=0,frame_known=0,thread=0;
    std::uint64_t qpc=0,sequence=0;
    // Process-wide at this Present: ePresentStart markers observed so far, and
    // which functions are hooked (bit 0 slPCLSetMarker, bit 1 slReflexSetMarker).
    std::uint64_t present_starts=0;
    unsigned functions=0,token_untrusted=0;
    // The one reading the binding layer acts on: this thread's own open
    // ePresentStart, inside the window, with a frame index known (never ambiguous).
    bool declares_frame() const noexcept {return state==PresentMarkerState::bound&&frame_known==1;}
};
static_assert(std::is_trivially_copyable_v<PresentMarker>);
}
