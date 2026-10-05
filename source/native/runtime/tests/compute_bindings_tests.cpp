// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_compute_bindings.hpp"
#include <iostream>
#include <vector>
using B=lab::ComputeBindings;
namespace {
unsigned checks=0;
void need(bool b){++checks;if(!b)throw std::runtime_error("Compute binding assertion failed");}
template<class F> void reject(F f){bool caught=false;try{f();}catch(const std::logic_error&){caught=true;}need(caught);}
template<class T> T* ptr(UINT64 n){return reinterpret_cast<T*>(n);}
struct Sink {
    std::vector<unsigned> order,values;std::vector<UINT64> addresses;
    void heaps(UINT n,ID3D12DescriptorHeap* const*){order.push_back(10);need(n==1);}
    void root(ID3D12RootSignature*){order.push_back(11);}
    void pipeline(ID3D12PipelineState*){order.push_back(12);}
    void constants(UINT i,UINT n,const UINT* p){order.push_back(i);values.assign(p,p+n);}
    void table(UINT i,D3D12_GPU_DESCRIPTOR_HANDLE p){order.push_back(i);addresses.push_back(p.ptr);}
    void descriptor(UINT i,B::Kind,UINT64 p){order.push_back(i);addresses.push_back(p);}
};
}
int main(){try{
    auto* list=ptr<ID3D12GraphicsCommandList>(0x1000);auto* root=ptr<ID3D12RootSignature>(0x2000);
    auto* pso=ptr<ID3D12PipelineState>(0x3000);ID3D12DescriptorHeap* heaps[]{ptr<ID3D12DescriptorHeap>(0x4000)};
    const B::Parameter layout[]{{B::Kind::table,0},{B::Kind::constants,4},{B::Kind::cbv,0},{B::Kind::srv,0},{B::Kind::uav,0}};
    B b;need(!b.ready(list,1));b.reset(list,1);b.pipeline(pso);b.root(root,layout);b.heaps(heaps);
    b.table(0,{0x9000});b.descriptor(2,B::Kind::cbv,0x10000);b.descriptor(3,B::Kind::srv,0x20000);b.descriptor(4,B::Kind::uav,0x30000);
    const UINT first[]{7,0},last[]{19,23},patch[]{13};b.constants(1,0,first);need(!b.ready(list,1));
    Sink empty;reject([&]{b.replay(empty,list,1);});need(empty.order.empty());
    b.constants(1,2,last);b.constants(1,1,patch);need(b.ready(list,1));
    b.root(root,layout);b.heaps(heaps);need(b.ready(list,1)); // Same binds preserve values.
    Sink sink;b.replay(sink,list,1);need(sink.order==std::vector<unsigned>({10,11,12,0,1,2,3,4}));
    need(sink.values==std::vector<unsigned>({7,13,19,23}));need(sink.addresses==std::vector<UINT64>({0x9000,0x10000,0x20000,0x30000}));
    need(!b.ready(list,2)&&!b.ready(ptr<ID3D12GraphicsCommandList>(0x1100),1));
    auto copy=b;ID3D12DescriptorHeap* other[]{ptr<ID3D12DescriptorHeap>(0x5000)};copy.heaps(other);need(!copy.ready(list,1));
    copy.table(0,{0x9100});need(copy.ready(list,1));copy.root(ptr<ID3D12RootSignature>(0x2100),layout);need(!copy.ready(list,1));
    copy=b;copy.invalidate();need(!copy.ready(list,1));reject([&]{copy.pipeline(pso);});
    copy=b;reject([&]{copy.constants(1,3,last);});need(!copy.ready(list,1));
    copy=b;reject([&]{copy.table(1,{1});});copy=b;reject([&]{copy.descriptor(2,B::Kind::cbv,17);});
    copy=b;reject([&]{copy.reset(list,1);});need(!copy.ready(list,1));copy.reset(list,2);need(!copy.ready(list,2));
    copy=b;B::Parameter oversized[]{{B::Kind::constants,64},{B::Kind::table,0}};
    reject([&]{copy.root(root,oversized);});
    copy=b;const B::Parameter changed[]{{B::Kind::constants,1}};reject([&]{copy.root(root,changed);});
    copy=b;reject([&]{copy.heaps({});copy.table(0,{1});});
    copy=b;ID3D12DescriptorHeap* duplicates[]{heaps[0],heaps[0]};reject([&]{copy.heaps(duplicates);});
    copy=b;reject([&]{copy.constants(1,0,{});});
    // Zero is a known constant, unlike a word which was never observed.
    copy=b;const UINT zero[]{0};copy.constants(1,1,zero);Sink zs;copy.replay(zs,list,1);need(zs.values[1]==0);
    std::cout<<"compute_bindings checks="<<checks<<" passed; synthetic pointers only, no native hooks/GPU\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
