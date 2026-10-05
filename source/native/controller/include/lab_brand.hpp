// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The product name as the out-of-game programs DISPLAY it: 釉光 · Overglaze --
// "釉光 · Overglaze" in Chinese text, "Overglaze" in English; the technical id
// is "overglaze". Applied with host contract V4.
//
// Display text only. Never put an identity here: pipe names, event names,
// window class names, schema kinds, environment variables, or any file name
// written into a game directory (dxgi.dll, overglaze\, overglaze.install.json)
// live in lab_identity.hpp and are how installed games and running hosts
// recognise us; renaming them is a migration, not a string change. In-game text
// (panel header, startup failure dialog) is compiled into the host and spells
// the same name itself.
namespace lab::brand {
inline constexpr const wchar_t* kProductName=L"釉光 · Overglaze";
// Viewer (overglaze_viewer.exe)
inline constexpr const wchar_t* kViewerWindowTitle=L"釉光 · Overglaze — 游戏管理 / 采集浏览";
inline constexpr const char* kViewerHeader="釉光 · Overglaze";
inline constexpr const wchar_t* kViewerStartFailureTitle=L"釉光 · Overglaze 查看器无法启动";
// Steam launch-option launcher (overglaze_launch.exe)
inline constexpr const wchar_t* kLauncherTitle=L"釉光 · Overglaze 启动器";
inline constexpr const wchar_t* kLauncherUsage=
    L"在 Steam 的启动选项里填写：\n\n\"<程序目录>\\overglaze_launch.exe\" %command%\n\n它会照常启动游戏；进游戏后第一次按 Insert 时加载 釉光 · Overglaze 并打开面板。";
// Command line help (overglaze_games.exe): the program name as shown in the usage text.
inline constexpr const wchar_t* kGamesCliName=L"overglaze_games";
}
