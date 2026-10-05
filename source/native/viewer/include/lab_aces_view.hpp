// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <algorithm>
// Stephen Hill's ACES RRT/ODT fit from MJP's BakingLab/ACES.hlsl.
// Linear Rec.709 input / linear display RGB output; sRGB encoding is separate.
// This is an approximation, NOT the official ACES reference transform, and NOT
// the game's tone mapper, scene exposure, LUT or calibrated colorimetry.
// MIT attribution and full license: third_party/BakingLab-LICENSE.txt.
namespace lab::viewer {
inline std::array<double,3> aces_fitted(std::array<double,3> rgb) {
    constexpr double input[3][3]{{.59719,.35458,.04823},{.07600,.90834,.01566},{.02840,.13383,.83777}};
    constexpr double output[3][3]{{1.60475,-.53108,-.07367},{-.10208,1.10813,-.00605},{-.00327,-.07276,1.07602}};
    std::array<double,3> fitted{},result{};
    for(unsigned i=0;i<3;++i){double v=0;for(unsigned j=0;j<3;++j)v+=input[i][j]*rgb[j];
        fitted[i]=(v*(v+.0245786)-.000090537)/(v*(.983729*v+.4329510)+.238081);}
    for(unsigned i=0;i<3;++i){for(unsigned j=0;j<3;++j)result[i]+=output[i][j]*fitted[j];result[i]=std::clamp(result[i],0.,1.);}
    return result;
}
}
