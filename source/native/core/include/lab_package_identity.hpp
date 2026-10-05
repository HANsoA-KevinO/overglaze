// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <filesystem>
#include <optional>
#include <string>

namespace lab {
// Identity of a game whose executable cannot be read.
//
// Xbox app (GDK) titles ship a license-protected EXE: opening it is refused, so
// the SHA-256 every other package pins does not exist for them. Policy: pin the
// OPERATING SYSTEM'S package identity instead: the package full name, which
// carries name, version, architecture and publisher id, taken from what
// Windows actually installed -- not from a file anyone could edit.
//
// The package's own manifest (MicrosoftGame.config, or AppxManifest.xml) is
// read only as a LEAD: its Identity Name and Publisher give the package family,
// Windows lists the installed packages of that family, and one of them must
// have an install path that contains the executable. A config that points at a
// package Windows does not have installed there identifies nothing.
struct PackageIdentity {
    std::wstring full_name, family_name, install_path;
    std::string version;  // "major.minor.build.revision" of the installed package
};

// The installed OS package whose install path contains `exe`, if any. Reads no
// executable; touches only the manifest and the appmodel package APIs.
// `why`, when given, receives the first reason no package matched -- so a
// preflight can say "installed at X, which does not contain the EXE" instead of
// a bare refusal.
std::optional<PackageIdentity> installed_package_covering(const std::filesystem::path& exe, std::string* why = nullptr);

// The single identity token for a game executable, used identically by the
// manager, the installation checker and the in-game host so that all three
// compute the same value for the same game:
//   "package:<PackageFullName>" when an installed OS package covers the EXE;
//   otherwise the EXE's lowercase hex SHA-256, exactly as before.
// A Steam/Epic title therefore gets byte-for-byte the value it always had.
std::string game_identity_token(const std::filesystem::path& exe);

// A pinned identity is either a 64-character lowercase hex SHA-256 or a
// package token whose full name is plain (letters, digits, . _ -).
bool valid_identity_token(const std::string& token);
// The running executable's identity record for the in-game host: the package
// token (with the package version) when an OS package covers it, otherwise
// module_identity(nullptr) exactly as before. Never reads a protected EXE.
json game_executable_identity();
bool is_package_token(const std::string& token);

// The same file's real path, for a process that sees its install folder through
// the OS package layout. An Xbox app title installed to a custom folder runs as
//   C:\Program Files\WindowsApps\<PackageFullName>\...      (a junction to)
//   D:\WindowsApps\<PackageFullName>                         (a junction to)
//   D:\Games\<title>\Content\...                            (the real files)
// and the installation contract, which refuses every reparse point on a path,
// would refuse the game's own host: "Installation path missing or contains a
// reparse point" (seen with A Plague Tale: Resonance). Resolved only when ALL
// hold, otherwise `path` comes back unchanged and the contract refuses it as
// before:
//   * the path contains ...\WindowsApps\<full name>, where the full name is
//     this process's own package identity (or, for a process without one, that
//     path component, which must then be a package Windows has installed);
//   * only that package root is redirected: every component of the real path
//     is plain -- no reparse point -- and it is the same file as `path`;
//   * Windows' own install location for that package covers the real path.
// `why`, when given, receives the first reason it was not resolved.
std::filesystem::path resolve_package_layout(const std::filesystem::path& path, std::string* why = nullptr);

// Exposed for tests: the Identity Name and Publisher from a manifest's text,
// with the few XML entities a DN can contain decoded. Empty when absent.
struct ManifestIdentity { std::wstring name, publisher; };
ManifestIdentity parse_manifest_identity(const std::string& text);
// The package family name for a name/publisher pair, through Windows' own
// PackageFamilyNameFromId (so the publisher-id hash is never reimplemented).
std::optional<std::wstring> package_family_name(const std::wstring& name, const std::wstring& publisher);
}
