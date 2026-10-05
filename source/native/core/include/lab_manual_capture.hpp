// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <memory>

namespace lab {
// Repeatable, bounded CPU callback capture. No NGX writes, GPU queries,
// image readback, object creation tracking, or files while idle.
class ManualCapture {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    ManualCapture(std::filesystem::path root,std::string origin);
    ~ManualCapture();
    void start(const json& params);
    void stop(const std::string& reason="operator_stop");
    void observe_present(std::uint64_t swapchain) noexcept;
    json snapshot() const;
    bool busy() const;
};
}
