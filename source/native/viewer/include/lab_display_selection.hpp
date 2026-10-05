// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
namespace lab {
inline std::uint64_t display_intersection(const RECT& a,const RECT& b){const auto w=std::max<std::int64_t>(0,std::int64_t(std::min(a.right,b.right))-std::max(a.left,b.left));const auto h=std::max<std::int64_t>(0,std::int64_t(std::min(a.bottom,b.bottom))-std::max(a.top,b.top));return std::uint64_t(w)*std::uint64_t(h);}
}
