// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

// CPU reference for the pre-NR colour conversion and FP16 packing. Not a general
// DLSS conversion contract, and not evidence of any game-time resource bindings.
namespace lab::preconvert {
struct Constants {
    std::uint32_t size[2], source_size[2], source_base[2], proxy_size[2];
    float paper_white, transfer_strength, color_strength;
    std::uint32_t hdr;
    float padding[4]{};
};
static_assert(sizeof(Constants)==64);
inline void validate(const Constants& c, unsigned width, unsigned height) {
    if(!c.size[0] || !c.size[1] || !width || !height ||
       c.size[0]>8192 || c.size[1]>8192 || width>8192 || height>8192 ||
       std::uint64_t(c.source_base[0])+std::max(c.source_size[0],1U)>width ||
       std::uint64_t(c.source_base[1])+std::max(c.source_size[1],1U)>height ||
       (c.hdr && (!std::isfinite(c.paper_white) || c.paper_white<=0)))
        throw std::runtime_error("Unsupported preconvert fixture constants");
}
inline unsigned coordinate(unsigned index, unsigned output_size, unsigned source_size, unsigned base) {
    if(!output_size)throw std::runtime_error("Zero output extent");
    const auto n=std::max(source_size,1U);
    return base+std::min(n-1,static_cast<unsigned>((static_cast<float>(index)+0.5f)*static_cast<float>(n)/static_cast<float>(output_size)));
}
inline float rgb(float input,const Constants& c) {
    float x=std::max(input,0.0f);
    if(!c.hdr)return x;
    x/=c.paper_white;
    if(x>0.75f)x=0.75f+0.25f*(1.0f-std::exp2((x-0.75f)*-8.325476f));
    x=std::clamp(x,0.0f,1.0f);
    return x<=0.0031308f?12.92f*x:1.055f*std::pow(x,1.0f/2.4f)-0.055f;
}
// Binary16 conversion, round-to-nearest-even. No host F16C/rounding-mode dependency.
inline std::uint16_t half(float f) {
    const auto u=std::bit_cast<std::uint32_t>(f);const auto sign=(u>>16)&0x8000;
    const int e=static_cast<int>((u>>23)&255)-127;const auto m=u&0x7fffff;
    if(e==128)return static_cast<std::uint16_t>(sign|0x7c00|(m?0x200:0));
    if(e>15)return static_cast<std::uint16_t>(sign|0x7c00);
    if(e<-25)return static_cast<std::uint16_t>(sign);
    const unsigned shift=e<-14?static_cast<unsigned>(-e-1):13U;
    const auto mantissa=e<-14?(m|0x800000):m;
    auto rounded=mantissa>>shift;
    const auto rem=mantissa&((1U<<shift)-1), halfway=1U<<(shift-1);
    if(rem>halfway || (rem==halfway && (rounded&1)))++rounded;
    const auto exponent=e<-14?0U:static_cast<unsigned>(e+15)<<10;
    return static_cast<std::uint16_t>(sign|(exponent+rounded));
}
inline float unhalf(std::uint16_t h) {
    const auto sign=static_cast<std::uint32_t>(h&0x8000)<<16;
    const auto e=(h>>10)&31, m=h&1023;
    if(e==31)return std::bit_cast<float>(sign|0x7f800000|(static_cast<std::uint32_t>(m)<<13));
    if(!e)return (h&0x8000?-1.0f:1.0f)*std::ldexp(static_cast<float>(m),-24);
    return std::bit_cast<float>(sign|(static_cast<std::uint32_t>(e+112)<<23)|(static_cast<std::uint32_t>(m)<<13));
}
}
