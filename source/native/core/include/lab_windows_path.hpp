// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <filesystem>
#include <stdexcept>

namespace lab::winpath {
// Spelling only. Security-sensitive callers also verify the existing file's
// physical identity: case-sensitive NTFS directories must not bypass admission.
inline bool same_spelling(const std::filesystem::path& a,const std::filesystem::path& b){
    auto left=a,right=b;left.make_preferred();right.make_preferred();
    const auto& x=left.native();const auto& y=right.native();
    return x.size()<=32767&&y.size()<=32767&&
        CompareStringOrdinal(x.data(),static_cast<int>(x.size()),y.data(),static_cast<int>(y.size()),TRUE)==CSTR_EQUAL;
}
inline void require_no_reparse(std::filesystem::path path){
    for(;!path.empty();){const auto attributes=GetFileAttributesW(path.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Installation path missing or contains a reparse point");
        const auto parent=path.parent_path();if(parent==path)break;path=parent;
    }
}
inline void require_same_file(const std::filesystem::path& a,const std::filesystem::path& b){
    require_no_reparse(a);require_no_reparse(b);
    if(!same_spelling(a,b)||!std::filesystem::equivalent(a,b))
        throw std::runtime_error("Installation path does not identify the same physical file");
}
}
