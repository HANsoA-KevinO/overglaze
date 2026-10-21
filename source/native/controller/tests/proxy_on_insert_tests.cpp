// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The root proxy's two modes, executed on the built dxgi.dll.
//
// root_proxy_on_insert exists for one property: while the game starts, nothing
// Lab is loaded -- RE9 died with the host present from start-up and lived with
// every attach made once in play. This test loads the
// real proxy from a scratch game directory whose installation names that
// strategy and checks that the host is NOT loaded, that the worker is watching
// for Insert, and that the game's first factory is not held up waiting for a
// host that is not coming. The second mode (the installation names the
// ordinary root proxy) must still try to load the host at once.
//
// No key is ever pressed: synthesising Insert would reach whatever window has
// the focus on this machine. The press itself is exercised in the game.
//
// on-insert-configured: the installation names a data root holding the panel
// hotkey setting (settings\panel-hotkey-v1.json). The worker watches the
// configured binding, follows a change of the file within its two-second
// look, and falls back to Insert for a malformed file.
#include <windows.h>
#include <unknwn.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
struct LabProxyStatus {
    unsigned size,attempts,failures,fallback_calls,last_stage,host_stage;
    unsigned long last_error,host_last_error;
    unsigned resolved,host_loaded,compat_string_pending,compat_string_replayed;
    unsigned mode,insert_presses,insert_stage;
    unsigned watched_key,hotkey_source;
};
// lab::hotkey::pack: vk, then Ctrl 0x100, Shift 0x200, Alt 0x400.
constexpr unsigned kInsert=VK_INSERT,kCtrlF10=VK_F10|0x100u,kShiftF9=VK_F9|0x200u;
std::string escaped(const std::wstring& path){std::string out;for(const auto c:path){if(c>0x7F)throw std::runtime_error("scratch path must be ASCII for this test");if(c==L'\\')out+="\\\\";else out+=char(c);}return out;}
void put(const std::filesystem::path& file,const std::string& text){std::ofstream f(file,std::ios::binary|std::ios::trunc);f<<text;}
std::string hotkey_document(const char* key){return std::string("{\n  \"kind\": \"overglaze-panel-hotkey\",\n  \"version\": 1,\n  \"key\": \"")+key+"\"\n}\n";}
constexpr GUID factory1={0x770aae78,0xf26f,0x4dba,{0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87}};
}

int wmain(int argc,wchar_t** argv){
    try{
        need(argc==4,"usage: lab_proxy_on_insert_tests <built dxgi.dll> <scratch dir> on-insert|on-insert-configured|at-first-factory");
        const std::filesystem::path built=argv[1],scratch=argv[2];const std::wstring which=argv[3];
        const bool configured=which==L"on-insert-configured",on_insert=which==L"on-insert"||configured;
        need(on_insert||which==L"at-first-factory","unknown mode");
        std::error_code ignored;std::filesystem::remove_all(scratch,ignored);
        std::filesystem::create_directories(scratch/"overglaze");
        std::filesystem::copy_file(built,scratch/"dxgi.dll");
        // Only the strategy value matters to the proxy; the host re-validates the
        // whole installation itself. No host DLL is placed: at-first-factory must
        // try to load it (and fail with stage 4), on-insert must never try.
        const auto data=std::filesystem::absolute(scratch/"data");
        {std::ofstream j(scratch/"overglaze"/"overglaze.install.json",std::ios::binary);
         j<<R"({"loader":{"strategy":")"<<(on_insert?"root_proxy_on_insert":"root_proxy_d3d12")<<R"(","basename":"overglaze_controller.dll","subdir":"overglaze"})";
         if(configured)j<<R"(,"output_root":")"<<escaped(data.lexically_normal().wstring())<<R"(")";
         j<<"}";}
        if(configured){std::filesystem::create_directories(data/"settings");put(data/"settings"/"panel-hotkey-v1.json",hotkey_document("Ctrl+F10"));}
        const auto proxy=LoadLibraryW((scratch/"dxgi.dll").c_str());
        need(proxy!=nullptr,"load the proxy from the scratch game directory");
        const auto read=reinterpret_cast<void(__cdecl*)(LabProxyStatus*)>(GetProcAddress(proxy,"LabDXGIProxyStatus"));
        need(read!=nullptr,"proxy status export");
        auto status=[&]{LabProxyStatus s{};s.size=sizeof(s);read(&s);return s;};
        for(unsigned i=0;i<200&&status().mode==0;++i)Sleep(10);
        need(status().mode==(on_insert?2u:1u),"the worker chose the mode the installation names");
        if(!on_insert)for(unsigned i=0;i<300&&status().host_stage==0;++i)Sleep(10); // the load attempt, as in a game's start-up

        // The game's first factory, through the proxy's own export.
        const auto create=reinterpret_cast<HRESULT(WINAPI*)(const GUID&,void**)>(GetProcAddress(proxy,"CreateDXGIFactory1"));
        need(create!=nullptr,"CreateDXGIFactory1 export");
        void* factory=nullptr;const auto started=std::chrono::steady_clock::now();
        const HRESULT made=create(factory1,&factory);
        const auto waited=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        need(SUCCEEDED(made)&&factory!=nullptr,"the factory is created and forwarded");
        static_cast<IUnknown*>(factory)->Release();
        need(status().resolved==1,"the system dxgi.dll was resolved");

        if(on_insert){
            need(waited<1000,"the first factory does not wait for a host that is not coming ("+std::to_string(waited)+" ms)");
            Sleep(300);
            const auto s=status();
            need(s.host_loaded==0&&s.host_stage==0,"nothing Lab is loaded while the game starts");
            need(s.insert_stage==2&&s.insert_presses==0,"the worker is watching for its hotkey and has seen none");
            need(GetModuleHandleW(L"overglaze_controller.dll")==nullptr,"no host module in the process");
            if(!configured)need(s.watched_key==kInsert&&s.hotkey_source==4,"no data root in the installation: Insert");
            else{
                need(s.watched_key==kCtrlF10&&s.hotkey_source==2,"the configured binding from the data root's settings file");
                const auto follows=[&](unsigned key,unsigned source){for(unsigned i=0;i<800;++i){const auto now=status();if(now.watched_key==key&&now.hotkey_source==source)return true;Sleep(10);}return false;};
                put(data/"settings"/"panel-hotkey-v1.json",hotkey_document("shift+f9"));
                need(follows(kShiftF9,2),"a changed file is followed within the two-second look");
                put(data/"settings"/"panel-hotkey-v1.json",hotkey_document("A")); // a refused binding is malformed
                need(follows(kInsert,3),"a malformed file means Insert");
                std::filesystem::remove(data/"settings"/"panel-hotkey-v1.json");
                need(follows(kInsert,1),"no file means Insert");
            }
        }else{
            for(unsigned i=0;i<300&&status().host_stage==0;++i)Sleep(10);
            const auto s=status();
            need(s.host_stage==4&&s.host_loaded==0,"the ordinary root proxy tries the host at once (absent here: stage 4)");
            need(s.insert_stage==0,"and never watches for Insert");
            need(waited<1000,"a host that failed to load does not hold up the factory either ("+std::to_string(waited)+" ms)");
        }
        std::cout<<"PASS "<<checks<<" root-proxy "<<(configured?"on-Insert (configured hotkey)":on_insert?"on-Insert":"at-first-factory")<<" checks; first factory took "<<waited<<" ms\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
