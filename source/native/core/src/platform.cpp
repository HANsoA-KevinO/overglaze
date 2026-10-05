// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_platform.hpp"
#include <bcrypt.h>
#include <objbase.h>
#include <psapi.h>
#include <wintrust.h>
#include <softpub.h>
#include <fstream>
#include <array>
#include <iomanip>
#include <sstream>
#include <vector>

namespace lab {
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) throw std::runtime_error("Invalid UTF-16");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) throw std::runtime_error("Invalid UTF-8");
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}
std::string uuid() {
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) throw std::runtime_error("CoCreateGuid failed");
    wchar_t s[40]{}; StringFromGUID2(g, s, 40);
    return utf8(s);
}
std::wstring pipe_name(DWORD pid) { return L"\\\\.\\pipe\\Overglaze." + std::to_wstring(pid); }
std::string sha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read file for SHA-256");
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("SHA provider failed");
    auto cleanup = [&]() { if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); };
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) { cleanup(); throw std::runtime_error("SHA init failed"); }
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(input.gcount()), 0) < 0) {
            cleanup(); throw std::runtime_error("SHA update failed");
        }
    }
    if (!input.eof()) { cleanup(); throw std::runtime_error("File read failed during hashing"); }
    std::array<UCHAR, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) { cleanup(); throw std::runtime_error("SHA finish failed"); }
    cleanup();
    std::ostringstream out;
    for (auto b : digest) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    return out.str();
}
json module_identity(HMODULE module) {
    std::wstring path(32768, L'\0');
    auto count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    check(count && count < path.size(), "GetModuleFileNameW");
    path.resize(count);
    auto result = file_identity(path);
    result["base"] = reinterpret_cast<std::uintptr_t>(module);
    return result;
}
json file_identity(const std::filesystem::path& target) {
    const std::wstring path = target.wstring();
    json result = {{"path", utf8(path)}, {"sha256", sha256(path)}, {"version", ""}};
    DWORD unused = 0, size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size) {
        std::vector<BYTE> info(size); VS_FIXEDFILEINFO* fixed = nullptr; UINT length = 0;
        if (GetFileVersionInfoW(path.c_str(), 0, size, info.data()) && VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &length)
            && length >= sizeof(VS_FIXEDFILEINFO))
            result["version"] = std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionMS))
                + "." + std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionLS));
    }
    WINTRUST_FILE_INFO file{}; file.cbStruct = sizeof(file); file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{}; trust.cbStruct = sizeof(trust); trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE; trust.dwUnionChoice = WTD_CHOICE_FILE; trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY; trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(nullptr, &action, &trust);
    result["signature"] = status == ERROR_SUCCESS ? "ValidCachedTrust" : "Unverified";
    result["signature_status"] = status;
    trust.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &action, &trust);
    return result;
}
json current_modules() {
    DWORD bytes = 0;
    check(K32EnumProcessModules(GetCurrentProcess(), nullptr, 0, &bytes), "Module inventory size");
    std::vector<HMODULE> modules(bytes / sizeof(HMODULE) + 32);
    check(K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes), "Module inventory");
    if (bytes > modules.size() * sizeof(HMODULE)) throw std::runtime_error("Module set grew; retry snapshot");
    json result = json::array();
    for (size_t i = 0; i < bytes / sizeof(HMODULE); ++i) {
        // Hold a reference against unload while reading the identity.
        HMODULE held{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(modules[i]), &held)) continue;
        try { result.push_back(module_identity(held)); }
        catch (const std::exception& e) { result.push_back({{"base", reinterpret_cast<std::uintptr_t>(held)}, {"error", e.what()}}); }
        FreeLibrary(held);
    }
    return result;
}
}
