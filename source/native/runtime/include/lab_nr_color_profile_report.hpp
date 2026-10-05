// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_nr_color_profile.hpp"
#include <nlohmann/json.hpp>
#include <cmath>

namespace lab::nr {
// Export the ACTUAL supplied constants, never manufacture them from a texture
// format or from the fact that ReShade successfully loaded. This metadata does
// not admit a frame or certify GPU placement; the adapter must do that separately.
inline nlohmann::json color_profile_report(const PrePostColorConstants& c,bool invalid_rgb_guard) {
    const auto expected=experimental_color_constants(c.width,c.height);
    if(c.pre_matrix!=expected.pre_matrix||c.post_matrix!=expected.post_matrix||
       !std::isfinite(c.exposure)||c.exposure<=0||c.tonemap_operator!=expected.tonemap_operator||
       c.sigma!=expected.sigma||c.gamma!=expected.gamma||c.pre_post!=expected.pre_post||c.reserved)
        throw std::invalid_argument("Changed recipe requires a new explicitly identified color profile");
    return {{"profile",current_color_profile},{"algorithm_reference",color_algorithm_reference},
        {"constants_origin","explicit-lab-experimental-configuration"},
        {"official_runtime_constants_observed",false},{"official_runtime_equivalence",false},
        {"game_color_semantics_verified",false},{"host_selects_color_math",false},
        {"stages",{"working-color","prepost-ratio-prepare","original-NR-API","prepost-ratio-composite","working-color-writeback"}},
        {"pre_matrix",c.pre_matrix},{"post_matrix",c.post_matrix},{"exposure",c.exposure},{"exposure_source","host-setting-exposure_stops-per-frame"},
        {"operator",c.tonemap_operator},{"sigma",c.sigma},{"gamma",c.gamma},
        {"pre_post",c.pre_post},{"width",c.width},{"height",c.height},
        {"lab_extensions",{{"invalid_rgb_guard",invalid_rgb_guard},{"preserve_original_alpha",true}}},
        {"viewer_transform_in_model_path",false},{"nr_output_is_api_texture_not_pure_tensor",true},
        {"placement_required","same-frame producer before NR, NR before original downstream consumers; no Present fallback"}};
}
}
