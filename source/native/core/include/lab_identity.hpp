// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The product's technical identity (釉光 · Overglaze, technical id
// "overglaze"), and the names it had before host contract V4.
//
// These names are how installed games, running hosts and the manager recognise
// one another: the payload subdirectory and files in a game directory, the
// installation receipt beside the host. The current names are the only ones a
// V4 host accepts. The legacy names exist for exactly one purpose: so the
// out-of-game manager and the research tools can still recognise, verify and
// remove an installation made before the rename, and so a leftover legacy
// add-on still counts as a conflict. Nothing here ever installs a legacy name.
//
// No code, no allocation: safe to include from the thin root proxy too.
namespace lab::identity {
inline constexpr const char* kTechnicalId="overglaze";
inline constexpr const wchar_t* kPayloadSubdirectory=L"overglaze";
inline constexpr const wchar_t* kInstallationFile=L"overglaze.install.json";
inline constexpr const wchar_t* kBridgeFile=L"overglaze_nvngx.dll";
inline constexpr const wchar_t* kControllerFile=L"overglaze_controller.dll";
inline constexpr const wchar_t* kModelFile=L"nvngx_dlssnr.dll"; // the user's own NVIDIA file; never ours to rename

namespace legacy {
// DLSS Lab / DLSS Pipeline Lab, every install made before host contract V4.
inline constexpr const char* kTechnicalId="dlsslab";
inline constexpr const wchar_t* kPayloadSubdirectory=L"dlsslab";
inline constexpr const wchar_t* kInstallationFile=L"dlsslab.install.json";
inline constexpr const wchar_t* kBridgeFile=L"dlsslab_nvngx.dll";
inline constexpr const wchar_t* kControllerFile=L"dlsslab_controller.dll";
inline constexpr const wchar_t* kReframeworkFile=L"dlsslab_reframework.dll";
inline constexpr const wchar_t* kResearchAddon=L"dlsslab.addon64";
inline constexpr const wchar_t* kPreviewAddon=L"dlsslab_preview.addon64";
}
}
