// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
namespace lab::nr {
// CPU side of the host auto-exposure. The GPU meter reports the mean log2
// luminance of the working RGB; this turns it into the gain (log2 stops) the
// colour preparation multiplies in before the compression operator. The user's
// exposure_stops becomes an offset on top. One-frame latency by design: the
// reading retired with frame N drives frame N+1, smoothed unless a snap is
// requested (first reading, history reset). It is a wrapper policy, not the
// game's exposure and not an NR model parameter.
struct ExposureController {
    static constexpr float target_log2_luminance=-2.47393f; // log2(0.18), photographic mid-grey
    static constexpr float seed_stops=5.f;                  // used until the first reading arrives
    // The range has to cover SCENE-REFERRED input, not only pre-exposed input.
    // It used to be -2..+10, sized for 007 (working RGB mean ~0.0045, needs
    // about +5). Alan Wake 2 hands RR un-exposed scene luminance and passes its
    // exposure separately as a texture (tag type 13): in a daylight forest the
    // working RGB log2 mean was 6.71 (~104), which needs -9.2 stops, and the -2
    // floor left NR an all-white input -- running, retiring, and changing
    // nothing visible, with the EV slider unable to reach it either.
    // Mean log2 is a geometric mean, so a wide range does
    // not make the meter jumpy; the smoothing below still applies.
    static constexpr float min_auto_stops=-14.f,max_auto_stops=14.f;
    static constexpr float min_applied_stops=-18.f,max_applied_stops=18.f;
    float alpha=0.1f;   // per-frame blend toward the target (~10 frames)
    bool valid=false;
    float stops=seed_stops;
    float gain_stops() const noexcept {return stops;}
    float update(float mean_log2_luminance,bool snap) noexcept {
        if(!std::isfinite(mean_log2_luminance))return stops;
        const float target=std::clamp(target_log2_luminance-mean_log2_luminance,min_auto_stops,max_auto_stops);
        stops=(!valid||snap)?target:stops+(target-stops)*alpha;valid=true;return stops;
    }
    void reset() noexcept {valid=false;stops=seed_stops;}
    static float clamp_applied(float s) noexcept {return std::clamp(s,min_applied_stops,max_applied_stops);}
};
}
