// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <iostream>
#include <stdexcept>
struct Large{std::uint64_t a,b,c;};
// /EHsc assumes extern-C declarations do not throw unless explicitly marked.
// Typed declarations also let MSVC generate the actual hidden sret argument.
extern "C" double LabForward0(std::uint64_t,double,std::uint64_t,double,std::uint64_t,double) noexcept(false);
extern "C" Large LabForward19(std::uint64_t,double,std::uint64_t,std::uint64_t) noexcept(false);
namespace {
unsigned last=0;
double mixed(std::uint64_t a,double b,std::uint64_t c,double d,std::uint64_t e,double f){
    if(a!=0x1234567812345678ULL||b!=1.25||c!=0xaabbccddeeff0011ULL||d!=3.5||e!=0x9988776655443322ULL||f!=7.75)throw std::runtime_error("Forwarding corrupted ABI arguments");return b+d+f;
}
Large aggregate(std::uint64_t a,double b,std::uint64_t c,std::uint64_t d){return {a+static_cast<unsigned>(b),c,d};}
}
extern "C" void* LabDXGIResolve(unsigned index){last=index;return index==0?reinterpret_cast<void*>(&mixed):reinterpret_cast<void*>(&aggregate);}
int main(){try{
    const auto f=&LabForward0;
    if(f(0x1234567812345678ULL,1.25,0xaabbccddeeff0011ULL,3.5,0x9988776655443322ULL,7.75)!=12.5||last!=0)return 1;
    const auto g=&LabForward19;const auto v=g(4,3.,7,9);
    if(last!=19||v.a!=7||v.b!=7||v.c!=9)return 1;
    // Native exception unwinds through the tail-forwarded call unchanged.
    bool caught=false;try{f(0,0,0,0,0,0);}catch(const std::runtime_error&){caught=true;}
    if(!caught)return 1;std::cout<<"PASS GP/XMM/stack/sret/exception forwarding\n";return 0;
}catch(...){return 1;}}
