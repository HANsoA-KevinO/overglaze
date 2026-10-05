// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The NR model versions this product has been reviewed against (host contract
// V4).
//
// The model, nvngx_dlssnr.dll, belongs to NVIDIA. This project never includes,
// redistributes or downloads it: the user supplies their own copy. What the
// product does decide is which exact builds it will load, because the bridge
// drives a non-public interface whose behaviour is only known for the builds
// listed here. A file whose SHA-256 is not in this table is refused, by the
// in-game bridge (on disk and again as mapped) and by the manager before it
// copies anything, and the manager says "unknown version" instead of failing
// obscurely. Adding a row is a reviewed change, never a configuration.
//
// Shared by the in-game bridge and the out-of-game manager; no code beyond the
// lookup.
#include <string_view>

namespace lab::model {
inline constexpr const char* kFileName="nvngx_dlssnr.dll";
struct KnownVersion {const char* sha256;const char* label;};
inline constexpr KnownVersion kKnownVersions[]{
    {"e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e","v1 (reviewed baseline)"},
};
// nullptr when the hash names no reviewed version.
inline const KnownVersion* known(std::string_view sha256) noexcept {
    for(const auto& v:kKnownVersions)if(sha256==v.sha256)return &v;
    return nullptr;
}
}
