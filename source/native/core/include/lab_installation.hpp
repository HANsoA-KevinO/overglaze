// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include "lab_game_profile.hpp"
#include <optional>
namespace lab {
// How this installation is loaded into the game, and therefore where its own
// files live. Absent from a config means the original layout: a dxgi.dll proxy
// in the game's own directory, which is what every install made before the
// loader object existed is.
struct Loader {
    std::string strategy="root_dxgi_minimal"; // root_dxgi_minimal | root_proxy_d3d12 | root_proxy_on_insert | late_d3d12 | reframework_plugin
    std::string basename="dxgi.dll";          // the loader DLL's own file name
    std::filesystem::path subdir;             // relative to the game directory; empty = the game root
    bool root()const noexcept{return strategy=="root_dxgi_minimal";}
    // The root-proxy strategy: the host and the whole payload sit in subdir,
    // exactly as for late loading, and a THIN dxgi.dll in the game directory
    // loads the host at the game's first CreateDXGIFactory. The game's loader
    // therefore resolves one tiny module instead of the entire host, which is
    // the difference that matters before the game runs.
    bool root_proxy()const noexcept{return strategy=="root_proxy_d3d12"||strategy=="root_proxy_on_insert";}
    // The same thin proxy, but it loads nothing while the game starts. The host
    // arrives only when the player first presses Insert in the game's own
    // foreground window, and then comes up exactly as a late attach would. For
    // games that refuse anything present during start-up but accept the same
    // host once in play (RE9: the root proxy died after 369 frames, while every
    // in-game late attach lived).
    bool on_insert()const noexcept{return strategy=="root_proxy_on_insert";}
    // Does the host arrive into a game that is already running? Then it missed
    // swapchain and command-signature creation, and its late-attach rules apply.
    bool late_host()const noexcept{return strategy=="late_d3d12"||on_insert();}
    // The one file this strategy places in the game directory itself; empty
    // for every other strategy. Strategy-bound, like the basenames.
    std::string proxy_basename()const{return root_proxy()?std::string("dxgi.dll"):std::string();}
};
struct Installation {
    std::string profile,bridge_sha256,console_sha256,package;
    // V4, optional: the SHA-256 of an unrecognized model the user chose to allow,
    // pinned when the manager generated this package. The bridge then loads a
    // reviewed version or exactly this file (lab_model_versions.hpp accepted()).
    // Empty -- every install without the opt-in -- means reviewed versions only.
    std::string model_sha256;
    profiles::Facts facts; // reviewed row or V3 data-driven facts
    Loader loader;
    // loader_directory == game_directory / loader.subdir. The loader, the NR
    // bridge, the config and the model are all resolved against it; only the
    // game executable and the pinned game modules live in game_directory.
    std::filesystem::path game_directory,loader_directory,console,output_root,run;
    bool open_console=false,in_game_controls=false,exception_diagnostics=false;
};
// Parses and validates the "loader" object. Throws on anything that could
// escape the game directory or rename the loader into a system DLL's spelling.
Loader parse_loader(const json&);
std::filesystem::path module_path(HMODULE module);
bool has_installation(HMODULE module);
// Host contract V4. The data root the receipt beside `module`
// records, read without validating anything else; nullopt when there is no
// readable receipt or it names no absolute root.
std::optional<std::filesystem::path> recorded_output_root(HMODULE module) noexcept;
std::optional<std::filesystem::path> recorded_output_root(const std::filesystem::path& config) noexcept;
// True when that receipt names a data root that no longer exists, or -- for a
// package-bearing receipt -- a program folder (<root>\..\app) that is gone: the
// user deleted the program without uninstalling first. The host then stands
// down silently and the proxy keeps forwarding DXGI: no dialog, no record, no
// crash. A receipt that cannot be read is NOT this case; the full contract
// reports that one as before.
bool installation_root_missing(HMODULE module) noexcept;
bool installation_root_missing(const std::filesystem::path& config) noexcept;
// A synthetic fixture's data root (no installation at all): the parent of its
// live run directory OVERGLAZE_LIVE_DATA_PATH, held to the same local-disk rules.
std::filesystem::path fixture_data_root();
Installation parse_installation(const json&,const json& exe,const json& host);
Installation load_installation(HMODULE module,const json& exe,const json& host);
Installation load_installation_file(const std::filesystem::path& path,const json& exe,const json& host);
void activate_installation(Installation&);
void open_installed_console(const Installation&);
}
