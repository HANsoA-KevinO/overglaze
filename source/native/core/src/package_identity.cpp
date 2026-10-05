// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_package_identity.hpp"
#include "lab_platform.hpp"
#include "lab_windows_path.hpp"
#include <windows.h>
#include <appmodel.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>

namespace lab {
namespace {
namespace fs = std::filesystem;

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream s; s << f.rdbuf();
    std::string bytes = s.str();
    if (bytes.size() > 1024 * 1024) return {};  // a manifest is small; refuse anything else
    // UTF-16LE with BOM -> UTF-8; strip a UTF-8 BOM.
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::wstring w(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
        return utf8(w);
    }
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
    return bytes;
}

std::string decode_entities(std::string s) {
    const std::pair<const char*, const char*> map[]{{"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
    for (const auto& [from, to] : map)
        for (std::size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += std::char_traits<char>::length(to))
            s.replace(at, std::char_traits<char>::length(from), to);
    return s;
}

std::wstring fold(std::wstring s) {
    for (auto& c : s) { if (c == L'/') c = L'\\'; c = static_cast<wchar_t>(std::towlower(c)); }
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}

// `inner` lies inside `outer` (or is it), compared on resolved, case-folded paths.
bool contains(const fs::path& outer, const fs::path& inner) {
    std::error_code ec;
    const auto a = fold(fs::weakly_canonical(outer, ec).wstring());
    const auto b = fold(fs::weakly_canonical(inner, ec).wstring());
    if (a.empty() || b.empty()) return false;
    return b == a || (b.size() > a.size() && b.compare(0, a.size(), a) == 0 && b[a.size()] == L'\\');
}

std::vector<std::wstring> installed_full_names(const std::wstring& family) {
    UINT32 count = 0, length = 0;
    const UINT32 filter = PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT;
    auto status = FindPackagesByPackageFamily(family.c_str(), filter, &count, nullptr, &length, nullptr, nullptr);
    if (status != ERROR_INSUFFICIENT_BUFFER || !count) return {};
    std::vector<PWSTR> names(count);
    std::vector<wchar_t> buffer(length);
    status = FindPackagesByPackageFamily(family.c_str(), filter, &count, names.data(), &length, buffer.data(), nullptr);
    if (status != ERROR_SUCCESS) return {};
    return {names.begin(), names.begin() + count};
}

std::optional<std::wstring> install_path(const std::wstring& full_name) {
    UINT32 length = 0;
    if (GetPackagePathByFullName(full_name.c_str(), &length, nullptr) != ERROR_INSUFFICIENT_BUFFER || !length) return std::nullopt;
    std::wstring path(length, L'\0');
    if (GetPackagePathByFullName(full_name.c_str(), &length, path.data()) != ERROR_SUCCESS) return std::nullopt;
    path.resize(wcslen(path.c_str()));
    return path;
}

std::string package_version(const std::wstring& full_name) {
    UINT32 length = 0;
    if (PackageIdFromFullName(full_name.c_str(), PACKAGE_INFORMATION_BASIC, &length, nullptr) != ERROR_INSUFFICIENT_BUFFER) return {};
    std::vector<BYTE> buffer(length);
    if (PackageIdFromFullName(full_name.c_str(), PACKAGE_INFORMATION_BASIC, &length, buffer.data()) != ERROR_SUCCESS) return {};
    const auto* id = reinterpret_cast<const PACKAGE_ID*>(buffer.data());
    const auto& v = id->version;
    return std::to_string(v.Major) + "." + std::to_string(v.Minor) + "." + std::to_string(v.Build) + "." + std::to_string(v.Revision);
}

void note(std::string* why, std::string text) { if (why && why->empty()) *why = std::move(text); }
}  // namespace

ManifestIdentity parse_manifest_identity(const std::string& text) {
    ManifestIdentity out;
    std::smatch element;
    static const std::regex identity(R"(<\s*Identity\b([^>]*)>)", std::regex::icase);
    if (!std::regex_search(text, element, identity)) return out;
    const std::string attributes = element[1].str();
    const auto attribute = [&](const char* name) -> std::wstring {
        std::smatch m;
        const std::regex re(std::string(R"(\b)") + name + R"re(\s*=\s*"([^"]*)")re", std::regex::icase);
        return std::regex_search(attributes, m, re) ? wide(decode_entities(m[1].str())) : std::wstring{};
    };
    out.name = attribute("Name");
    out.publisher = attribute("Publisher");
    return out;
}

std::optional<std::wstring> package_family_name(const std::wstring& name, const std::wstring& publisher) {
    if (name.empty() || publisher.empty()) return std::nullopt;
    PACKAGE_ID id{};
    id.name = const_cast<PWSTR>(name.c_str());
    id.publisher = const_cast<PWSTR>(publisher.c_str());
    id.processorArchitecture = PROCESSOR_ARCHITECTURE_NEUTRAL;  // the family ignores architecture and version
    UINT32 length = PACKAGE_FAMILY_NAME_MAX_LENGTH + 1;
    std::wstring family(length, L'\0');
    if (PackageFamilyNameFromId(&id, &length, family.data()) != ERROR_SUCCESS) return std::nullopt;
    family.resize(wcslen(family.c_str()));
    return family;
}

std::optional<PackageIdentity> installed_package_covering(const std::filesystem::path& exe, std::string* why) {
    std::error_code ec;
    fs::path dir = exe.parent_path();
    bool any_manifest = false;
    for (unsigned up = 0; up < 4 && !dir.empty(); ++up, dir = dir.parent_path()) {
        for (const auto* file : {L"MicrosoftGame.config", L"AppxManifest.xml"}) {
            const auto manifest = dir / file;
            if (!fs::is_regular_file(manifest, ec)) continue;
            any_manifest = true;
            const auto lead = parse_manifest_identity(read_text(manifest));
            if (lead.name.empty() || lead.publisher.empty()) { note(why, "manifest has no Identity Name/Publisher: " + utf8(manifest.wstring())); continue; }
            const auto family = package_family_name(lead.name, lead.publisher);
            if (!family) { note(why, "Windows refused the family for " + utf8(lead.name)); continue; }
            const auto names = installed_full_names(*family);
            if (names.empty()) { note(why, "no installed package of family " + utf8(*family)); continue; }
            for (const auto& full : names) {
                const auto path = install_path(full);
                if (!path) { note(why, "installed package has no path: " + utf8(full)); continue; }
                if (!contains(*path, exe)) { note(why, "installed package " + utf8(full) + " lives at " + utf8(*path) + ", which does not contain the executable"); continue; }
                return PackageIdentity{full, *family, *path, package_version(full)};
            }
        }
        if (dir == dir.root_path()) break;
    }
    if (!any_manifest) note(why, "no MicrosoftGame.config or AppxManifest.xml within four levels of the executable");
    return std::nullopt;
}

namespace {
// The real path of an existing file or directory, without the \\?\ prefix.
std::optional<fs::path> final_path(const fs::path& p) {
    const HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    std::wstring buffer(32768, L'\0');
    const auto n = GetFinalPathNameByHandleW(h, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= buffer.size()) return std::nullopt;
    buffer.resize(n);
    if (buffer.rfind(L"\\\\?\\UNC\\", 0) == 0) return std::nullopt;  // a network location is never a package root
    if (buffer.rfind(L"\\\\?\\", 0) == 0) buffer.erase(0, 4);
    return fs::path(buffer).lexically_normal();
}
std::optional<std::wstring> current_package_full_name() {
    UINT32 length = 0;
    if (GetCurrentPackageFullName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER || !length) return std::nullopt;
    std::wstring name(length, L'\0');
    if (GetCurrentPackageFullName(&length, name.data()) != ERROR_SUCCESS) return std::nullopt;
    name.resize(wcslen(name.c_str()));
    return name;
}
}  // namespace

fs::path resolve_package_layout(const fs::path& path, std::string* why) {
    const std::vector<fs::path> parts(path.begin(), path.end());
    std::size_t at = parts.size();
    for (std::size_t i = 1; i + 1 < parts.size(); ++i)
        if (fold(parts[i - 1].wstring()) == L"windowsapps") { at = i; break; }
    if (at == parts.size()) { note(why, "not inside an OS package layout (no ...\\WindowsApps\\<package>\\)"); return path; }
    const std::wstring component = parts[at].wstring();
    std::wstring full;
    if (const auto own = current_package_full_name()) {
        if (fold(*own) != fold(component)) { note(why, "WindowsApps component " + utf8(component) + " is not this process's package " + utf8(*own)); return path; }
        full = *own;
    } else full = component;
    const auto installed = install_path(full);
    if (!installed) { note(why, "Windows has no installed package " + utf8(full)); return path; }
    fs::path root, relative;
    for (std::size_t i = 0; i <= at; ++i) root /= parts[i];
    for (std::size_t i = at + 1; i < parts.size(); ++i) relative /= parts[i];
    const auto real_root = final_path(root);
    if (!real_root) { note(why, "package root " + utf8(root.wstring()) + " does not resolve"); return path; }
    const auto real = (*real_root / relative).lexically_normal();
    try { winpath::require_no_reparse(real); }
    catch (const std::exception&) { note(why, "the real path " + utf8(real.wstring()) + " is not plain: only the package root may be redirected"); return path; }
    std::error_code ec;
    if (!fs::equivalent(real, path, ec) || ec) { note(why, "the real path is not the same file"); return path; }
    if (!contains(*installed, real)) { note(why, "package " + utf8(full) + " is installed at " + utf8(*installed) + ", which does not cover " + utf8(real.wstring())); return path; }
    return real;
}

std::string game_identity_token(const std::filesystem::path& exe) {
    if (const auto package = installed_package_covering(exe)) return "package:" + utf8(package->full_name);
    return sha256(exe);
}

bool is_package_token(const std::string& token) { return token.rfind("package:", 0) == 0; }

json game_executable_identity() {
    std::wstring path(32768, L'\0');
    const auto n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) return module_identity(nullptr);
    path.resize(n);
    if (const auto package = installed_package_covering(path))
        return {{"path", utf8(path)}, {"sha256", "package:" + utf8(package->full_name)},
                {"version", package->version}, {"signature", "os-package"}, {"base", 0},
                {"identity", "installed OS package; the executable itself is not read"}};
    return module_identity(nullptr);
}

bool valid_identity_token(const std::string& token) {
    if (token.size() == 64)
        return std::all_of(token.begin(), token.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    if (!is_package_token(token) || token.size() <= 8 || token.size() > 8 + 127) return false;
    return std::all_of(token.begin() + 8, token.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}
}  // namespace lab
