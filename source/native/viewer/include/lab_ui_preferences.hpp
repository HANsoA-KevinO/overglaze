// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include "lab_viewer_color.hpp"
#include <memory>
#include <optional>
namespace lab {
struct ViewerPreferences {
    viewer::Display display=[] {viewer::Display d;viewer::viewing_preset(d,0);return d;}();
    bool nearest=true,library=true,hdr=false,processing=true;unsigned view=0;float wipe=.5f;
    json document() const;
    static ViewerPreferences parse(const json&);
};
struct OverlayPreferences {
    unsigned hotkey=VK_INSERT;float white=203;
    // Version 2: the last requested model controls and host exposure, restored
    // as a staged request at the next start. The NR ON/OFF gate is never
    // persisted; every start remains OFF until the user enables it.
    // Version 3 (Live ABI22) adds Skin and AutoMask; Tone/Structure 0..2.
    bool has_model=false;float tone=1,structure=1,exposure_stops=0,skin=1;unsigned style=0,exposure_auto=0,automask=0;
    json document() const;
    static OverlayPreferences parse(const json&);
};
// Small, local UI preferences plus the last requested model controls. No NR
// mode, experiment state or input interpretation is persisted. Disk I/O lives
// on the worker, never Present.
class UiPreferencesFile final {
    struct Impl;std::unique_ptr<Impl> p_;
public:
    // data_root: the directory the file must sit one level below (<root>\<sub>\x.json).
    // Empty means the file's own grandparent, still held to the same shape.
    UiPreferencesFile(std::filesystem::path,json defaults,std::filesystem::path inherit={},std::filesystem::path data_root={});
    ~UiPreferencesFile();
    json loaded() const;
    void save(json);
    std::string error() const;
    void close();
};
}
