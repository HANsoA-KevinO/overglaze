// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <vector>

namespace lab {
// Where a game keeps the NVIDIA modules. Most ship them beside the executable;
// an Unreal game ships them inside its plugin tree instead, which is why Halo:
// Campaign Evolved looks like it has no DLSS at all from beside the EXE -- its EXE
// sits in Meteorite\Binaries\Win64 while the models are under Engine\Plugins.
//
// This assumption ("the modules are beside the EXE") was written independently
// in five places: the preflight scan, the installed package's pin check, route
// classification, the installer's pin verification, and the installed-contract
// check that runs INSIDE THE GAME at load. The first three were fixed one at a
// time and the last two still failed, each with an error that named something
// else -- a missing path reads as "missing or contains a reparse point", not as
// "look somewhere else". It lives here so the next caller cannot re-derive it.
//
// Bounded by name and by depth, never a disk scan. Layouts seen so far:
//   beside the EXE                                   (most Steam titles)
//   <exe>\streamline[\<one level>]                   (id Tech: DOOM The Dark Ages
//                                                     \production, Indiana Jones)
//   <ancestor>\Engine\Plugins\...\Binaries\ThirdParty\{Win64,WinGDK}
//   <ancestor>\Plugins\...\Binaries\ThirdParty\{Win64,WinGDK}   (project plugins:
//                                                     Clair Obscur's Sandfall\Plugins)
// with up to three directory levels below Plugins (<plugin>, <group>\<plugin>,
// Hellblade 2's Runtime\Nvidia\DLSS) and at most three ancestors, so the EXE of
// an Xbox app build in <Project>\Binaries\WinGDK still reaches Content\Engine.
// The EXE's own directory is always first.
inline std::vector<std::filesystem::path> module_directories(const std::filesystem::path& exe_dir) {
    namespace fs = std::filesystem;
    std::vector<fs::path> out{exe_dir};
    std::error_code ec;
    const auto add = [&](const fs::path& p) { if (fs::is_directory(p, ec)) out.push_back(p); };
    const auto subdirs = [&](const fs::path& p) {
        std::vector<fs::path> r;
        std::error_code e;
        for (fs::directory_iterator it(p, e), end; it != end && !e; it.increment(e))
            if (it->is_directory(e)) r.push_back(it->path());
        return r;
    };
    // id Tech keeps Streamline in a named folder beside the executable.
    const auto streamline = exe_dir / L"streamline";
    if (fs::is_directory(streamline, ec)) { add(streamline); for (const auto& s : subdirs(streamline)) add(s); }
    // Unreal plugin trees.
    const auto plugin_binaries = [&](const fs::path& plugin) {
        for (const auto* platform : {L"Win64", L"WinGDK"}) add(plugin / L"Binaries" / L"ThirdParty" / platform);
    };
    fs::path root = exe_dir;
    for (unsigned up = 0; up < 3 && root.has_parent_path() && root != root.parent_path(); ++up) {
        root = root.parent_path();
        for (const auto& plugins : {root / L"Engine" / L"Plugins", root / L"Plugins"}) {
            if (!fs::is_directory(plugins, ec)) continue;
            for (const auto& a : subdirs(plugins)) {            // <plugin> or <group>
                plugin_binaries(a);
                for (const auto& b : subdirs(a)) {              // <group>\<plugin>
                    if (b.filename() == L"Binaries" || b.filename() == L"Source" || b.filename() == L"Content") continue;
                    plugin_binaries(b);
                    for (const auto& c : subdirs(b)) {          // <group>\<group>\<plugin>
                        if (c.filename() == L"Binaries" || c.filename() == L"Source" || c.filename() == L"Content") continue;
                        plugin_binaries(c);
                    }
                }
            }
        }
        ec.clear();
    }
    return out;
}

// The one place a pinned module is resolved. Returns an empty path when the
// module is nowhere we recognise, so the caller can say THAT rather than let a
// path check downstream report a missing file as something unrelated.
inline std::filesystem::path find_module(const std::vector<std::filesystem::path>& directories, const std::wstring& name) {
    std::error_code ec;
    for (const auto& candidate : directories) {
        const auto file = candidate / name;
        // The first place it exists is the one that must match. The executable's
        // own directory is searched first, so a plugin copy can never shadow a
        // module the game actually loads from beside itself.
        if (std::filesystem::is_regular_file(file, ec)) return file;
    }
    return {};
}
// Convenience for a single lookup. Callers resolving several modules should
// compute module_directories once: the plugin walk is the expensive part.
inline std::filesystem::path find_module(const std::filesystem::path& exe_dir, const std::wstring& name) {
    return find_module(module_directories(exe_dir), name);
}}
