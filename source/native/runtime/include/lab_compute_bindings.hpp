// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <array>
#include <bitset>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace lab {
// CPU-only shadow of one host recording. Caller feeds EVERY relevant setter in
// order, including Reset/ClearState. No native hooks or lifetime discovery here.
// Borrowed objects/heap contents MUST stay alive and unchanged through GPU use.
// This is COMPUTE binding restoration, not a full graphics state block.
class ComputeBindings final {
public:
    enum class Kind { table, constants, cbv, srv, uav };
    struct Parameter {Kind kind=Kind::table;unsigned words=0;};
    using Layout=std::span<const Parameter>;
private:
    struct Argument {
        Parameter parameter{};std::array<UINT,64> values{};std::bitset<64> known{};
        UINT64 address=0;bool assigned=false;
    };
    ID3D12GraphicsCommandList* list_=nullptr;
    std::uint64_t generation_=0;
    ID3D12PipelineState* pso_=nullptr;
    ID3D12RootSignature* root_=nullptr;
    std::array<ID3D12DescriptorHeap*,2> heaps_{};
    std::array<Argument,64> arguments_{};
    unsigned heap_count_=0,argument_count_=0;
    bool valid_=false,heaps_known_=false;
    void require(bool b){if(!b){valid_=false;throw std::logic_error("Incomplete/unsupported compute binding observation");}}
    Argument& arg(unsigned index,Kind kind){require(valid_ && root_ && index<argument_count_ && arguments_[index].parameter.kind==kind);return arguments_[index];}
public:
    void reset(ID3D12GraphicsCommandList* list,std::uint64_t generation) {
        // Generation comes from observed recording lifetime, not the pointer.
        if(!list || !generation || generation<=generation_){valid_=false;throw std::logic_error("Stale command recording");}
        *this=ComputeBindings{};list_=list;generation_=generation;valid_=true;
    }
    void invalidate() noexcept {valid_=false;}
    void pipeline(ID3D12PipelineState* pso){require(valid_ && pso);pso_=pso;}
    void root(ID3D12RootSignature* root,Layout layout) {
        require(valid_ && root && layout.size()<=arguments_.size());
        unsigned cost=0;for(const auto& p:layout){
            require(p.kind==Kind::table || p.kind==Kind::constants || p.kind==Kind::cbv || p.kind==Kind::srv || p.kind==Kind::uav);
            require(p.kind==Kind::constants ? p.words>0&&p.words<=64 : p.words==0);
            cost+=p.kind==Kind::constants?p.words:p.kind==Kind::table?1:2;
        }
        require(cost<=64);
        if(root_==root){
            require(layout.size()==argument_count_);
            for(unsigned i=0;i<argument_count_;++i)require(layout[i].kind==arguments_[i].parameter.kind && layout[i].words==arguments_[i].parameter.words);
            return; // Redundantly binding the same root does not invalidate args.
        }
        root_=root;arguments_={};argument_count_=static_cast<unsigned>(layout.size());
        for(unsigned i=0;i<argument_count_;++i)arguments_[i].parameter=layout[i];
    }
    void heaps(std::span<ID3D12DescriptorHeap* const> heaps) {
        require(valid_ && heaps.size()<=2);
        for(auto* h:heaps)require(h!=nullptr);
        if(heaps.size()==2)require(heaps[0]!=heaps[1]);
        bool changed=!heaps_known_ || heaps.size()!=heap_count_;
        for(unsigned i=0;i<heaps.size();++i)changed|=heaps_[i]!=heaps[i];
        heaps_={};heap_count_=static_cast<unsigned>(heaps.size());
        for(unsigned i=0;i<heap_count_;++i)heaps_[i]=heaps[i];
        heaps_known_=true;
        if(changed)for(auto& a:arguments_)if(a.parameter.kind==Kind::table)a.assigned=false;
    }
    void table(unsigned index,D3D12_GPU_DESCRIPTOR_HANDLE address) {
        auto& a=arg(index,Kind::table);require(heaps_known_ && heap_count_ && address.ptr);
        a.address=address.ptr;a.assigned=true;
    }
    void constants(unsigned index,unsigned offset,std::span<const UINT> values) {
        auto& a=arg(index,Kind::constants);
        require(offset<=a.parameter.words && values.size()<=a.parameter.words-offset && !values.empty());
        for(unsigned i=0;i<values.size();++i){a.values[offset+i]=values[i];a.known.set(offset+i);}
    }
    void descriptor(unsigned index,Kind kind,D3D12_GPU_VIRTUAL_ADDRESS address) {
        require(kind==Kind::cbv || kind==Kind::srv || kind==Kind::uav);
        auto& a=arg(index,kind);require(address && (kind!=Kind::cbv || address%256==0));a.address=address;a.assigned=true;
    }
    bool ready(ID3D12GraphicsCommandList* list,std::uint64_t generation) const noexcept {
        if(!valid_ || !list || list!=list_ || generation!=generation_ || !pso_ || !root_ || !heaps_known_)return false;
        for(unsigned i=0;i<argument_count_;++i){const auto& a=arguments_[i];
            if(a.parameter.kind==Kind::constants){for(unsigned j=0;j<a.parameter.words;++j)if(!a.known[j])return false;}
            else if(!a.assigned)return false;
        }
        return true;
    }
    // Replay through a typed sink so unit tests inspect exact order/values. The
    // snapshot is not fed our injected setters. No restore on a failed NR call.
    template<class Sink> void replay(Sink& sink,ID3D12GraphicsCommandList* list,std::uint64_t generation) const {
        if(!ready(list,generation))throw std::logic_error("Unsafe compute restore refused before recording");
        sink.heaps(heap_count_,heaps_.data());sink.root(root_);sink.pipeline(pso_);
        for(unsigned i=0;i<argument_count_;++i){const auto& a=arguments_[i];
            if(a.parameter.kind==Kind::constants)sink.constants(i,a.parameter.words,a.values.data());
            else if(a.parameter.kind==Kind::table)sink.table(i,{a.address});
            else sink.descriptor(i,a.parameter.kind,a.address);
        }
    }
    void restore(ID3D12GraphicsCommandList* list,std::uint64_t generation) const {
        struct NativeSink {
            ID3D12GraphicsCommandList* c;
            void heaps(UINT n,ID3D12DescriptorHeap* const* h){c->SetDescriptorHeaps(n,h);}
            void root(ID3D12RootSignature* r){c->SetComputeRootSignature(r);}
            void pipeline(ID3D12PipelineState* p){c->SetPipelineState(p);}
            void constants(UINT i,UINT n,const UINT* v){c->SetComputeRoot32BitConstants(i,n,v,0);}
            void table(UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h){c->SetComputeRootDescriptorTable(i,h);}
            void descriptor(UINT i,Kind k,D3D12_GPU_VIRTUAL_ADDRESS a){
                if(k==Kind::cbv)c->SetComputeRootConstantBufferView(i,a);
                else if(k==Kind::srv)c->SetComputeRootShaderResourceView(i,a);
                else c->SetComputeRootUnorderedAccessView(i,a);
            }
        } sink{list};replay(sink,list,generation);
    }
};
}
