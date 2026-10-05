// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_insertion_bindings.hpp"
#include "binding_state.hpp"
#include "lab_insertion_slots.hpp"
#include "lab_hook_bank.hpp"
#include "lab_resume_slots.hpp"
#include <MinHook.h>
#include <atomic>
#include <bitset>
#include <mutex>
#include <wrl/client.h>

namespace lab {
using Microsoft::WRL::ComPtr;
using List=ID3D12GraphicsCommandList;
namespace {
// Private ownership points from the native list to our bounded lifetime token,
// never back to the list. Destruction releases the token even without Close.
constexpr GUID recording_tag={0x645c6d48,0x83d1,0x45be,{0xb4,0x67,0x29,0xaf,0x6a,0x18,0x27,0x11}};
using Arguments=host::RootArguments;
using State=host::BindingState;
}
// Concurrent recordings tracked on the seed device. Level loading records many
// lists in parallel; overflow refuses the untracked list, never evicts a slot.
constexpr std::size_t kRecordingSlots=256;
constexpr const char* kCapacityMessage="active-recording-capacity-256";
struct InsertionBindings::Impl {
    using Slot=insertions::Slot;
    inline static std::atomic<Impl*> active{nullptr};
    inline static thread_local Impl* injected=nullptr;
    inline static thread_local List* injected_list=nullptr;
    inline static thread_local State* injected_restore=nullptr;
    inline static thread_local unsigned create_depth=0;
    using Hooks=HookBank<insertions::Count,2>;
    Hooks hooks;
    HookBank<1,1> creation_hook;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12DescriptorHeap> saved_rtvs,saved_dsvs;
    UINT rtv_increment=0,dsv_increment=0;
    std::atomic<std::uint64_t> target_copies{0};
    std::atomic<bool> tolerate_unknown_indirect{false};
    std::atomic<std::uint64_t> unknown_indirect_calls{0};
    std::array<void*,2> lifecycle_methods{};
    std::array<Hooks::Entries,2> targets{};
    struct Entry final : IUnknown {
        Impl* owner=nullptr;unsigned index=0;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_copy{},dsv_copy{};
        std::atomic<List*> list{nullptr};mutable std::mutex mutex;State state;
        std::atomic<bool> loss{false},reserved{false};std::atomic<ULONG> refs{0};
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{
            if(!out)return E_POINTER;*out=nullptr;if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;
            *out=static_cast<IUnknown*>(this);AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
        ULONG STDMETHODCALLTYPE Release()override{
            const auto remaining=--refs;if(!remaining){
                {std::lock_guard lock(mutex);owner->keys[index]=nullptr;list=nullptr;state={};loss=false;}
                --owner->recordings;++owner->recycled;reserved.store(false,std::memory_order_release);
            }return remaining; // Fixed storage stays alive with the detours.
        }
    };
    std::array<Entry,kRecordingSlots> entries;
    struct GameCall {
        State saved;
        std::array<ComPtr<IUnknown>,8> pins;
        ComPtr<List> list;
        ComPtr<IUnknown> token;
        Entry* entry=nullptr;
        std::uint64_t call=0,frame=0;
        DWORD thread=0;
        const char* error=nullptr;
        bool used=false;
    } game_call;
    // Separate CPU descriptor storage: foreign Evaluate may overwrite the
    // normal per-recording RTV/DSV snapshots while the entry checkpoint lives.
    ComPtr<ID3D12DescriptorHeap> entry_rtvs,entry_dsvs;
    std::atomic<std::uint64_t> game_checkpoints{0},game_restores{0};
    std::atomic<unsigned> game_list_pins{0};
    std::array<std::atomic<List*>,kRecordingSlots> keys{}; // contiguous lookup, not large state cache lines
    std::atomic<std::uint64_t> serial{0},begins{0},created{0},recycled{0},overflows{0},attachment_failures{0};
    std::atomic<unsigned> recordings{0},peak{0};
    std::atomic<bool> stopped{false},injected_fault{false};
    std::atomic<std::uint64_t> setters{0},restores{0},losses{0};
    std::atomic<std::uint64_t> heap_permutations{0},heap_permuted_insertions{0};
    std::atomic<const char*> watch_failure{"none"};
    std::atomic<unsigned> mismatch_slot{insertions::Count};
    std::atomic<std::uintptr_t> mismatch_expected{0},mismatch_actual{0};
    Impl(){for(unsigned i=0;i<entries.size();++i){entries[i].owner=this;entries[i].index=i;}}
    Entry* find(List* l){if(!l)return nullptr;for(unsigned i=0;i<keys.size();++i)if(keys[i].load(std::memory_order_acquire)==l)return &entries[i];return nullptr;}
    template<class F> static F original(unsigned bank,Slot slot){return active.load()->hooks.original<F>(bank,slot);}
    static void optional_mutation(List* l,bool State::*flag)noexcept{
        if(injected_list==l&&injected_restore)injected_restore->*flag=true;
    }
    template<class F> static void edit(List* l,F&& f,bool allowed_injection=true)noexcept{
        auto* s=active.load();if(!s||s->stopped)return;
        if(injected==s&&injected_list==l){if(!allowed_injection)s->injected_fault=true;return;}
        auto* e=s->find(l);if(!e)return;
        std::unique_lock lock(e->mutex,std::try_to_lock);if(!lock.owns_lock()){e->loss=true;++s->losses;return;}
        if(e->list!=l)return;
        ++s->setters;if(!e->state.closed)f(e->state);
    }
    static HRESULT STDMETHODCALLTYPE create(ID3D12Device* d,UINT node,D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator* allocator,ID3D12PipelineState* pso,REFIID iid,void** result){
        struct Scope{Scope(){++create_depth;}~Scope(){--create_depth;}} scope;
        auto* s=active.load();
        const auto hr=s->creation_hook.original<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,UINT,D3D12_COMMAND_LIST_TYPE,
            ID3D12CommandAllocator*,ID3D12PipelineState*,REFIID,void**)>(0,0)(d,node,type,allocator,pso,iid,result);
        if(create_depth==1&&!s->stopped&&d==s->device.Get()&&type==D3D12_COMMAND_LIST_TYPE_DIRECT&&SUCCEEDED(hr)&&result&&*result){
            ComPtr<List> l;if(SUCCEEDED(static_cast<IUnknown*>(*result)->QueryInterface(IID_PPV_ARGS(&l))))s->begin(l.Get(),pso,true);
        }return hr;
    }
    void detach(List* l)noexcept{
        if(!find(l))return;
        // No entry mutex may be held across the foreign call: releasing the
        // private interface can synchronously retire the slot.
        if(FAILED(l->SetPrivateDataInterface(recording_tag,nullptr))){
            ++attachment_failures;if(auto* e=find(l))e->loss=true;
        }
    }
    void begin(List* l,ID3D12PipelineState* pso,bool from_create)noexcept{
        if(stopped||!l||l->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
        ComPtr<ID3D12Device> d;ComPtr<ID3D12GraphicsCommandList10> v4;
        if(FAILED(l->GetDevice(IID_PPV_ARGS(&d)))||d.Get()!=device.Get()||
            FAILED(l->QueryInterface(IID_PPV_ARGS(&v4)))||v4.Get()!=l)return;
        auto** v=*reinterpret_cast<void***>(l);const auto indices=insertions::slots();
        const auto lifecycle=resume::slots();
        if(v[lifecycle[resume::Reset]]!=lifecycle_methods[0]||v[lifecycle[resume::Close]]!=lifecycle_methods[1])return;
        for(unsigned i=0;i<indices.size();++i)if(!known(i,v[indices[i]]))return;
        detach(l); // successful Reset invalidates ALL earlier recording state
        if(find(l)){++attachment_failures;return;}
        for(auto& e:entries){bool empty=false;if(!e.reserved.compare_exchange_strong(empty,true))continue;
            e.refs=1;const auto current=++recordings;auto old=peak.load();while(old<current&&!peak.compare_exchange_weak(old,current)){}
            {std::lock_guard lock(e.mutex);e.state={};e.loss=false;e.state.generation=++serial;
                e.state.closed=false;e.state.reset_seen=!from_create;e.state.create_seen=from_create;e.state.pso=pso;
                if(!e.state.generation)e.state.reject("recording-generation-overflow");}
            const auto hr=l->SetPrivateDataInterface(recording_tag,&e);
            if(SUCCEEDED(hr)){e.list.store(l,std::memory_order_release);keys[e.index].store(l,std::memory_order_release);++begins;if(from_create)++created;}
            else ++attachment_failures;
            e.Release();return;
        }++overflows; // missing state remains unusable; never evict active work
    }
    template<unsigned B>static void STDMETHODCALLTYPE pipeline(List* l,ID3D12PipelineState* p){original<void(STDMETHODCALLTYPE*)(List*,ID3D12PipelineState*)>(B,Slot::Pipeline)(l,p);
        edit(l,[&](State& s){s.pso=p;s.rt=nullptr;});}
    template<unsigned B>static void STDMETHODCALLTYPE heaps(List* l,UINT n,ID3D12DescriptorHeap* const* h){original<void(STDMETHODCALLTYPE*)(List*,UINT,ID3D12DescriptorHeap* const*)>(B,Slot::Heaps)(l,n,h);
        edit(l,[&](State& s){const auto before=s.heap_order_permutations;s.set_heaps(n,h);
            if(s.heap_order_permutations!=before)++active.load()->heap_permutations;});}
    template<unsigned B>static void STDMETHODCALLTYPE root(List* l,ID3D12RootSignature* r){original<void(STDMETHODCALLTYPE*)(List*,ID3D12RootSignature*)>(B,Slot::Root)(l,r);
        edit(l,[&](State& s){s.compute.signature(r);});}
    template<unsigned B>static void STDMETHODCALLTYPE table(List* l,UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h){original<void(STDMETHODCALLTYPE*)(List*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE)>(B,Slot::Table)(l,i,h);
        edit(l,[&](State& s){s.compute.address(i,Arguments::table,h.ptr);});}
    template<unsigned B,Slot slot,Arguments::Kind kind>static void STDMETHODCALLTYPE descriptor(List* l,UINT i,D3D12_GPU_VIRTUAL_ADDRESS a){original<void(STDMETHODCALLTYPE*)(List*,UINT,D3D12_GPU_VIRTUAL_ADDRESS)>(B,slot)(l,i,a);
        edit(l,[&](State& s){s.compute.address(i,kind,a);});}
    template<unsigned B>static void STDMETHODCALLTYPE constant(List* l,UINT i,UINT v,UINT o){original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT,UINT)>(B,Slot::Constant)(l,i,v,o);
        edit(l,[&](State& s){s.compute.constant(i,1,&v,o);});}
    template<unsigned B>static void STDMETHODCALLTYPE constants(List* l,UINT i,UINT n,const void* v,UINT o){original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT,const void*,UINT)>(B,Slot::Constants)(l,i,n,v,o);
        edit(l,[&](State& s){s.compute.constant(i,n,v,o);});}
    template<unsigned B>static void STDMETHODCALLTYPE graphics_root(List* l,ID3D12RootSignature* r){original<void(STDMETHODCALLTYPE*)(List*,ID3D12RootSignature*)>(B,Slot::GraphicsRoot)(l,r);
        edit(l,[&](State& s){s.graphics.signature(r);});}
    template<unsigned B>static void STDMETHODCALLTYPE graphics_table(List* l,UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h){original<void(STDMETHODCALLTYPE*)(List*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE)>(B,Slot::GraphicsTable)(l,i,h);
        edit(l,[&](State& s){s.graphics.address(i,Arguments::table,h.ptr);});}
    template<unsigned B,Slot slot,Arguments::Kind kind>static void STDMETHODCALLTYPE graphics_descriptor(List* l,UINT i,D3D12_GPU_VIRTUAL_ADDRESS a){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,D3D12_GPU_VIRTUAL_ADDRESS)>(B,slot)(l,i,a);edit(l,[&](State& s){s.graphics.address(i,kind,a);});}
    template<unsigned B>static void STDMETHODCALLTYPE graphics_constant(List* l,UINT i,UINT v,UINT o){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT,UINT)>(B,Slot::GraphicsConstant)(l,i,v,o);edit(l,[&](State& s){s.graphics.constant(i,1,&v,o);});}
    template<unsigned B>static void STDMETHODCALLTYPE graphics_constants(List* l,UINT i,UINT n,const void* v,UINT o){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT,const void*,UINT)>(B,Slot::GraphicsConstants)(l,i,n,v,o);edit(l,[&](State& s){s.graphics.constant(i,n,v,o);});}
    template<unsigned B>static void STDMETHODCALLTYPE topology(List* l,D3D12_PRIMITIVE_TOPOLOGY t){
        original<void(STDMETHODCALLTYPE*)(List*,D3D12_PRIMITIVE_TOPOLOGY)>(B,Slot::Topology)(l,t);edit(l,[&](State& s){s.topology=t;});}
    template<unsigned B>static void STDMETHODCALLTYPE viewports(List* l,UINT n,const D3D12_VIEWPORT* p){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,const D3D12_VIEWPORT*)>(B,Slot::Viewports)(l,n,p);edit(l,[&](State& s){s.range(s.viewports,0,n,p);s.viewport_count=std::min(n,16u);});}
    template<unsigned B>static void STDMETHODCALLTYPE scissors(List* l,UINT n,const D3D12_RECT* p){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,const D3D12_RECT*)>(B,Slot::Scissors)(l,n,p);edit(l,[&](State& s){s.range(s.scissors,0,n,p);s.scissor_count=std::min(n,16u);});}
    template<unsigned B>static void STDMETHODCALLTYPE blend(List* l,const FLOAT* p){
        original<void(STDMETHODCALLTYPE*)(List*,const FLOAT*)>(B,Slot::Blend)(l,p);edit(l,[&](State& s){if(p)std::copy_n(p,4,s.blend.begin());else s.blend={1,1,1,1};});}
    template<unsigned B>static void STDMETHODCALLTYPE stencil(List* l,UINT v){
        original<void(STDMETHODCALLTYPE*)(List*,UINT)>(B,Slot::Stencil)(l,v);edit(l,[&](State& s){s.front_stencil=s.back_stencil=v;});}
    template<unsigned B>static void STDMETHODCALLTYPE index(List* l,const D3D12_INDEX_BUFFER_VIEW* p){
        original<void(STDMETHODCALLTYPE*)(List*,const D3D12_INDEX_BUFFER_VIEW*)>(B,Slot::Index)(l,p);edit(l,[&](State& s){s.has_index=p!=nullptr;s.index=p?*p:D3D12_INDEX_BUFFER_VIEW{};});}
    template<unsigned B>static void STDMETHODCALLTYPE vertex(List* l,UINT first,UINT n,const D3D12_VERTEX_BUFFER_VIEW* p){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT,const D3D12_VERTEX_BUFFER_VIEW*)>(B,Slot::Vertex)(l,first,n,p);edit(l,[&](State& s){s.range(s.vertex,first,n,p);});}
    template<unsigned B>static void STDMETHODCALLTYPE stream_output(List* l,UINT first,UINT n,const D3D12_STREAM_OUTPUT_BUFFER_VIEW* p){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT,const D3D12_STREAM_OUTPUT_BUFFER_VIEW*)>(B,Slot::StreamOutput)(l,first,n,p);edit(l,[&](State& s){s.range(s.stream_output,first,n,p);});}
    template<unsigned B>static void STDMETHODCALLTYPE render_targets(List* l,UINT n,const D3D12_CPU_DESCRIPTOR_HANDLE* p,BOOL range,const D3D12_CPU_DESCRIPTOR_HANDLE* d){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*)>(B,Slot::Targets)(l,n,p,range,d);
        edit(l,[&](State& state){auto* s=active.load();auto* entry=s->find(l);
            state.render_targets(n,p,range,d,s->rtv_increment);
            if(state.invalid||!entry)return;
            // RTV/DSV descriptor contents are consumed by the native bind call.
            // The caller may overwrite/free its CPU descriptor heap after our
            // detour returns. Keep CONTENTS now, not addresses to reread later.
            // Fixed per-recording slots are allocated before hooks attach; no
            // callback allocations, GPU copies, or resource lifetime inference.
            for(UINT i=0;i<state.target_count;++i){auto saved=entry->rtv_copy;saved.ptr+=SIZE_T(i)*s->rtv_increment;
                s->device->CopyDescriptorsSimple(1,saved,state.targets[i],D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                state.targets[i]=saved;++s->target_copies;}
            if(state.has_depth){s->device->CopyDescriptorsSimple(1,entry->dsv_copy,state.depth_target,D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
                state.depth_target=entry->dsv_copy;++s->target_copies;}
        });}
    template<unsigned B>static void STDMETHODCALLTYPE depth_bounds(List* l,FLOAT a,FLOAT b){
        original<void(STDMETHODCALLTYPE*)(List*,FLOAT,FLOAT)>(B,Slot::DepthBounds)(l,a,b);optional_mutation(l,&State::depth_bounds_seen);
        edit(l,[&](State& s){s.min_depth=a;s.max_depth=b;s.depth_bounds_seen=true;});}
    template<unsigned B>static void STDMETHODCALLTYPE samples(List* l,UINT n,UINT pixels,D3D12_SAMPLE_POSITION* p){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT,D3D12_SAMPLE_POSITION*)>(B,Slot::SamplePositions)(l,n,pixels,p);optional_mutation(l,&State::samples_seen);
        edit(l,[&](State& s){if(n>16||pixels>16||(n*pixels&&!p)){s.reject("sample-positions-invalid");return;}s.samples_per_pixel=n;s.sample_pixels=pixels;s.range(s.samples,0,n*pixels,p);s.samples_seen=true;});}
    template<unsigned B>static void STDMETHODCALLTYPE view_mask(List* l,UINT m){
        original<void(STDMETHODCALLTYPE*)(List*,UINT)>(B,Slot::ViewMask)(l,m);optional_mutation(l,&State::view_mask_seen);
        edit(l,[&](State& s){s.view_mask=m;s.view_mask_seen=true;});}
    template<unsigned B>static void STDMETHODCALLTYPE shading(List* l,D3D12_SHADING_RATE r,const D3D12_SHADING_RATE_COMBINER* p){
        original<void(STDMETHODCALLTYPE*)(List*,D3D12_SHADING_RATE,const D3D12_SHADING_RATE_COMBINER*)>(B,Slot::ShadingRate)(l,r,p);optional_mutation(l,&State::shading_seen);
        edit(l,[&](State& s){s.shading_rate=r;s.combiners={D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};if(p)std::copy_n(p,2,s.combiners.begin());s.shading_seen=true;});}
    template<unsigned B>static void STDMETHODCALLTYPE shading_image(List* l,ID3D12Resource* p){
        original<void(STDMETHODCALLTYPE*)(List*,ID3D12Resource*)>(B,Slot::ShadingImage)(l,p);optional_mutation(l,&State::shading_image_seen);
        edit(l,[&](State& s){s.shading_image=p;s.shading_image_seen=true;});}
    template<unsigned B>static void STDMETHODCALLTYPE split_stencil(List* l,UINT f,UINT b){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT)>(B,Slot::SplitStencil)(l,f,b);optional_mutation(l,&State::split_stencil_seen);
        edit(l,[&](State& s){s.front_stencil=f;s.back_stencil=b;s.split_stencil_seen=true;});}
    template<unsigned B>static void STDMETHODCALLTYPE depth_bias(List* l,FLOAT a,FLOAT b,FLOAT c){
        original<void(STDMETHODCALLTYPE*)(List*,FLOAT,FLOAT,FLOAT)>(B,Slot::DepthBias)(l,a,b,c);optional_mutation(l,&State::depth_bias_seen);
        edit(l,[&](State& s){s.depth_bias=a;s.depth_bias_clamp=b;s.slope_bias=c;s.depth_bias_seen=true;});}
    template<unsigned B>static void STDMETHODCALLTYPE strip_cut(List* l,D3D12_INDEX_BUFFER_STRIP_CUT_VALUE v){
        original<void(STDMETHODCALLTYPE*)(List*,D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)>(B,Slot::StripCut)(l,v);optional_mutation(l,&State::strip_cut_seen);
        edit(l,[&](State& s){s.strip_cut=v;s.strip_cut_seen=true;});}
    template<unsigned B>static void STDMETHODCALLTYPE protected_session(List* l,ID3D12ProtectedResourceSession* p){
        original<void(STDMETHODCALLTYPE*)(List*,ID3D12ProtectedResourceSession*)>(B,Slot::ProtectedSession)(l,p);edit(l,[](State& s){s.reject("protected-session-untracked");},false);}
    template<unsigned B,Slot S>static void STDMETHODCALLTYPE meta(List* l,ID3D12MetaCommand* m,const void* p,SIZE_T n){
        original<void(STDMETHODCALLTYPE*)(List*,ID3D12MetaCommand*,const void*,SIZE_T)>(B,S)(l,m,p,n);edit(l,[](State& s){s.reject(S==Slot::InitMeta?"meta-initialize-untracked":"meta-execute-untracked");},false);}
    template<unsigned B>static void STDMETHODCALLTYPE program(List* l,const D3D12_SET_PROGRAM_DESC* p){
        original<void(STDMETHODCALLTYPE*)(List*,const D3D12_SET_PROGRAM_DESC*)>(B,Slot::Program)(l,p);edit(l,[](State& s){s.reject("program-untracked");},false);}
    template<unsigned B>static void STDMETHODCALLTYPE clear(List* l,ID3D12PipelineState* p){original<void(STDMETHODCALLTYPE*)(List*,ID3D12PipelineState*)>(B,Slot::Clear)(l,p);
        edit(l,[](State& s){s.reject("clear-state-untracked");},false);}
    template<unsigned B>static void STDMETHODCALLTYPE bundle(List* l,List* b){original<void(STDMETHODCALLTYPE*)(List*,List*)>(B,Slot::Bundle)(l,b);edit(l,[](State& s){s.reject("bundle-state-untracked");},false);}
    template<unsigned B>static void STDMETHODCALLTYPE indirect(List* l,ID3D12CommandSignature* sig,UINT n,ID3D12Resource* a,UINT64 o,ID3D12Resource* c,UINT64 co){
        original<void(STDMETHODCALLTYPE*)(List*,ID3D12CommandSignature*,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64)>(B,Slot::Indirect)(l,sig,n,a,o,c,co);
        edit(l,[&](State& s){host::indirect::Layout layout;
            if(!host::indirect::read(sig,layout)){
                // See InsertionBindings::tolerate_unknown_indirect.
                if(auto* self=active.load();self&&self->tolerate_unknown_indirect.load()){++self->unknown_indirect_calls;return;}
                s.reject("indirect-signature-unobserved");return;}
            s.after_indirect(layout);},false);}
    template<unsigned B>static void STDMETHODCALLTYPE predicate(List* l,ID3D12Resource* r,UINT64 o,D3D12_PREDICATION_OP op){original<void(STDMETHODCALLTYPE*)(List*,ID3D12Resource*,UINT64,D3D12_PREDICATION_OP)>(B,Slot::Predicate)(l,r,o,op);
        edit(l,[&](State& s){s.predicate=r;s.predicate_offset=o;s.predicate_op=op;});}
    template<unsigned B>static void STDMETHODCALLTYPE state_object(ID3D12GraphicsCommandList4* l,ID3D12StateObject* p){original<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,ID3D12StateObject*)>(B,Slot::StateObject)(l,p);
        edit(l,[&](State& s){s.rt=p;s.pso=nullptr;});}
    template<unsigned B>static void STDMETHODCALLTYPE begin_pass(ID3D12GraphicsCommandList4* l,UINT n,const D3D12_RENDER_PASS_RENDER_TARGET_DESC* r,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* d,D3D12_RENDER_PASS_FLAGS f){
        original<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,UINT,const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,D3D12_RENDER_PASS_FLAGS)>(B,Slot::BeginPass)(l,n,r,d,f);
        edit(l,[](State& s){s.render_pass=true;s.targets_known=false;},false);}
    template<unsigned B>static void STDMETHODCALLTYPE end_pass(ID3D12GraphicsCommandList4* l){original<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*)>(B,Slot::EndPass)(l);edit(l,[](State& s){s.render_pass=false;},false);}
    template<unsigned B>static Hooks::Entries callbacks(){using S=insertions::Slot;return {
        (void*)pipeline<B>,(void*)heaps<B>,(void*)root<B>,(void*)table<B>,
        (void*)descriptor<B,S::Cbv,Arguments::cbv>,(void*)descriptor<B,S::Srv,Arguments::srv>,(void*)descriptor<B,S::Uav,Arguments::uav>,
        (void*)constant<B>,(void*)constants<B>,(void*)graphics_root<B>,(void*)graphics_table<B>,(void*)clear<B>,
        (void*)bundle<B>,(void*)indirect<B>,(void*)predicate<B>,(void*)state_object<B>,(void*)begin_pass<B>,(void*)end_pass<B>,
        (void*)graphics_descriptor<B,S::GraphicsCbv,Arguments::cbv>,(void*)graphics_descriptor<B,S::GraphicsSrv,Arguments::srv>,(void*)graphics_descriptor<B,S::GraphicsUav,Arguments::uav>,
        (void*)graphics_constant<B>,(void*)graphics_constants<B>,(void*)topology<B>,(void*)viewports<B>,(void*)scissors<B>,(void*)blend<B>,(void*)stencil<B>,
        (void*)index<B>,(void*)vertex<B>,(void*)stream_output<B>,(void*)render_targets<B>,(void*)depth_bounds<B>,(void*)samples<B>,(void*)view_mask<B>,(void*)shading<B>,(void*)shading_image<B>,
        (void*)split_stencil<B>,(void*)depth_bias<B>,(void*)strip_cut<B>,(void*)protected_session<B>,(void*)meta<B,S::InitMeta>,(void*)meta<B,S::ExecuteMeta>,(void*)program<B>};}
    bool known(unsigned slot,void* method)const{return method==targets[0][slot]||method==targets[1][slot];}
};
InsertionBindings* InsertionBindings::install(List* method,List* reset_method){
    if(!method||!reset_method)throw std::logic_error("Native binding lifecycle methods required");
    for(auto* l:{method,reset_method}){ComPtr<ID3D12GraphicsCommandList10> v10;if(FAILED(l->QueryInterface(IID_PPV_ARGS(&v10)))||v10.Get()!=l)throw std::logic_error("Native v10 identity required for complete setter coverage");}
    auto* s=new Impl;Impl* empty=nullptr;if(!Impl::active.compare_exchange_strong(empty,s)){delete s;throw std::logic_error("One binding tracker only");}
    auto* result=new InsertionBindings(s);
    const Impl::Hooks::Detours detours{Impl::callbacks<0>(),Impl::callbacks<1>()};
    const auto indices=insertions::slots();unsigned bank=0;
    const auto lifecycle=resume::slots();auto** methods=*reinterpret_cast<void***>(method);
    s->lifecycle_methods={methods[lifecycle[resume::Reset]],methods[lifecycle[resume::Close]]};
    for(auto* l:{method,reset_method}){auto** v=*reinterpret_cast<void***>(l);for(unsigned i=0;i<indices.size();++i)s->targets[bank][i]=v[indices[i]];++bank;}
    try{if(FAILED(method->GetDevice(IID_PPV_ARGS(&s->device))))throw std::runtime_error("Binding seed device unavailable");
        s->rtv_increment=s->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        s->dsv_increment=s->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_DESCRIPTOR_HEAP_DESC descriptors{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,UINT(s->entries.size()*8),D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        if(FAILED(s->device->CreateDescriptorHeap(&descriptors,IID_PPV_ARGS(&s->saved_rtvs))))throw std::runtime_error("Binding RTV snapshot heap unavailable");
        descriptors.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;descriptors.NumDescriptors=UINT(s->entries.size());
        if(FAILED(s->device->CreateDescriptorHeap(&descriptors,IID_PPV_ARGS(&s->saved_dsvs))))throw std::runtime_error("Binding DSV snapshot heap unavailable");
        descriptors.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;descriptors.NumDescriptors=8;
        if(FAILED(s->device->CreateDescriptorHeap(&descriptors,IID_PPV_ARGS(&s->entry_rtvs))))throw std::runtime_error("Game entry RTV snapshot heap unavailable");
        descriptors.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;descriptors.NumDescriptors=1;
        if(FAILED(s->device->CreateDescriptorHeap(&descriptors,IID_PPV_ARGS(&s->entry_dsvs))))throw std::runtime_error("Game entry DSV snapshot heap unavailable");
        for(auto& entry:s->entries){entry.rtv_copy=s->saved_rtvs->GetCPUDescriptorHandleForHeapStart();
            entry.rtv_copy.ptr+=SIZE_T(entry.index)*8*s->rtv_increment;
            entry.dsv_copy=s->saved_dsvs->GetCPUDescriptorHandleForHeapStart();entry.dsv_copy.ptr+=SIZE_T(entry.index)*s->dsv_increment;}
        const auto init=MH_Initialize();if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("Binding MinHook init");
        HMODULE pin{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Impl::pipeline<0>),&pin))throw std::runtime_error("Pin binding code");
        for(const auto& family:s->targets)s->hooks.install(family,detours);
        s->creation_hook.install({(*reinterpret_cast<void***>(s->device.Get()))[insertions::create_command_list_slot()]},
            {{{reinterpret_cast<void*>(&Impl::create)}}});
        return result;
    }catch(...){s->stopped=true;throw;}
}
bool InsertionBindings::watch(List* l){
    auto& s=*impl_;if(!l||s.stopped){s.watch_failure=!l?"null-list":"tracker-stopped";return false;}
    const auto indices=insertions::slots();auto** v=*reinterpret_cast<void***>(l);
    ComPtr<ID3D12GraphicsCommandList10> v4;if(FAILED(l->QueryInterface(IID_PPV_ARGS(&v4)))){s.watch_failure="v10-query-failed";return false;}
    if(v4.Get()!=l){s.watch_failure="v10-interface-address-differs";return false;}
    for(unsigned i=0;i<indices.size();++i)if(!s.known(i,v[indices[i]])){
        s.mismatch_slot=i;s.mismatch_expected=reinterpret_cast<std::uintptr_t>(s.targets[1][i]);s.mismatch_actual=reinterpret_cast<std::uintptr_t>(v[indices[i]]);
        s.watch_failure="method-mismatch";return false;}
    if(!s.find(l)&&s.recordings>=s.entries.size()){s.watch_failure=kCapacityMessage;return false;}
    s.watch_failure="none";return true; // unknown pre-install recording waits for a real begin
}
std::string InsertionBindings::watch_error()const{auto& s=*impl_;const auto reason=std::string(s.watch_failure.load());
    return "Native bindings: "+reason+(reason=="method-mismatch"?
        "; slot="+std::to_string(s.mismatch_slot.load())+"; expected="+std::to_string(s.mismatch_expected.load())+"; actual="+std::to_string(s.mismatch_actual.load()):"");}
diagnostic::BindingSnapshot InsertionBindings::boundary_snapshot(List* l)const noexcept {
    diagnostic::BindingSnapshot out;
    auto reason=[&](const char* s){strncpy_s(out.reason,s,_TRUNCATE);return out;};
    auto* e=impl_->find(l);if(!e)return reason("recording-unobserved");
    std::unique_lock lock(e->mutex,std::try_to_lock);
    if(!lock.owns_lock())return reason("snapshot-busy");
    if(e->list!=l||e->loss||impl_->stopped)return reason("recording-lost-or-stopped");
    const auto& s=e->state;out.tracked=1;out.ready=s.ready()?1u:0u;out.generation=s.generation;
    out.pipeline=reinterpret_cast<std::uint64_t>(s.pso);out.raytracing_pipeline=reinterpret_cast<std::uint64_t>(s.rt);
    out.heap_count=s.heap_count;out.heap_permutations=s.heap_order_permutations;
    for(unsigned i=0;i<2;++i)out.heaps[i]=reinterpret_cast<std::uint64_t>(s.heaps[i]);
    const auto bank=[](const Arguments& in,diagnostic::BindingBankSnapshot& b){
        b.root=reinterpret_cast<std::uint64_t>(in.root);b.constant_hash=14695981039346656037ull;
        const auto hash=[&](std::uint64_t v){for(unsigned j=0;j<8;++j){b.constant_hash^=(v>>(j*8))&255;b.constant_hash*=1099511628211ull;}};
        for(unsigned i=0;i<64;++i){const auto& a=in.args[i];const auto bit=1ull<<i;
            if(a.kind==Arguments::table){b.tables|=bit;b.table_addresses[i]=a.address;}
            else if(a.kind==Arguments::constants){b.constants|=bit;hash(i);hash(a.known.to_ullong());
                for(unsigned j=0;j<64;++j)if(a.known[j])hash(a.words[j]);}
            else if(a.kind!=Arguments::unset){b.descriptors|=bit;hash(i);hash(a.kind);hash(a.address);}}
    };
    bank(s.compute,out.compute);bank(s.graphics,out.graphics);
    if(const auto* why=s.blocker())strncpy_s(out.reason,why,_TRUNCATE);return out;
}
json InsertionBindings::inspect(List* l)const{
    auto& s=*impl_;auto* e=s.find(l);json out={{"tracked",e!=nullptr},{"watch_error",watch_error()},{"method_mismatches",json::array()}};
    if(!l){out["recordings"]=json::array();for(auto& item:s.entries){std::lock_guard lock(item.mutex);if(auto* p=item.list.load())
        out["recordings"].push_back({{"list",reinterpret_cast<UINT64>(p)},{"refs",item.refs.load()},
            {"generation",item.state.generation},{"create_seen",item.state.create_seen},{"reset_seen",item.state.reset_seen}});}}
    if(e){std::lock_guard lock(e->mutex);const auto& st=e->state;
        out["tracked"]=e->list==l;
        out["state"]={{"reset_seen",st.reset_seen},{"create_seen",st.create_seen},{"generation",st.generation},{"closed",st.closed},{"invalid",st.invalid},{"render_pass",st.render_pass},
            {"compute_valid",st.compute.valid},{"graphics_valid",st.graphics.valid},{"invalid_reason",st.invalid_reason?st.invalid_reason:"none"},
            {"compute_error",st.compute.invalid_reason?st.compute.invalid_reason:"none"},{"graphics_error",st.graphics.invalid_reason?st.graphics.invalid_reason:"none"},
            {"compute_root",st.compute.root!=nullptr},{"pso",st.pso!=nullptr},{"rt",st.rt!=nullptr},{"loss",e->loss.load()}};}
    if(l){auto** v=*reinterpret_cast<void***>(l);const auto indices=insertions::slots();for(unsigned i=0;i<indices.size();++i)
        if(!s.known(i,v[indices[i]]))out["method_mismatches"].push_back({{"slot",i},{"vtable_slot",indices[i]},
            {"expected",reinterpret_cast<UINT64>(s.targets[1][i])},{"actual",reinterpret_cast<UINT64>(v[indices[i]])}});}
    return out;
}
void InsertionBindings::reset(List* l,HRESULT hr,ID3D12PipelineState* p)noexcept{
    auto& s=*impl_;if(FAILED(hr)||s.stopped){s.detach(l);return;}s.begin(l,p,false);
}
void InsertionBindings::close(List* l)noexcept{Impl::edit(l,[](State& s){s.closed=true;},false);impl_->detach(l);}
bool InsertionBindings::ready(List* l)const noexcept{
    return blocker(l)==nullptr;
}
const char* InsertionBindings::blocker(List* l)const noexcept{
    auto& s=*impl_;if(s.stopped)return "tracker-stopped";if(!l)return "null-list";
    auto* e=s.find(l);if(!e)return "recording-start-unobserved";
    if(e->loss)return "recording-events-lost";
    std::unique_lock lock(e->mutex,std::try_to_lock);
    if(!lock.owns_lock())return "recording-lock-busy";
    if(e->list!=l)return "recording-retired";
    if(e->loss)return "recording-events-lost";
    return e->state.blocker();
}
std::string InsertionBindings::readiness_error(List* l)const{
    // Only materialized at refusal, never every frame. All components bounded.
    const char* reason=blocker(l);std::string out="Native bindings: ";out+=reason?reason:"state-changed-during-check";
    if(auto* e=impl_->find(l)){std::unique_lock lock(e->mutex,std::try_to_lock);if(lock.owns_lock()&&e->list==l){
        out+="; generation="+std::to_string(e->state.generation);
        if(!e->state.compute.valid&&e->state.compute.invalid_reason)out+="; compute="+std::string(e->state.compute.invalid_reason);
        if(!e->state.graphics.valid&&e->state.graphics.invalid_reason)out+="; graphics="+std::string(e->state.graphics.invalid_reason);
    }}return out.substr(0,240);
}
void InsertionBindings::preserving(List* l,const std::function<void()>& work){
    auto& s=*impl_;auto* e=s.find(l);if(!e||s.stopped||e->loss||Impl::injected)throw std::logic_error("Unobserved insertion bindings");
    State saved;ComPtr<IUnknown> token;
    {std::unique_lock lock(e->mutex,std::try_to_lock);if(!lock.owns_lock()||e->list!=l||e->loss||!e->state.ready())throw std::logic_error("Incomplete insertion bindings");saved=e->state;token=e;}
    if(saved.heap_order_permutations)++s.heap_permuted_insertions;
    // Keep original binding objects through the insertion. The host retains
    // descriptors/resources for its own submitted work; no content inference.
    std::array<ComPtr<IUnknown>,8> pins{saved.compute.root,saved.graphics.root,saved.pso,saved.rt,saved.heaps[0],saved.heaps[1],saved.predicate,saved.shading_image};
    Impl::injected=&s;Impl::injected_list=l;Impl::injected_restore=&saved;s.injected_fault=false;
    std::exception_ptr failure;
    try{l->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);work();}catch(...){failure=std::current_exception();}
    try{saved.restore(l);}catch(...){if(!failure)failure=std::current_exception();}
    Impl::injected=nullptr;Impl::injected_list=nullptr;Impl::injected_restore=nullptr;
    if(s.injected_fault||e->loss){e->loss=true;throw std::logic_error("Unexpected state mutation during compute insertion");}
    ++s.restores;if(failure)std::rethrow_exception(failure);
}
void InsertionBindings::begin_game_call(std::uint64_t call,std::uint64_t frame,List* l){
    auto& s=*impl_;auto& g=s.game_call;
    if(g.call){g.error="overlapping-game-call";throw std::logic_error(g.error);}
    g.call=call;g.frame=frame;g.thread=GetCurrentThreadId();
    if(!call||!frame||!l||s.stopped||Impl::injected){g.error="invalid-game-entry";return;}
    auto* e=s.find(l);if(!e){g.error="game-entry-recording-unobserved";return;}
    {
        std::unique_lock lock(e->mutex,std::try_to_lock);
        if(!lock.owns_lock()||e->list!=l||e->loss){g.error="game-entry-recording-lost";return;}
        if(const auto* why=e->state.blocker()){g.error=why;return;}
        g.saved=e->state;g.token=e;g.entry=e;
    }
    // Never hold a tracker mutex over foreign AddRef/CopyDescriptors calls.
    g.list=l;s.game_list_pins=1;const auto& v=g.saved;
    g.pins={v.compute.root,v.graphics.root,v.pso,v.rt,v.heaps[0],v.heaps[1],v.predicate,v.shading_image};
    for(UINT i=0;i<g.saved.target_count;++i){auto dst=s.entry_rtvs->GetCPUDescriptorHandleForHeapStart();dst.ptr+=SIZE_T(i)*s.rtv_increment;
        s.device->CopyDescriptorsSimple(1,dst,g.saved.targets[i],D3D12_DESCRIPTOR_HEAP_TYPE_RTV);g.saved.targets[i]=dst;}
    if(g.saved.has_depth){const auto dst=s.entry_dsvs->GetCPUDescriptorHandleForHeapStart();
        s.device->CopyDescriptorsSimple(1,dst,g.saved.depth_target,D3D12_DESCRIPTOR_HEAP_TYPE_DSV);g.saved.depth_target=dst;}
    ++s.game_checkpoints;
}
void InsertionBindings::end_game_call(std::uint64_t call)noexcept{
    auto& g=impl_->game_call;if(!g.call)return;
    // A stale return must not release a different live checkpoint.
    if(g.call!=call||g.thread!=GetCurrentThreadId()){g.error="game-return-identity-mismatch";return;}
    g={};impl_->game_list_pins=0; // Releases pins; never applies bindings on an OFF/aborted call.
}
const char* InsertionBindings::game_call_blocker(std::uint64_t call,std::uint64_t frame,List* l)const noexcept{
    auto& s=*impl_;const auto& g=s.game_call;
    if(s.stopped)return "tracker-stopped";
    if(!g.call||g.call!=call||g.frame!=frame)return "game-entry-call-mismatch";
    if(g.thread!=GetCurrentThreadId())return "game-entry-thread-mismatch";
    if(g.error)return g.error;
    if(g.used)return "game-entry-already-consumed";
    if(g.list.Get()!=l||!g.entry)return "game-entry-list-mismatch";
    auto* e=g.entry;std::unique_lock lock(e->mutex,std::try_to_lock);
    if(!lock.owns_lock()||e->loss||e->list!=l)return "game-entry-recording-lost";
    if(e->state.closed||e->state.generation!=g.saved.generation)return "game-entry-recording-changed";
    // Optional/unsupported operations during SL cannot be silently covered by
    // the earlier snapshot. Preserve the original fail-closed checks.
    if(const auto* why=e->state.blocker())return why;
    return g.saved.blocker();
}
void InsertionBindings::preserving_game_call(std::uint64_t call,std::uint64_t frame,List* l,const std::function<void()>& work){
    if(const auto* why=game_call_blocker(call,frame,l))throw std::logic_error(why);
    auto& s=*impl_;auto& g=s.game_call;auto* e=g.entry;
    if(Impl::injected)throw std::logic_error("Nested game insertion");
    State saved=g.saved;g.used=true;
    // Put checkpoint descriptors in the normal recording-owned slots before
    // publishing restored shadow state. The next checkpoint can then reuse its
    // own slots without changing this recording's descriptor snapshots.
    for(UINT i=0;i<saved.target_count;++i){auto dst=e->rtv_copy;dst.ptr+=SIZE_T(i)*s.rtv_increment;
        s.device->CopyDescriptorsSimple(1,dst,saved.targets[i],D3D12_DESCRIPTOR_HEAP_TYPE_RTV);saved.targets[i]=dst;}
    if(saved.has_depth){s.device->CopyDescriptorsSimple(1,e->dsv_copy,saved.depth_target,D3D12_DESCRIPTOR_HEAP_TYPE_DSV);saved.depth_target=e->dsv_copy;}
    Impl::injected=&s;Impl::injected_list=l;Impl::injected_restore=&saved;s.injected_fault=false;
    std::exception_ptr failure;
    try{l->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);work();}catch(...){failure=std::current_exception();}
    try{saved.restore(l);}catch(...){if(!failure)failure=std::current_exception();}
    Impl::injected=nullptr;Impl::injected_list=nullptr;Impl::injected_restore=nullptr;
    {std::unique_lock lock(e->mutex,std::try_to_lock);
        if(!lock.owns_lock()||e->loss||e->list!=l||e->state.closed||e->state.generation!=saved.generation||s.injected_fault){
            e->loss=true;throw std::logic_error("Game checkpoint changed during insertion");}
        e->state=saved;
    }
    ++s.restores;++s.game_restores;if(failure)std::rethrow_exception(failure);
}
void InsertionBindings::tolerate_unknown_indirect(bool on)noexcept{impl_->tolerate_unknown_indirect=on;}
InsertionBindings::UnknownIndirect InsertionBindings::unknown_indirect()const noexcept{
    return {impl_->tolerate_unknown_indirect.load(),impl_->unknown_indirect_calls.load()};}
InsertionBindings::HeapOrderStats InsertionBindings::heap_order_stats()const noexcept{
    return {impl_->heap_permutations.load(),impl_->heap_permuted_insertions.load()};
}
json InsertionBindings::status()const{auto& s=*impl_;unsigned count=0;for(auto& e:s.entries)count+=e.list!=nullptr;
    unsigned distinct=0;for(unsigned i=0;i<insertions::Count;++i)distinct+=s.targets[0][i]!=s.targets[1][i];
    return {{"enabled",true},{"tracked_lists",count},{"list_capacity",s.entries.size()},{"capacity_scope","concurrent-recordings-not-lifetime-lists"},
        {"pinned_game_lists",s.game_list_pins.load()},
        {"unknown_indirect_policy",s.tolerate_unknown_indirect.load()?"assume-untouched (late attach)":"strict"},{"unknown_indirect_calls",s.unknown_indirect_calls.load()},{"recording_begins",s.begins.load()},{"creation_begins",s.created.load()},{"recycled",s.recycled.load()},
        {"active_recordings",s.recordings.load()},{"peak_recordings",s.peak.load()},{"capacity_refusals",s.overflows.load()},{"attachment_failures",s.attachment_failures.load()},
        {"fixed_storage_bytes",sizeof(Impl)},{"method_lifecycle_variants",distinct},{"method_implementations",insertions::Count+distinct},{"restores",s.restores.load()},{"setters",s.setters.load()},{"losses",s.losses.load()},
        {"stopped",s.stopped.load()},{"event_files",0},{"scope","source-integrated-D3D12-binding-block-not-resource-history"},
        {"state_block","lab-reshade-derived-bindings-v3"},{"graphics_root_arguments",true},{"graphics_fixed_bindings",true},
        {"heap_order_permutations",s.heap_permutations.load()},{"heap_permuted_insertions",s.heap_permuted_insertions.load()},
        {"game_entry_checkpoints",s.game_checkpoints.load()},{"game_entry_restores",s.game_restores.load()},
        {"cpu_target_descriptors_snapshotted",true},{"cpu_target_descriptor_capacity",s.entries.size()*9+9},
        {"cpu_target_descriptor_copies",s.target_copies.load()},{"callback_descriptor_allocations",0}};}
void InsertionBindings::stop()noexcept{impl_->stopped=true;}
void InsertionBindings::uninstall_for_test(){stop();impl_->creation_hook.uninstall_quiesced_for_test();impl_->hooks.uninstall_quiesced_for_test();
    for(auto& e:impl_->entries)if(auto* l=e.list.load())impl_->detach(l);if(Impl::active==impl_)Impl::active=nullptr;}
}
