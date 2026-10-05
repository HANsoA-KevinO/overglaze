// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <stdexcept>
namespace lab::color {
// Inserts only PNG color-description chunks, never changes IDAT samples.
// Replace encoder defaults with the recorded source description. WIC may emit
// an sRGB chunk even when the application did not set a color context.
inline std::vector<std::uint8_t> tag_g22_p709(const void* memory,std::size_t size){
    const auto* p=static_cast<const std::uint8_t*>(memory);
    constexpr std::array<std::uint8_t,8> signature{137,80,78,71,13,10,26,10};
    if(size<33||!std::equal(signature.begin(),signature.end(),p))throw std::runtime_error("Invalid encoded PNG");
    auto be=[](const std::uint8_t* v){return (std::uint32_t(v[0])<<24)|(std::uint32_t(v[1])<<16)|(std::uint32_t(v[2])<<8)|v[3];};
    std::vector<std::uint8_t> tail;
    for(std::size_t at=8;at<size;){if(size-at<12)throw std::runtime_error("Truncated PNG chunk");const auto n=be(p+at);if(n>size-at-12)throw std::runtime_error("PNG chunk overflow");
        const std::string_view type(reinterpret_cast<const char*>(p+at+4),4);
        if(at!=8&&type!="sRGB"&&type!="gAMA"&&type!="cHRM"&&type!="iCCP")tail.insert(tail.end(),p+at,p+at+n+12);at+=n+12;}
    if(be(p+8)!=13||std::string_view(reinterpret_cast<const char*>(p+12),4)!="IHDR")throw std::runtime_error("Missing PNG IHDR");
    std::vector<std::uint8_t> result(p,p+33);
    auto u32=[&](std::uint32_t v){for(int shift=24;shift>=0;shift-=8)result.push_back(std::uint8_t(v>>shift));};
    auto chunk=[&](const char* name,const std::vector<std::uint32_t>& values){u32(std::uint32_t(values.size()*4));const auto start=result.size();for(unsigned i=0;i<4;++i)result.push_back(name[i]);for(auto v:values)u32(v);
        std::uint32_t crc=~0u;for(auto i=start;i<result.size();++i){crc^=result[i];for(unsigned b=0;b<8;++b)crc=(crc>>1)^(0xedb88320u&std::uint32_t(-int(crc&1)));}u32(~crc);};
    chunk("gAMA",{45455});chunk("cHRM",{31270,32900,64000,33000,30000,60000,15000,6000});
    result.insert(result.end(),tail.begin(),tail.end());return result;
}
}
