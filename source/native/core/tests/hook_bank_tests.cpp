// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_hook_bank.hpp"
#include <iostream>
#include <thread>
namespace {
lab::HookBank<2> hooks;
using Fn=int(int);
std::atomic<unsigned> calls[5][2]{};
std::atomic<unsigned> detours[4][2]{};
unsigned checks=0;bool throw_original=false;
void need(bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);}
template<unsigned B,unsigned S>__declspec(noinline) int target(int n){
    ++calls[B][S];if(throw_original)throw std::runtime_error("original");
    // The second implementation wraps the first: both originals must execute.
    if constexpr(B==1){Fn* volatile base=&target<0,S>;return base(n)+100;}
    return n+int(B*100+S*10);
}
template<unsigned B,unsigned S>__declspec(noinline) int callback(int n){
    ++detours[B][S];return hooks.original<Fn*>(B,S)(n);
}
template<unsigned B>lab::HookBank<2>::Entries targets(){return {reinterpret_cast<void*>(&target<B,0>),reinterpret_cast<void*>(&target<B,1>)};}
template<unsigned B>lab::HookBank<2>::Entries callbacks(){return {reinterpret_cast<void*>(&callback<B,0>),reinterpret_cast<void*>(&callback<B,1>)};}
int invoke(void* p,int n){Fn* volatile f=reinterpret_cast<Fn*>(p);return f(n);}
}
int main(){try{
    const lab::HookBank<2>::Detours entries{callbacks<0>(),callbacks<1>(),callbacks<2>(),callbacks<3>()};
    auto denied=[&](lab::HookBank<2>::Entries t,const char* why){const auto before=hooks.snapshot();bool caught=false;
        try{hooks.install(t,entries);}catch(const std::runtime_error&){caught=true;}
        need(caught&&hooks.snapshot()==before,why);};
    denied({targets<0>()[0],nullptr},"Invalid batch leaves all targets untouched");
    denied({targets<0>()[0],targets<0>()[0]},"Different ABI slots cannot alias");
    need(invoke(targets<0>()[0],7)==7&&detours[0][0]==0,"No modification after rejected batch");
    hooks.install(targets<0>(),entries);need(hooks.snapshot().size()==2,"First implementation installed");
    need(invoke(targets<0>()[0],7)==7&&invoke(targets<0>()[1],7)==17,"Arguments and return passed through");
    hooks.install(targets<0>(),entries);need(hooks.snapshot().size()==2,"Repeated implementation is deduplicated");
    hooks.install({targets<1>()[0],targets<0>()[1]},entries);
    need(hooks.snapshot().size()==3,"Partially shared vtable only installs new code");
    const auto a=calls[0][0].load(),b=calls[1][0].load(),d=detours[0][0].load();
    need(invoke(targets<1>()[0],13)==113&&calls[0][0]==a+1&&calls[1][0]==b+1&&detours[0][0]==d+1,
         "Nested wrapper routes each implementation to its own original exactly once");
    hooks.install(targets<1>(),entries);need(invoke(targets<1>()[1],13)==123,"Second method fills its own bank");
    std::atomic<bool> good=true;
    std::thread reader([&]{for(int i=0;i<2000;++i)if(invoke(targets<1>()[0],i)!=i+100)good=false;});
    hooks.install(targets<2>(),entries);reader.join();need(good,"Existing original routing remains stable during another install");
    need(invoke(targets<2>()[0],3)==203&&invoke(targets<2>()[1],3)==213,"Third implementation preserved");
    hooks.install(targets<3>(),entries);need(invoke(targets<3>()[0],5)==305,"Fourth bounded implementation");
    denied(targets<4>(),"Capacity refusal does not partially modify a fifth implementation");
    need(invoke(targets<4>()[0],5)==405&&hooks.snapshot().size()==8,"Unhooked implementation still behaves normally");
    denied({targets<0>()[1],targets<0>()[0]},"Cross-slot existing ABI alias refused");
    throw_original=true;bool caught=false;try{invoke(targets<1>()[0],0);}catch(const std::runtime_error& e){caught=std::string(e.what())=="original";}
    throw_original=false;need(caught,"Original C++ exception propagates through nested detours");
    hooks.uninstall_quiesced_for_test();const auto stopped=detours[0][0].load();
    need(invoke(targets<1>()[0],11)==111&&detours[0][0]==stopped,"Quiesced fixture removes only its own hooks");
    std::cout<<"PASS checks="<<checks<<" implementations=4 signatures=2 game=0 GPU=0 NR=0 files=0\n";return 0;
}catch(const std::exception& e){hooks.uninstall_quiesced_for_test();std::cerr<<e.what()<<'\n';return 1;}}
