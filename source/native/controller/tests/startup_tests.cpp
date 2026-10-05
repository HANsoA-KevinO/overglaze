// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_startup_failure.hpp"
#include "lab_windows_path.hpp"
#include "lab_launch_arguments.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
namespace {
void need(bool value,const char* why){if(!value)throw std::runtime_error(why);}
template<class F>void reject(F f){bool rejected=false;try{f();}catch(...){rejected=true;}need(rejected,"Invalid input accepted");}
}
int main(){try{
    for(const auto* command:{L"game.exe --overglaze-access-only",L"\"I:\\Games\\007 First Light\\game.exe\" \"--overglaze-access-only\""})
        need(lab::startup::access_only_argument(command),"Explicit diagnostic argument");
    for(const auto* command:{L"game.exe",L"game.exe --overglaze-access-only=1",L"game.exe --overglaze-access-only-extra",
        L"game.exe \"label --overglaze-access-only\"",L"--overglaze-access-only",L""})
        need(!lab::startup::access_only_argument(command),"No implicit diagnostic launch");
    need(!lab::startup::access_only_argument(nullptr),"Null command line dormant");
    need(lab::winpath::same_spelling(L"D:\\Games\\Cyberpunk2077.exe",L"d:/games/cyberpunk2077.exe"),"Windows spelling comparison");
    need(!lab::winpath::same_spelling(L"D:\\Games\\dxgi.dll",L"D:\\Other\\dxgi.dll"),"Different directory accepted");
    std::once_flag gate;std::atomic<int> starts=0,failures=0;std::vector<std::thread> threads;
    for(int i=0;i<16;++i)threads.emplace_back([&]{lab::startup::once(gate,[&]{++starts;throw std::runtime_error("Rejected");},[&](const char*)noexcept{++failures;});});
    for(auto& thread:threads)thread.join();need(starts==1&&failures==1,"Failed startup repeated across factories");
    std::once_flag success;int successful=0;for(int i=0;i<3;++i)lab::startup::once(success,[&]{++successful;},[](const char*)noexcept{});
    need(successful==1,"Successful startup repeated");
    // OVERGLAZE_TEST_TEMP when set, else the user's temp directory: none of this
    // needs the Lab data root (the in-game writer's own root is not exercised).
    const auto temp=[]{std::wstring v(32768,L'\0');auto n=GetEnvironmentVariableW(L"OVERGLAZE_TEST_TEMP",v.data(),DWORD(v.size()));
        if(!n||n>=v.size()){n=GetTempPathW(DWORD(v.size()),v.data());need(n&&n<v.size(),"temp path");}
        v.resize(n);return std::filesystem::canonical(std::filesystem::path(v));}();
    const auto folder=temp/("startup-unit-"+lab::uuid());
    need(std::filesystem::create_directory(folder),"Unique test folder");
    const auto original=folder/L"Identity.bin",different=folder/L"Other.bin",diagnostic=folder/L"failure.json";
    {std::ofstream(original)<<"one";std::ofstream(different)<<"two";}
    lab::winpath::require_same_file(original,folder/L"identity.BIN");
    reject([&]{lab::winpath::require_same_file(original,different);});
    reject([&]{lab::winpath::require_same_file(original,folder/L"missing.bin");});
    const auto data=lab::startup::record(123,std::string(50000,'x'),std::string(50000,'y'),std::string(50000,'z'));
    need(data.at("text_may_be_truncated")==true&&data.at("nr_started")==false&&data.at("capture_started")==false,"Failure record semantics");
    lab::startup::save_new(diagnostic,data);const auto hash=lab::sha256(diagnostic);
    need(std::filesystem::file_size(diagnostic)<=16384,"Unbounded failure record");
    reject([&]{lab::startup::save_new(diagnostic,{{"overwrite",true}});});
    need(lab::sha256(diagnostic)==hash,"Existing failure overwritten");
    reject([&]{lab::startup::save_new(folder/L"large.json",{{"huge",std::string(20000,'x')}});});
    need(!std::filesystem::exists(folder/L"large.json"),"Oversize failure left a file");
    reject([&]{lab::startup::save_new(folder/L"bad.txt",data);});
    const auto link=folder/L"linked.bin";
    if(CreateSymbolicLinkW(link.c_str(),original.c_str(),SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)){
        reject([&]{lab::winpath::require_no_reparse(link);});std::filesystem::remove(link);
    }else std::cout<<"SKIP symbolic-link creation unavailable; reparse guard not dynamically tested\n";
    // Only these three tiny, owned fixture files are removed, never recursively.
    std::filesystem::remove(original);std::filesystem::remove(different);std::filesystem::remove(diagnostic);std::filesystem::remove(folder);
    std::cout<<"PASS bounded no-overwrite diagnostics, terminal once, physical path identity; no dialog, no GPU\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
