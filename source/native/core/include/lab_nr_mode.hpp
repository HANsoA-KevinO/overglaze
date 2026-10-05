// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <string_view>
#include <stdexcept>
namespace lab::nr {
// Public product modes. compute_only still runs NR and the private composite,
// but never writes the result into the game's Color resource.
enum class Mode : unsigned { off=0, on=1, compute_only=2 };
inline constexpr const char* mode_name(unsigned mode) noexcept {
    return mode==0?"off":mode==1?"on":mode==2?"compute-only":"unknown";
}
inline unsigned mode_value(std::string_view mode) {
    if(mode=="off")return 0;if(mode=="on")return 1;if(mode=="compute-only")return 2;
    throw std::invalid_argument("Unknown NR mode");
}
inline bool executes(std::string_view mode) noexcept {return mode=="on"||mode=="compute-only";}
}
