// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include "lab_color_contract.hpp"
#include <array>
#include <memory>
#include <vector>
namespace lab {
struct CaptureEntry {std::filesystem::path manifest;std::string run,pair,origin,error,application,parameters;std::uint64_t frame=0,call=0,bytes=0,modified=0;unsigned width=0,height=0;bool synthetic=false;};
struct CaptureCatalog {std::vector<CaptureEntry> entries;unsigned skipped=0;bool truncated=false;};
CaptureCatalog scan_capture_library(const std::filesystem::path& root);
// Uses the catalog's on-disk manifest location, never a path from its JSON.
// Also works for incomplete records; absent folders fail without opening a fallback.
std::filesystem::path capture_directory(const std::filesystem::path& manifest);
// Locked, read-only mappings. No SDR conversion, resizing or file modification.
class RawCapture final {
    struct Impl;std::unique_ptr<Impl> p_;
public:
    explicit RawCapture(const std::filesystem::path& manifest);
    ~RawCapture();
    RawCapture(const RawCapture&)=delete;
    const json& manifest() const;
    const std::filesystem::path& path() const;
    const std::string& manifest_hash() const;
    unsigned width()const;unsigned height()const;
    unsigned stage_count()const;
    unsigned format(unsigned stage)const;
    color::Contract color_contract(unsigned stage)const;
    bool display_pair()const;
    unsigned row_pitch(unsigned stage)const;
    const void* data(unsigned stage)const;
    std::array<std::uint16_t,4> bits(unsigned stage,unsigned x,unsigned y)const;
    std::array<float,4> pixel(unsigned stage,unsigned x,unsigned y)const;
    std::uint64_t bytes()const;
};
}
