// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "binding_state.hpp"
#include <iostream>
#include <stdexcept>

// Metadata-only identities: never dereferenced or sent to D3D12.
int main(){unsigned checks=0;try{
    using lab::host::BindingState;using lab::host::RootArguments;
    auto require=[&](bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);};
    auto* a=reinterpret_cast<ID3D12DescriptorHeap*>(0x1000);
    auto* b=reinterpret_cast<ID3D12DescriptorHeap*>(0x2000);
    auto* other=reinterpret_cast<ID3D12DescriptorHeap*>(0x3000);
    auto* root=reinterpret_cast<ID3D12RootSignature*>(0x4000);
    BindingState s;
    s.compute.signature(root);s.graphics.signature(root);
    ID3D12DescriptorHeap* ab[]{a,b};s.set_heaps(2,ab);
    auto bind=[&]{s.compute.address(3,RootArguments::table,0x1234);s.graphics.address(2,RootArguments::table,0x5678);};
    auto tables=[&]{return s.compute.args[3].kind==RootArguments::table&&s.graphics.args[2].kind==RootArguments::table;};
    bind();s.set_heaps(2,ab);require(tables(),"Same heap set preserves both table banks");
    ID3D12DescriptorHeap* ba[]{b,a};s.set_heaps(2,ba);
    require(tables(),"Reordered identical heap set must preserve both table banks");
    require(s.compute.args[3].address==0x1234&&s.graphics.args[2].address==0x5678,"Reorder preserves actual table addresses");
    for(unsigned i=0;i<16;++i){s.set_heaps(2,i%2?ba:ab);require(tables(),"Repeated permutation is not heap replacement");}
    ID3D12DescriptorHeap* changed[]{a,other};s.set_heaps(2,changed);require(!tables(),"Real heap replacement still invalidates tables");
    bind();s.set_heaps(1,&a);require(!tables(),"Removing a bound heap invalidates tables");
    bind();s.set_heaps(1,&a);require(tables(),"Single same heap preserves tables");
    s.set_heaps(0,nullptr);require(!tables(),"Unbind all heaps invalidates tables");
    BindingState invalid;ID3D12DescriptorHeap* duplicate[]{a,a};invalid.set_heaps(2,duplicate);
    require(invalid.invalid,"Duplicate heap identities are rejected");
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"gpu_work\":0,\"raw_files\":0}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
