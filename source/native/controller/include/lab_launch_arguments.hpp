// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string_view>
#include <stdexcept>

namespace lab::startup {
// Exact argv token, not substring matching. Read once on the startup worker.
inline bool access_only_argument(const wchar_t* command_line) {
    if(!command_line || !*command_line)return false;
    int count=0;auto** args=CommandLineToArgvW(command_line,&count);
    if(!args)throw std::runtime_error("Cannot parse diagnostic launch arguments");
    bool found=false;
    for(int i=1;i<count;++i)found|=std::wstring_view(args[i])==L"--overglaze-access-only";
    LocalFree(args);return found;
}
}
