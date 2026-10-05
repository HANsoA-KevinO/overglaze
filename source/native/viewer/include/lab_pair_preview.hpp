// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <array>
#include <vector>

namespace lab {
struct PairPreview {
    unsigned width=0,height=0,original_width=0,original_height=0;
    std::array<std::vector<unsigned char>,4> rgba;
    json manifest;
    std::string source;
};
// Validates the whole raw package; keeps only bounded SDR display previews.
// No file modifications, no rendering/NR work, no inferred display HDR.
// data_root: the caller's data directory; manifest must be <data_root>/<run>/<pair>/manifest.json.
PairPreview load_pair_preview(const std::filesystem::path& manifest,const std::filesystem::path& data_root);
std::array<unsigned char,4> pair_preview_pixel(const std::array<float,4>& value,bool working_hdr);
}
