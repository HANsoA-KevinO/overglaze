// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
namespace lab {
// This only validates an explicitly requested test-host label. It never
// authorizes hooks, enables NR, or upgrades evidence from synthetic to game.
// The data root is the caller's (host contract V4: no compiled machine path here).
inline bool valid_reshade_fixture_identity(const json& identity,const std::string& expected_hash,const std::filesystem::path& data_root) {
    if(!identity.is_object() || !identity.contains("path") || !identity["path"].is_string()
       || !identity.contains("sha256") || !identity["sha256"].is_string() || expected_hash.size()!=64)return false;
    for(char c:expected_hash)if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return false;
    if(identity["sha256"]!=expected_hash)return false;
    const auto path=std::filesystem::path(wide(identity["path"].get<std::string>()));
    if(!path.is_absolute() || path.lexically_normal()!=path)return false;
    const auto text=path.wstring();if(!data_root.is_absolute() || data_root.lexically_normal()!=data_root || !data_root.has_relative_path())return false;
    const std::wstring prefix=data_root.wstring()+L"\\";
    if(text.size()<=prefix.size() || _wcsnicmp(text.c_str(),prefix.c_str(),prefix.size())!=0)return false;
    // Require a child fixture directory, independently of the root's name.
    return _wcsicmp(path.filename().c_str(),L"lab_reshade_present_harness.exe")==0 && text.substr(prefix.size()).find(L'\\')!=std::wstring::npos;
}
}
