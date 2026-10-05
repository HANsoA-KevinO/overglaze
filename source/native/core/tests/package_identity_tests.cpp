// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// OS package identity (Xbox app / GDK titles) and the module-search layouts.
// Deterministic: no game, no installed-package assumption beyond Windows' own
// publisher-id algorithm, which is checked against Microsoft's published id.
#include "lab_package_identity.hpp"
#include "lab_windows_path.hpp"
#include <windows.h>
#include <appmodel.h>
#include <vector>
#include "lab_module_search.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
int checks = 0;
void need(bool ok, const std::string& why) { ++checks; if (!ok) throw std::runtime_error(why); }
void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary); f << text;
    if (!f) throw std::runtime_error("fixture write " + p.string());
}
}

int main() try {
    // ---- manifest parsing: attributes in any order, entities decoded
    {
        const auto id = lab::parse_manifest_identity(
            "<?xml version=\"1.0\"?><Game><Identity Version=\"1.2.3.0\" Publisher=\"CN=A &amp; B, O=&quot;X&quot;\" Name=\"Foo.Bar\"/></Game>");
        need(id.name == L"Foo.Bar", "Identity Name parsed");
        need(id.publisher == L"CN=A & B, O=\"X\"", "Identity Publisher parsed with entities decoded");
        const auto none = lab::parse_manifest_identity("<Game><NotIdentity Name=\"x\"/></Game>");
        need(none.name.empty() && none.publisher.empty(), "no Identity element -> nothing");
    }
    // ---- the family name comes from Windows, checked against Microsoft's published publisher id
    {
        const auto family = lab::package_family_name(L"Microsoft.WindowsCalculator",
            L"CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US");
        need(family && *family == L"Microsoft.WindowsCalculator_8wekyb3d8bbwe", "family name matches Microsoft's published id");
        need(!lab::package_family_name(L"", L"CN=X"), "empty name refused");
    }
    // ---- identity tokens
    {
        need(lab::valid_identity_token(std::string(64, 'a')), "lowercase SHA-256 is a token");
        need(!lab::valid_identity_token(std::string(64, 'A')), "uppercase hex is not");
        need(!lab::valid_identity_token(std::string(63, 'a')), "short hex is not");
        need(lab::valid_identity_token("package:Microsoft.Superb_1.0.7.0_x64__8wekyb3d8bbwe"), "package full name is a token");
        need(!lab::valid_identity_token("package:"), "empty package token is not");
        need(!lab::valid_identity_token("package:bad name"), "a space is not a package character");
        need(lab::is_package_token("package:x") && !lab::is_package_token(std::string(64, 'a')), "package tokens are recognised");
    }
    const auto root = fs::temp_directory_path() / (L"lab-package-identity-" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root);
    // ---- a plain Steam-like directory keeps its SHA-256, byte for byte
    {
        put(root / L"steam" / L"Game.exe", "not really a PE, only bytes to hash");
        const auto exe = root / L"steam" / L"Game.exe";
        need(lab::game_identity_token(exe) == lab::sha256(exe), "no manifest -> the EXE SHA-256, as always");
        std::string why;
        need(!lab::installed_package_covering(exe, &why) && why.find("no MicrosoftGame.config") != std::string::npos,
             "no manifest is reported as such");
    }
    // ---- a manifest naming a package Windows has not installed identifies nothing
    {
        put(root / L"fake" / L"Content" / L"MicrosoftGame.config",
            "<Game><Identity Name=\"Lab.NotInstalled.Anywhere\" Publisher=\"CN=Lab Test\" Version=\"1.0.0.0\"/></Game>");
        put(root / L"fake" / L"Content" / L"Game.exe", "bytes");
        const auto exe = root / L"fake" / L"Content" / L"Game.exe";
        std::string why;
        need(!lab::installed_package_covering(exe, &why), "an uninstalled package covers nothing");
        need(why.find("no installed package of family") != std::string::npos, "the reason names the missing family: " + why);
        need(lab::game_identity_token(exe) == lab::sha256(exe), "and the identity falls back to the SHA-256");
    }
    // ---- module search: every layout met so far, EXE directory first
    {
        const auto content = root / L"layouts" / L"Content";
        const auto exe_dir = content / L"Sandfall" / L"Binaries" / L"WinGDK";          // UE Xbox app build
        put(exe_dir / L"Game-WinGDK-Shipping.exe", "x");
        put(content / L"Sandfall" / L"Plugins" / L"NVIDIA" / L"DLSS" / L"Binaries" / L"ThirdParty" / L"Win64" / L"nvngx_dlss.dll", "project plugin");
        put(content / L"Engine" / L"Plugins" / L"Runtime" / L"Nvidia" / L"StreamlineCore" / L"Binaries" / L"ThirdParty" / L"WinGDK" / L"sl.interposer.dll", "three levels, WinGDK");
        const auto dirs = lab::module_directories(exe_dir);
        need(!dirs.empty() && dirs.front() == exe_dir, "the EXE directory is searched first");
        need(!lab::find_module(dirs, L"nvngx_dlss.dll").empty(), "project-level Plugins are searched (Clair Obscur)");
        need(!lab::find_module(dirs, L"sl.interposer.dll").empty(), "three plugin levels and WinGDK are searched (Hellblade 2)");
        const auto idtech = root / L"idtech" / L"Content";
        put(idtech / L"Game.exe", "x");
        put(idtech / L"streamline" / L"production" / L"sl.dlss_d.dll", "id Tech 8");
        put(idtech / L"streamline" / L"nvngx_dlssd.dll", "id Tech");
        const auto id_dirs = lab::module_directories(idtech);
        need(!lab::find_module(id_dirs, L"sl.dlss_d.dll").empty(), "<exe>\\streamline\\production is searched (DOOM)");
        need(!lab::find_module(id_dirs, L"nvngx_dlssd.dll").empty(), "<exe>\\streamline is searched (Indiana Jones)");
        put(idtech / L"sl.dlss_d.dll", "beside the exe");
        need(lab::find_module(lab::module_directories(idtech), L"sl.dlss_d.dll") == idtech / L"sl.dlss_d.dll",
             "a module beside the EXE wins over any other copy");
    }
    // ---- the OS package layout (WindowsApps junctions) is resolved only as documented
    {
        std::string why;
        const auto plain_exe = root / L"steam" / L"Game.exe";
        need(lab::resolve_package_layout(plain_exe, &why) == plain_exe && why.find("not inside an OS package layout") != std::string::npos,
             "a path outside any package layout is returned unchanged");
        const auto fake = root / L"WindowsApps" / L"Lab.NotInstalled_1.0.0.0_x64__labtest00000" / L"Game.exe";
        put(fake, "bytes");why.clear();
        need(lab::resolve_package_layout(fake, &why) == fake && why.find("no installed package") != std::string::npos,
             "a WindowsApps folder naming a package Windows does not have is not resolved: " + why);
        // Positive case through a real installed package, when this machine has one
        // readable: <temp>\WindowsApps\<full name> as a junction to its install folder.
        UINT32 count = 0, length = 0;
        const wchar_t* family = L"Microsoft.WindowsCalculator_8wekyb3d8bbwe";
        std::wstring full;
        if (FindPackagesByPackageFamily(family, PACKAGE_FILTER_HEAD, &count, nullptr, &length, nullptr, nullptr) == ERROR_INSUFFICIENT_BUFFER && count) {
            std::vector<PWSTR> names(count);std::vector<wchar_t> buffer(length);
            if (FindPackagesByPackageFamily(family, PACKAGE_FILTER_HEAD, &count, names.data(), &length, buffer.data(), nullptr) == ERROR_SUCCESS) full = names[0];
        }
        UINT32 path_length = 0;std::wstring installed;
        if (!full.empty() && GetPackagePathByFullName(full.c_str(), &path_length, nullptr) == ERROR_INSUFFICIENT_BUFFER) {
            installed.resize(path_length);
            if (GetPackagePathByFullName(full.c_str(), &path_length, installed.data()) == ERROR_SUCCESS) installed.resize(wcslen(installed.c_str())); else installed.clear();
        }
        std::error_code ec;
        const auto manifest = fs::path(installed) / L"AppxManifest.xml";
        if (!installed.empty() && fs::is_regular_file(manifest, ec)) {
            const auto layout = root / L"layout" / L"WindowsApps";fs::create_directories(layout);
            const auto junction = layout / full;
            const std::wstring command = L"cmd /c mklink /J \"" + junction.wstring() + L"\" \"" + installed + L"\" >nul";
            need(_wsystem(command.c_str()) == 0 && fs::exists(junction / L"AppxManifest.xml"), "fixture junction to an installed package");
            why.clear();const auto through = junction / L"AppxManifest.xml";
            const auto real = lab::resolve_package_layout(through, &why);
            need(real != through && fs::equivalent(real, manifest),
                 "a junction named WindowsApps\\<installed package> resolves to the package's real files: " + why);
            fs::remove(junction, ec);  // removes the junction only, never its target
        } else std::cout << "note: no readable Calculator package; positive layout case skipped\n";
    }
    fs::remove_all(root);
    std::cout << "PASS " << checks << " package identity and module search checks\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL after " << checks << " checks: " << e.what() << "\n";
    return 1;
}
