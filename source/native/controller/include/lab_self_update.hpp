// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// Self-update of the desktop program: check GitHub for a newer release, verify
// its signed manifest, download the Setup and hand over to it.
//
// Out of game only. This library is linked by overglaze_viewer.exe and
// overglaze_games.exe and by nothing that runs in a game: the host, the
// bridge, the root proxy and the in-game panel never connect anywhere
// (test_controller_link_closure.py checks their import tables).
//
// What is read: the public release list of the project's GitHub repository and
// the assets of one release. What is sent: HTTPS GET requests with the standard
// headers, Accept and "User-Agent: Overglaze/<version>". Nothing about the
// user, their games or their files; no cookies, no credentials.
//
// Authenticity: the installer is not Authenticode-signed, so every installable
// update carries overglaze-update.json signed with the project's release key
// (ECDSA P-256 over SHA-256, the 64-byte r||s signature base64-encoded in
// overglaze-update.json.sig). The public key is compiled in (kReleaseKey) and
// official() is the only Endpoint any program builds: no file, environment
// variable or argument replaces it. The signature is checked over the exact
// bytes before anything in the manifest is read, and a Setup is started only
// when its size and SHA-256 equal the signed manifest's.
#include "lab_platform.hpp"
#include "lab_root_locator.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace lab::update {
// The release public key as a BCRYPT_ECCPUBLIC_BLOB ("ECS1", 32-byte X, Y).
// Also in the repository as assets/release-key/overglaze-release-p256.public.blob.
inline constexpr std::array<unsigned char,72> kReleaseKey{
    0x45,0x43,0x53,0x31,0x20,0x00,0x00,0x00,0x93,0x14,0x58,0xab,
    0xc2,0x8b,0x3c,0xbc,0xc9,0x99,0xf9,0x40,0x4b,0x08,0xae,0x6b,
    0x05,0x12,0xfa,0x64,0x95,0xda,0xf1,0x49,0xbe,0xf2,0x75,0x54,
    0xb9,0x02,0x3a,0xdf,0xa1,0x89,0x2d,0x7d,0x01,0x06,0xd2,0xc4,
    0x2a,0x1a,0x10,0xe0,0xd7,0x46,0xb2,0xba,0xb1,0x37,0x3f,0xcf,
    0x59,0x35,0x47,0xdc,0xb1,0xbb,0x03,0xd8,0x9b,0x71,0xde,0x29};
// SHA-256 of those 72 bytes: the fingerprint docs/INSTALLER.md publishes.
inline constexpr const char* kReleaseKeySha256="92148a47da4ce5ba8cf755a0e3bce14bbbfdd9fb79c3de1e1972fcaa13892e3c";
inline constexpr const char* kReleasesApi="https://api.github.com/repos/HANsoA-KevinO/overglaze/releases?per_page=20";
inline constexpr const char* kAssetPrefix="https://github.com/HANsoA-KevinO/overglaze/releases/download/";
inline constexpr const char* kPagePrefix="https://github.com/HANsoA-KevinO/overglaze/releases/";
inline constexpr const char* kReleasesPage="https://github.com/HANsoA-KevinO/overglaze/releases";
inline constexpr const char* kManifestAsset="overglaze-update.json";
inline constexpr const char* kSignatureAsset="overglaze-update.json.sig";
// The Inno Setup AppId (source/installer/Overglaze.iss); its uninstall entry is
// HKCU\...\Uninstall\<AppId>_is1. test_installer_package.py compares the two.
inline constexpr const wchar_t* kSetupAppId=L"{66B920DD-3026-4D7D-AC73-64E349940B95}";
// Bounds on what is read from the network.
inline constexpr std::uint64_t kReleasesLimit=4ULL<<20;
inline constexpr std::uint64_t kManifestLimit=64ULL<<10;
inline constexpr std::uint64_t kSignatureLimit=1024;
inline constexpr std::uint64_t kSetupLimit=1ULL<<30;

// Every refusal and failure, in words the desktop program shows as they are.
struct Error : std::runtime_error {using std::runtime_error::runtime_error;};
struct Cancelled : Error {Cancelled():Error("已取消"){}};

// ---- versions: SemVer 2.0 precedence, core[-prerelease], no build metadata.
struct Version {
    std::uint64_t major=0,minor=0,patch=0;
    std::vector<std::string> pre; // empty: a release, which outranks its previews
    std::string text() const;
};
std::optional<Version> parse_version(std::string_view);
std::optional<Version> parse_tag(std::string_view); // one optional leading 'v'
int compare(const Version&,const Version&);        // <0, 0, >0

// ---- signature
// One base64 line (an optional trailing newline) holding exactly 64 bytes.
std::optional<std::vector<unsigned char>> decode_signature(std::string_view file);
// ECDSA P-256 / SHA-256 over the exact bytes of `message`. Never throws.
bool verify(std::string_view message,std::string_view signature_file,std::span<const unsigned char> public_blob,std::string* why=nullptr);
std::string sha256_hex(std::string_view bytes);

// ---- the signed manifest (schema overglaze-update-v1)
struct FileRef {std::string name;std::uint64_t size=0;std::string sha256;};
struct Manifest {std::string version,channel,published,source_commit,notes_zh;FileRef setup,portable;};
// Only for bytes whose signature verified. Throws Error naming what is wrong.
Manifest parse_manifest(std::string_view verified_bytes);
// Size first, then SHA-256. Throws Error.
void verify_file(const std::filesystem::path&,const FileRef&);

// ---- where to look. official() is the only one a program builds; the tests
// build a loopback one with an ephemeral key.
struct Endpoint {
    std::string releases_api,asset_prefix,page_prefix,releases_page,user_agent;
    std::vector<unsigned char> release_key;
    bool loopback_test=false; // plain HTTP to 127.0.0.1 without a proxy: tests only
};
Endpoint official(const std::string& running_version);

// ---- the release list
struct Asset {std::string name,url;std::uint64_t size=0;};
struct Release {
    std::string tag,page;Version version;std::vector<Asset> assets;
    const Asset* asset(std::string_view name) const;
};
struct Selection {
    // none: nothing newer. manual_only: the newest is missing the manifest or
    // its signature, so it can only be downloaded by hand.
    enum class Kind {none,signed_update,manual_only};
    Kind kind=Kind::none;Release release;
};
// Drafts and unparsable entries are ignored; the highest version strictly
// newer than `running` is chosen. Asset and page URLs outside the endpoint's
// prefixes are dropped (a dropped manifest makes the release manual-only).
Selection select_release(const json& releases,const Version& running,const Endpoint&);

// ---- the check
using Cancel=std::atomic<bool>;
struct Offer {
    enum class State {up_to_date,available,manual_only};
    State state=State::up_to_date;
    std::string version,page; // the newer version; its release page, else the releases page
    Manifest manifest;        // available only: verified, version == tag > running
    Asset setup;              // available only: the release asset the manifest names
};
// Throws Error with the reason (network, GitHub, signature, manifest).
Offer check(const Endpoint&,const Version& running,const Cancel* cancel=nullptr);

// ---- download into <data root>\updates\<version>\ (marked as ours)
struct Progress {std::atomic<std::uint64_t> received{0},total{0};};
std::filesystem::path updates_dir(const std::filesystem::path& data_root);
// Free space required on the program's disk before downloading and installing.
std::uint64_t space_needed(std::uint64_t setup_size);
// Verified against the signed manifest (size and SHA-256) before it returns.
// Removes the other update folders this program created first.
std::filesystem::path download_setup(const Endpoint&,const Offer&,const std::filesystem::path& data_root,const Cancel* cancel=nullptr,Progress* progress=nullptr);
// Removes the update folders this program created (marker file inside),
// except `keep_version`; only the files it wrote, never anything else, and a
// folder only once it is empty. Returns the folders removed.
unsigned cleanup_downloads(const std::filesystem::path& data_root,const std::string& keep_version);
// The same, for the folders of versions up to `running`: already installed or older.
unsigned cleanup_installed(const std::filesystem::path& data_root,const Version& running);

// ---- this copy: installed by Setup, or portable
struct Installation {bool by_setup=false;std::filesystem::path root,data;std::string why;};
// InstallLocation of the Setup's uninstall entry; tests inject it.
using UninstallLocation=std::function<std::optional<std::wstring>()>;
// Installed by Setup only when all of these hold, else portable (why says
// which did not): the published layout (<root>\app\*.exe, no override file),
// data\settings\application-install.json naming this root, app\unins000.exe,
// and the HKCU uninstall entry of kSetupAppId pointing at this root.
Installation detect(const lab::root::Location&,const UninstallLocation& reader={});
Installation detect_self();
// Why the Setup must not be started now (empty: it may): not installed by
// Setup, the installer's own maintenance check (a registered game running,
// another Overglaze program running, an unfinished game operation), or too
// little disk space.
std::string install_refusal(const Installation&,std::uint64_t setup_size);
// "<setup>" /SILENT /SUPPRESSMSGBOXES /NORESTART /DIR="<root>" /WAITPID=<pid> /RELAUNCH
std::wstring setup_command_line(const std::filesystem::path& setup,const std::filesystem::path& root,DWORD pid);
// Holds the file against writes, verifies it once more and starts it.
void launch_setup(const std::filesystem::path& setup,const FileRef&,const std::filesystem::path& root,DWORD pid);

// ---- running version: the ProductVersion of overglaze_viewer.exe beside this
// program (the version Build-Installer.ps1 checked against the release).
std::optional<std::string> product_version(const std::filesystem::path& exe);
std::string running_version(); // throws Error
json offer_json(const Offer&,const std::string& running,const Installation&);

// ---- the desktop setting and the update record (data\settings\update-v1.json)
struct Settings {
    bool check_on_start=true;
    std::int64_t last_check=0;    // UTC seconds of the latest check attempt
    std::string last_result;      // shown under 设置 / 更新
    std::string pending_from,pending_to; // written just before the Setup starts
    json document() const;
    static Settings parse(const json&); // lenient: an invalid field keeps its default
};
inline constexpr std::int64_t kAutomaticInterval=24*60*60;
bool automatic_check_due(const Settings&,std::int64_t now);
// After a relaunch: the update arrived (running >= pending_to), or the
// program still runs the version it left (running == pending_from).
enum class AfterUpdate {none,updated,failed};
AfterUpdate after_update(const Settings&,const Version& running);
// Thread-safe; saved at once, atomically. Disabled (memory only) without a data root.
class SettingsStore {
public:
    explicit SettingsStore(std::filesystem::path data_root={});
    Settings get() const;
    void update(const std::function<void(Settings&)>&);
    std::string error() const;
private:
    mutable std::mutex lock_;std::filesystem::path file_;Settings settings_;std::string error_;
    void save_locked();
};
}
