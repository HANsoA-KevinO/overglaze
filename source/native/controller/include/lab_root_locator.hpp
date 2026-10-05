// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// Where the Lab lives, for the programs that run OUTSIDE a game: the viewer,
// overglaze_games, the Steam launcher, the game manager and the dev console.
// None of them spells out a fixed path or derives <self>\..\.. silently; they
// all resolve the root by the rules below.
//
// In the controller track and in its own library: nothing here is linked into
// the in-game host, bridge or proxy, nor into the installation checker
// (lab_installation_tests.exe = app\tools\overglaze_install_check.exe). Under
// host contract V4 the in-game side does not compare output_root against a
// compiled constant: it takes the data root the install record
// (overglaze.install.json) holds, which the manager writes from THIS
// resolution, and holds it to the
// same disk rules (core lab_local_path.hpp).
//
// Resolution, in order:
//   1. <directory of the executable>\overglaze-root.json, when present:
//        {"schema":"overglaze-root-override-v1","lab_root":"D:\\Lab"}
//      A present but malformed file is an error, never a silent fallback.
//   2. <root>\app\<program>.exe                      (the published layout)
//   3. <root>\data\_build*\<program>.exe             (a track build directory)
// Every candidate must be a drive-letter path on a local fixed disk with no
// reparse point on the way (junction, symbolic link, cloud placeholder).
// The data root is always <root>\data: installed games find app\adapters as
// the parent of their output_root, so the two cannot be separated yet.
#include <filesystem>
#include <functional>
#include <string>

namespace lab::root {
enum class Source { override_file, app_layout, build_layout };
struct Location {
    std::filesystem::path root, data;
    Source source=Source::app_layout;
    std::filesystem::path override_file; // the file consulted for step 1
    const char* source_name() const noexcept;
};
inline constexpr const wchar_t* kOverrideFile=L"overglaze-root.json";
inline constexpr const char* kOverrideSchema="overglaze-root-override-v1";
// GetDriveTypeW of "X:\"; tests inject other answers. Only DRIVE_FIXED passes.
using DriveType=std::function<unsigned(const std::wstring& drive_root)>;
// Throws unless p is an absolute, normalized X:\ path (no UNC, \\?\, device,
// alternate data stream or "..") on a local fixed disk with no reparse point on
// any existing component. With must_exist, p itself has to exist.
void require_local_fixed(const std::filesystem::path& p,bool must_exist=true,const DriveType& drive={});
// Pure: from an executable path. Throws with the reason; never guesses.
Location resolve_from(const std::filesystem::path& executable,const DriveType& drive={});
// The running process image (GetModuleFileNameW).
std::filesystem::path self_executable();
Location resolve_self();
}
