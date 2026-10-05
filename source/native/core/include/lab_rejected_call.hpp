// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <type_traits>
namespace lab {
// skipped_before_insertion: the outer RR call completed, nothing of ours was
// recorded, and the miss is not a nested call. The live backend may skip that
// frame and reset NR history instead of failing; persistent misses still fail.
// recording_discarded_by_game_reset: our frame WAS recorded, then the game
// Reset that list before executing it; nothing ran, history reset, ON kept.
enum class RejectedDisposition:std::uint32_t {terminal=0,constants_missing_before_nr=1,skipped_before_insertion=2,recording_discarded_by_game_reset=3};
// Fixed metadata for one rejected invocation; no borrowed resource pointers.
struct RejectedCall {
    std::uint64_t call=0,frame=0;
    char stage[48]{},reason[160]{};
    // Set only by the adapter after a successful matched original return,
    // with intact coverage and no invocation of our NR insertion receiver.
    RejectedDisposition disposition=RejectedDisposition::terminal;
};
static_assert(std::is_trivially_copyable_v<RejectedCall>);
}
