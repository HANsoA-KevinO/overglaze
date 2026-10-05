// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// Is this a directory the product may write into? Decided from the spelling and
// the disk alone (host contract V4).
//
// Shared by the out-of-game root locator (lab_root_locator.hpp) and, since V4,
// by the in-game installation contract: the host no longer compares its data
// root against a compiled machine path, it takes the root its install receipt
// records and holds it to exactly these rules. The threat model is a program
// error writing sessions where it should not, not an attacker: the same user
// can write both the game directory and the data root.
#include <filesystem>
#include <functional>
#include <string>

namespace lab::localpath {
// GetDriveTypeW of "X:\"; tests inject other answers. Only DRIVE_FIXED passes.
using DriveType=std::function<unsigned(const std::wstring& drive_root)>;
// Throws "<title> [<tag>:<kind>]：<why>" unless p is an absolute, normalized X:\
// path (no UNC, \\?\, device, alternate data stream or "..") on a local fixed
// disk with no reparse point on any existing component. With must_exist, p
// itself has to exist. Kinds: path, network, drive, access, missing, reparse.
void require_local_fixed(const std::filesystem::path& p,bool must_exist,const DriveType& drive,const char* title,const char* tag);
}
