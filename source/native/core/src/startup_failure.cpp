// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_startup_failure.hpp"
#include "lab_installation.hpp"
#include "lab_windows_path.hpp"
#include <atomic>
#include <thread>

namespace lab::startup {
json record(DWORD pid,const std::string& error,const std::string& exe,const std::string& host){
    return {{"version",1},{"purpose","startup-diagnostic"},{"pid",pid},
        {"stage","standalone-host-startup"},{"error",error.substr(0,512)},
        {"executable_path",exe.substr(0,1024)},{"host_path",host.substr(0,1024)},
        {"text_may_be_truncated",error.size()>512||exe.size()>1024||host.size()>1024},
        {"nr_started",false},{"capture_started",false}};
}
void save_new(const std::filesystem::path& path,const json& data){
    if(!path.is_absolute()||path!=path.lexically_normal()||path.extension()!=L".json")
        throw std::runtime_error("Invalid startup diagnostic path");
    winpath::require_no_reparse(path.parent_path());
    const auto bytes=data.dump(2,' ',false,json::error_handler_t::replace);
    if(bytes.size()>16384)throw std::runtime_error("Startup diagnostic exceeds 16 KiB");
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    check(file.valid(),"Create startup diagnostic");DWORD written=0;
    check(WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size(),"Write startup diagnostic");
}
void report_async(HMODULE module,const char* error) noexcept{
    static std::atomic<bool> reported=false;if(reported.exchange(true))return;
    try{
        // Host contract V4: the program folder or its data root was deleted while
        // this game still carries our payload. That is the user removing the
        // product, not a fault: say nothing in the game, write nothing anywhere,
        // and let the proxy keep forwarding DXGI.
        if(installation_root_missing(module)){OutputDebugStringW(L"Overglaze: data root recorded at install is gone; standing down silently.\n");return;}
        const std::string reason=std::string(error?error:"Unknown startup failure").substr(0,512);
        // The diagnostic goes to the data root the receipt beside the host
        // records (V4), never to a compiled path. Without one (a fixture, an
        // unreadable receipt) there is no file, only the dialog.
        const auto root=recorded_output_root(module);
        std::thread([module,reason,root]{
            std::wstring message=L"釉光 · Overglaze 未能启动，本次 NR 控制不可用。\n请退出游戏后检查安装配置。\n\n";
            try{
                if(!root)throw std::runtime_error("no recorded data root");
                winpath::require_no_reparse(*root);
                const auto folder=*root/L"startup-errors";std::filesystem::create_directory(folder);
                winpath::require_no_reparse(folder);
                const auto path=folder/("startup-"+std::to_string(GetCurrentProcessId())+"-"+uuid()+".json");
                const auto data=record(GetCurrentProcessId(),reason,utf8(module_path(nullptr).wstring()),utf8(module_path(module).wstring()));
                save_new(path,data);
                message+=L"原因："+wide(data.at("error").get<std::string>())+L"\n\n诊断文件："+path.wstring();
            }catch(...){message+=L"原因："+wide(reason)+L"\n\n诊断文件未能保存。请检查数据目录和磁盘空间。";}
            OutputDebugStringW(message.c_str());
            MessageBoxW(nullptr,message.c_str(),L"釉光 · Overglaze — 启动失败",MB_OK|MB_ICONWARNING);
        }).detach();
    }catch(...){OutputDebugStringW(L"Overglaze: startup failed; notification worker unavailable.\n");}
}
}
