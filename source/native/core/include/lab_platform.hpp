// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace lab {
using json = nlohmann::json;
class Handle {
public:
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (valid()) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};
std::string utf8(const std::wstring& text);
std::wstring wide(const std::string& text);
std::string uuid();
std::string sha256(const std::filesystem::path& path);
json module_identity(HMODULE module);
// Same identity (sha256, version, Authenticode state) for a file that is NOT loaded.
json file_identity(const std::filesystem::path& path);
json current_modules();
std::wstring pipe_name(DWORD pid);
inline void check(bool ok, const char* context) {
    if (!ok) throw std::runtime_error(std::string(context) + ": Win32=" + std::to_string(GetLastError()));
}
}
