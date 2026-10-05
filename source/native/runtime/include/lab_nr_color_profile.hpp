// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_prepost_ratio_codec.hpp"
#include <stdexcept>

namespace lab::nr {
// The host (native or ReShade) does not select the color algorithm. This is the
// current experimental GAME recipe, not an observation of any game's runtime values.
// Keep model Tone/Structure/Style and viewer settings out of these constants.
inline constexpr char current_color_profile[]="lab-linear-identity-op3-v1";
inline constexpr char color_algorithm_reference[]="prepost-ratio-static-v1";
inline PrePostColorConstants experimental_color_constants(unsigned image_width,unsigned image_height) {
    if(!image_width||!image_height||image_width>8192||image_height>8192)
        throw std::invalid_argument("Explicit bounded color extent required");
    PrePostColorConstants c{};
    c.width=image_width;c.height=image_height;c.pre_post=1;c.tonemap_operator=3;
    c.exposure=1;c.sigma=10;c.gamma=.454f;
    c.pre_matrix={{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
    c.post_matrix=c.pre_matrix;
    return c;
}
}
