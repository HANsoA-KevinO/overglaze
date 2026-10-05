// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <mutex>

namespace lab::startup {
// Failed admission is terminal for this process, not a retry on every factory.
template<class Start,class Failed> void once(std::once_flag& flag,Start start,Failed failed){
    std::call_once(flag,[&]{
        try{start();}catch(const std::exception& e){failed(e.what());}
        catch(...){failed("Unknown startup exception");}
    });
}
json record(DWORD pid,const std::string& error,const std::string& exe,const std::string& host);
void save_new(const std::filesystem::path& path,const json& data);
// Called outside DllMain. One bounded record and one notification per process;
// disk I/O and the dialog both run off the game's factory/render thread.
void report_async(HMODULE module,const char* error) noexcept;
}
