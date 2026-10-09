// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace lab::nr {
// Conservative laboratory range. Not an assertion about the complete SDK range.
// Style 0..2: pinned 310.8 registry count + original GPU tests, not A/B/C names.
struct Settings {
    float tone=1.f, structure=1.f;
    std::uint32_t style=0;
    // Host colour-preparation exposure in log2 stops: working RGB is multiplied
    // by 2^stops before the compression operator and divided back afterwards.
    // It is a Lab wrapper setting, not an NR model parameter; the DLL never
    // reads it, so it is excluded from actual-read matching.
    float exposure_stops=0.f;
    // 0: exposure_stops is the absolute host gain. 1: the host meters the frame
    // (mean log2 luminance -> mid-grey) and exposure_stops is an offset on top.
    std::uint32_t exposure_auto=0;
    // Diagnostic view only: left half of the composite shows the untouched
    // working RGB, right half the NR result. Captures are refused while set.
    std::uint32_t compare_split=0;
    // DLSSNR.SkinStructureStrength and DLSSNR.UseAutoMask (Live ABI22).
    // Skin only acts while UseAutoMask=1 (offline: with the mask off, Skin 0/1/2
    // and the SDK default -1 give bit-identical output; with it on, -1 equals 1),
    // so the default 1 with the mask off leaves the output unchanged. Skin 0
    // keeps NR almost entirely off skin. The panel turns the mask on when Skin
    // is moved.
    float skin=1.f;
    std::uint32_t automask=0;
    // Edit extrapolation (Live ABI25), a Lab composite setting the DLL never
    // reads. With extrapolate=1, what is composited back is
    // N' = saturate(C + extrapolate_factor * (N - C)) per channel, where C is the
    // encoded image handed to NR and N its output in that same domain. NR still
    // runs once and keeps its own output as history. Off, or a factor of 1, is
    // exactly the plain composite.
    std::uint32_t extrapolate=0;
    float extrapolate_factor=2.f;
    // Manual exposure (and the offset on top of auto). The floor was -6, which
    // cannot reach scene-referred input such as Alan Wake 2 in daylight.
    static constexpr float min_exposure_stops=-12.f, max_exposure_stops=10.f;
    // Tone/Structure were 0..1. The model has no hard cap (measured: outputs
    // still change up to 16, clipping and artefacts from about 4); RenoDX uses
    // 1.85 / 2.00. 2 is the laboratory ceiling. Skin keeps the same ceiling.
    static constexpr float max_tone_structure=2.f, max_skin=2.f;
    static constexpr float min_extrapolate_factor=1.f, max_extrapolate_factor=4.f;
    bool valid() const noexcept {
        return std::isfinite(tone)&&std::isfinite(structure)&&std::isfinite(exposure_stops)&&std::isfinite(skin)&&
            tone>=0.f&&tone<=max_tone_structure&&structure>=0.f&&structure<=max_tone_structure&&style<3&&
            exposure_stops>=min_exposure_stops&&exposure_stops<=max_exposure_stops&&exposure_auto<=1&&compare_split<=1&&
            skin>=0.f&&skin<=max_skin&&automask<=1&&extrapolate<=1&&std::isfinite(extrapolate_factor)&&
            extrapolate_factor>=min_extrapolate_factor&&extrapolate_factor<=max_extrapolate_factor;
    }
    // The factor the composite applies: 1 (the plain composite) while off.
    float applied_extrapolation() const noexcept {return extrapolate?extrapolate_factor:1.f;}
    float exposure_gain() const noexcept {return std::exp2(exposure_stops);}
    bool model_equal(const Settings& o) const noexcept {return tone==o.tone&&structure==o.structure&&style==o.style&&skin==o.skin&&automask==o.automask;}
    bool operator==(const Settings&) const = default;
};
struct SettingsReads {
    Settings values{};
    // bit 1 Tone, bit 2 Structure: required, as before ABI22.
    std::uint32_t mask=0;
    bool style_observed=false;
    // ABI22. Skin and UseAutoMask are matched when the DLL read them, never
    // required: an unread optional control is reported, not a fault.
    bool skin_observed=false,automask_observed=false;
    // Only DLL-read fields are matched; exposure_stops and extrapolation are host-applied.
    bool matches(const Settings& expected) const noexcept {
        return mask==3&&style_observed&&values.tone==expected.tone&&values.structure==expected.structure&&values.style==expected.style&&
            (!skin_observed||values.skin==expected.skin)&&(!automask_observed||values.automask==expected.automask);
    }
};
static_assert(std::is_trivially_copyable_v<Settings>);
static_assert(std::is_trivially_copyable_v<SettingsReads>);
}
