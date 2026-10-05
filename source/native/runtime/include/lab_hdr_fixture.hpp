// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_preconvert_reference.hpp"
#include <string_view>

// Synthetic numeric domain experiment, NOT a verified game/NR HDR contract.
namespace lab::hdr_fixture {
inline bool valid(std::string_view condition) {
    return condition=="bounded" || condition=="direct" || condition=="clipped" || condition=="encoded";
}
inline std::array<std::uint16_t,4> pixel(unsigned x,unsigned y,unsigned width,unsigned height,std::string_view condition) {
    if(!valid(condition) || !width || !height || x>=width || y>=height)
        throw std::invalid_argument("Invalid HDR fixture");
    constexpr float levels[]{0,.01f,.05f,.125f,.25f,.5f,.75f,1};
    const float texture=((x/8+y/8)&1)?1.0f:.875f;
    const float base=levels[std::min(7U,x*8/width)]*texture;
    std::array<std::uint16_t,4> result{};
    const preconvert::Constants constants{{width,height},{width,height},{0,0},{width,height},1,1,1,1,{}};
    for(unsigned c=0;c<3;++c){
        // Top half neutral; bottom half has known RGB ratios, not game colourimetry.
        const float channel=y<height/2?1.0f:(c==0?1.0f:c==1?.5f:.25f);
        float value=base*channel*(condition=="bounded"?1.0f:16.0f);
        // Establish the common FP16 source BEFORE branching into transforms.
        value=preconvert::unhalf(preconvert::half(value));
        if(condition=="clipped")value=std::clamp(value,0.0f,1.0f);
        if(condition=="encoded")value=preconvert::rgb(value,constants);
        result[c]=preconvert::half(value);
    }
    result[3]=preconvert::half(1.0f);return result;
}
}
