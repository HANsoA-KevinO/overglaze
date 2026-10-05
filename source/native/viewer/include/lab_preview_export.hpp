// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_capture_library.hpp"
#include "lab_viewer_color.hpp"
namespace lab {
// Creates a new derived SDR package only on explicit user request. Always
// includes the exact source digest and common display recipe; never rewrites raw.
std::filesystem::path export_sdr_pair(const RawCapture&,int left,int right,viewer::Display,const std::filesystem::path& root);
}
