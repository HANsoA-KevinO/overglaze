// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_present_queue_witness.hpp"
#include <MinHook.h>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>
namespace lab {
using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned MaxBuffers=8,MaxMarks=32,MaxWaits=32;
std::string hex(const void* p){char b[24];std::snprintf(b,sizeof(b),"0x%llx",static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));return b;}
}
struct PresentQueueWitness::Impl {
    inline static std::atomic<Impl*> active{nullptr};
    inline static std::array<void*,4> originals{};
    std::array<void*,4> targets{};
    ComPtr<ID3D12Device> device;Policy policy;
    // Raw identities of the adopted chain's back buffers, refreshed every
    // present. Compared, never dereferenced, and never AddRef'd: a held
    // reference would make the game's ResizeBuffers fail.
    std::array<std::atomic<void*>,MaxBuffers> buffers{};
    std::atomic<bool> armed{false};std::atomic<unsigned> pending{0};
    mutable std::mutex mutex;
    // A recorded, not yet executed, transition of a back buffer into PRESENT.
    struct Mark{void* list=nullptr;void* resource=nullptr;};
    std::array<Mark,MaxMarks> marks{};
    // The last execution of such a transition, per back buffer. Consumed by the
    // present of that buffer.
    struct Record{void* resource=nullptr;ComPtr<ID3D12CommandQueue> queue;std::uint64_t seq=0;bool fresh=false,conflict=false;};
    std::array<Record,MaxBuffers> records{};
    // GPU waits issued by DIRECT queues, newest last.
    struct Waited{ID3D12CommandQueue* queue=nullptr;std::uint64_t seq=0;};
    std::array<Waited,MaxWaits> waits{};std::uint64_t waits_seen=0;
    std::uint64_t seq=0;
    ComPtr<ID3D12CommandQueue> candidate,decided;unsigned streak=0;
    std::uint64_t presents=0,accepted=0,queue_changes=0,contradictions=0,legacy_marks=0,enhanced_marks=0,mark_overflow=0,buffer_failures=0;
    // Per decision: contradictions since it was made. Over the process: how
    // often a decision was made, restarted after a contradiction, refused a
    // restart past the bound, and reset for a new followed chain.
    std::uint64_t since_decision=0,decisions=0,redecisions=0,refused_redecisions=0,rebinds=0;
    std::vector<std::uint64_t> restarts_at; // within the window only
    std::map<std::string,std::uint64_t> misses;std::string last_miss;
    bool enhanced_hooked=false;std::string enhanced_note;json hooks=json::array();

    bool back_buffer(const void* r)const noexcept{
        if(!r)return false;for(const auto& b:buffers)if(b.load(std::memory_order_relaxed)==r)return true;return false;}
    void mark(void* list,void* resource,bool enhanced)noexcept{
        std::lock_guard lock(mutex);
        for(const auto& m:marks)if(m.list==list&&m.resource==resource)return;
        for(auto& m:marks)if(!m.list){m={list,resource};pending.fetch_add(1,std::memory_order_relaxed);++(enhanced?enhanced_marks:legacy_marks);return;}
        ++mark_overflow;
    }
    void record(void* resource,ID3D12CommandQueue* queue){
        Record* slot=nullptr;
        for(auto& r:records)if(r.resource==resource){slot=&r;break;}
        if(!slot)for(auto& r:records)if(!r.resource){slot=&r;break;}
        if(!slot){slot=&records[0];for(auto& r:records)if(r.seq<slot->seq)slot=&r;*slot={};} // a resized-away buffer
        // Two queues executing a transition of one buffer before its present:
        // which one presents is exactly what cannot be told apart.
        if(slot->resource==resource&&slot->fresh&&slot->queue.Get()!=queue)slot->conflict=true;
        slot->resource=resource;slot->queue=queue;slot->seq=++seq;slot->fresh=true;
    }
    void miss(const char* why){++misses[why];last_miss=why;streak=0;}
    void contradiction(){++contradictions;++since_decision;}
    // Nothing decided, no run under way, and no transition executed before
    // this moment may count towards the next run.
    void clear_decision(){decided.Reset();candidate.Reset();streak=0;since_decision=0;for(auto& r:records){r.fresh=false;r.conflict=false;}}

    static void STDMETHODCALLTYPE barrier(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* b){
        auto* s=active.load(std::memory_order_acquire);
        if(s&&s->armed.load(std::memory_order_relaxed)&&b)
            for(UINT i=0;i<count;++i)
                if(b[i].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&b[i].Transition.StateAfter==D3D12_RESOURCE_STATE_PRESENT&&
                   s->back_buffer(b[i].Transition.pResource))s->mark(list,b[i].Transition.pResource,false);
        reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*)>(originals[0])(list,count,b);
    }
    static void STDMETHODCALLTYPE enhanced(ID3D12GraphicsCommandList7* list,UINT32 groups,const D3D12_BARRIER_GROUP* g){
        auto* s=active.load(std::memory_order_acquire);
        if(s&&s->armed.load(std::memory_order_relaxed)&&g)
            for(UINT32 i=0;i<groups;++i)if(g[i].Type==D3D12_BARRIER_TYPE_TEXTURE&&g[i].pTextureBarriers)
                for(UINT32 j=0;j<g[i].NumBarriers;++j){const auto& t=g[i].pTextureBarriers[j];
                    if(t.LayoutAfter==D3D12_BARRIER_LAYOUT_PRESENT&&s->back_buffer(t.pResource))s->mark(list,t.pResource,true);}
        reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*,UINT32,const D3D12_BARRIER_GROUP*)>(originals[1])(list,groups,g);
    }
    static void STDMETHODCALLTYPE execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists){
        auto* s=active.load(std::memory_order_acquire);
        if(s&&s->pending.load(std::memory_order_relaxed)&&lists&&count<=256){std::lock_guard lock(s->mutex);
            for(auto& m:s->marks)if(m.list)for(UINT i=0;i<count;++i)
                if(static_cast<void*>(lists[i])==m.list){s->record(m.resource,queue);m={};s->pending.fetch_sub(1,std::memory_order_relaxed);break;}}
        reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*)>(originals[2])(queue,count,lists);
    }
    static HRESULT STDMETHODCALLTYPE wait(ID3D12CommandQueue* queue,ID3D12Fence* fence,UINT64 value){
        auto* s=active.load(std::memory_order_acquire);
        if(s&&s->armed.load(std::memory_order_relaxed)&&queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT){std::lock_guard lock(s->mutex);
            s->waits[s->waits_seen++%MaxWaits]={queue,++s->seq};}
        return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,ID3D12Fence*,UINT64)>(originals[3])(queue,fence,value);
    }
};

PresentQueueWitness* PresentQueueWitness::install(ID3D12Device* device,Policy policy){
    if(!device||!policy.streak)throw std::logic_error("Present-queue witness needs the game's device and a non-zero streak");
    auto* s=new Impl;s->device=device;s->policy=policy;
    D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    if(FAILED(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)))||FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)))||FAILED(list->Close()))
        throw std::runtime_error("Present-queue witness method-discovery objects"); // never submitted
    const auto slots=latebind::witness_slots();auto** cv=*reinterpret_cast<void***>(list.Get());auto** qv=*reinterpret_cast<void***>(queue.Get());
    s->targets={cv[slots[0]],nullptr,qv[slots[2]],qv[slots[3]]};
    // Enhanced barriers are hooked only where the list's version-7 interface is
    // the same object pointer ExecuteCommandLists will be handed; otherwise a
    // mark could never be matched and would only look like missing evidence.
    ComPtr<ID3D12GraphicsCommandList7> seven;
    if(FAILED(list.As(&seven)))s->enhanced_note="runtime has no ID3D12GraphicsCommandList7";
    else if(static_cast<void*>(seven.Get())!=static_cast<void*>(list.Get()))s->enhanced_note="ID3D12GraphicsCommandList7 is a different pointer";
    else{s->targets[1]=(*reinterpret_cast<void***>(seven.Get()))[slots[1]];s->enhanced_note="hooked";}
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)if(s->targets[i]&&s->targets[i]==s->targets[j])throw std::runtime_error("Present-queue witness hook targets alias");
    Impl* empty=nullptr;if(!Impl::active.compare_exchange_strong(empty,s))throw std::logic_error("One present-queue witness per process");
    try{
        HMODULE pin{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Impl::execute),&pin))
            throw std::runtime_error("Pin present-queue witness code");
        const auto init=MH_Initialize();if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("MinHook unavailable");
        const std::array<void*,4> callbacks{reinterpret_cast<void*>(&Impl::barrier),reinterpret_cast<void*>(&Impl::enhanced),
                                            reinterpret_cast<void*>(&Impl::execute),reinterpret_cast<void*>(&Impl::wait)};
        for(unsigned i=0;i<4;++i){if(!s->targets[i])continue;
            const auto r=MH_CreateHook(s->targets[i],callbacks[i],&Impl::originals[i]);
            s->hooks.push_back({{"slot",i},{"vtable_slot",slots[i]},{"address",reinterpret_cast<std::uint64_t>(s->targets[i])},{"create",static_cast<int>(r)}});
            if(r!=MH_OK){for(unsigned j=0;j<i;++j)if(s->targets[j])MH_RemoveHook(s->targets[j]);throw std::runtime_error("Present-queue witness hook create failed");}
        }
        // Enable ExecuteCommandLists and Wait before the barrier hooks, so no mark
        // is ever made that its execution could slip past.
        for(unsigned i:{2u,3u,0u,1u}){if(!s->targets[i])continue;const auto r=MH_EnableHook(s->targets[i]);
            for(auto& h:s->hooks)if(h["slot"]==i)h["enable"]=static_cast<int>(r);
            if(r!=MH_OK)throw std::runtime_error("Present-queue witness partial enable retained");}
        s->enhanced_hooked=s->targets[1]!=nullptr;
    }catch(...){Impl::active.store(nullptr);throw;} // an enabled hook forwards with no active witness
    return new PresentQueueWitness(s);
}

PresentQueueWitness::BackBuffers PresentQueueWitness::name(IDXGISwapChain3* chain)noexcept{
    BackBuffers b;
    try{DXGI_SWAP_CHAIN_DESC1 d{};
        if(!chain||FAILED(chain->GetDesc1(&d))||!d.BufferCount||d.BufferCount>b.identity.size())return b;
        for(UINT i=0;i<d.BufferCount;++i){ComPtr<ID3D12Resource> r;if(FAILED(chain->GetBuffer(i,IID_PPV_ARGS(&r)))||!r)return b;b.identity[i]=r.Get();}
        b.count=d.BufferCount;b.current=chain->GetCurrentBackBufferIndex();b.named=true;
    }catch(...){b.named=false;}
    return b;
}

void PresentQueueWitness::present(const BackBuffers& b)noexcept{
    auto& s=*impl_;
    try{
        const bool named=b.named&&b.count>0&&b.count<=MaxBuffers;
        if(named)for(unsigned i=0;i<MaxBuffers;++i)s.buffers[i].store(i<b.count?const_cast<void*>(b.identity[i]):nullptr,std::memory_order_relaxed);
        // Until one present named the back buffers, no barrier could be matched.
        const bool first=named&&!s.armed.exchange(true);
        std::lock_guard lock(s.mutex);++s.presents;
        if(!named){++s.buffer_failures;s.miss("back-buffers-not-nameable");return;}
        if(first){s.miss("first-present-names-the-back-buffers");return;}
        if(b.current>=b.count){s.miss("current-index-out-of-range");return;}
        Impl::Record* r=nullptr;for(auto& x:s.records)if(x.resource==b.identity[b.current]){r=&x;break;}
        const char* why=nullptr;bool contradicts=false;
        if(!r||!r->fresh)why="no-present-transition-for-current-buffer";
        else if(r->conflict){why="two-queues-transitioned-the-buffer";contradicts=true;}
        else{
            ComPtr<ID3D12Device> device;
            if(r->queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT||FAILED(r->queue->GetDevice(IID_PPV_ARGS(&device)))||device.Get()!=s.device.Get()){
                why="transition-queue-not-direct-on-this-device";contradicts=true;}
            // The wait history only proves "no foreign wait" back to its oldest entry.
            else if(s.waits_seen>MaxWaits&&s.waits[s.waits_seen%MaxWaits].seq>r->seq)why="wait-history-overflow";
            else for(const auto& w:s.waits)if(w.queue&&w.seq>r->seq&&w.queue!=r->queue.Get()){why="other-direct-queue-waited-before-present";contradicts=true;break;}
        }
        ComPtr<ID3D12CommandQueue> queue;if(r){queue=r->queue;r->fresh=false;r->conflict=false;}
        if(why){s.miss(why);if(s.decided&&contradicts)s.contradiction();return;}
        ++s.accepted;
        if(s.decided){if(queue!=s.decided)s.contradiction();return;}
        if(s.candidate==queue)++s.streak;else{if(s.candidate)++s.queue_changes;s.candidate=queue;s.streak=1;}
        if(s.streak>=s.policy.streak){s.decided=s.candidate;s.since_decision=0;++s.decisions;}
    }catch(...){}
}

ComPtr<ID3D12CommandQueue> PresentQueueWitness::decided()const{std::lock_guard lock(impl_->mutex);return impl_->decided;}
std::uint64_t PresentQueueWitness::contradictions()const noexcept{std::lock_guard lock(impl_->mutex);return impl_->contradictions;}
bool PresentQueueWitness::contradicted()const noexcept{std::lock_guard lock(impl_->mutex);return impl_->decided&&impl_->since_decision>0;}
bool PresentQueueWitness::restart(std::uint64_t now)noexcept{
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    // A clock read earlier than a recorded restart also falls out of the window.
    std::erase_if(s.restarts_at,[&](std::uint64_t t){return now<t||now-t>=s.policy.restart_window_ms;});
    if(s.restarts_at.size()>=s.policy.restarts){++s.refused_redecisions;return false;}
    s.restarts_at.push_back(now);++s.redecisions;s.clear_decision();return true;
}
void PresentQueueWitness::rebind()noexcept{
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    s.clear_decision();for(auto& r:s.records)r=Impl::Record{};for(auto& m:s.marks)m={};s.pending.store(0,std::memory_order_relaxed);
    // The next present names the new chain's buffers and arms the hooks again;
    // until then no barrier can be matched (first-present-names-the-back-buffers).
    for(auto& b:s.buffers)b.store(nullptr,std::memory_order_relaxed);s.armed.store(false,std::memory_order_relaxed);++s.rebinds;
}
json PresentQueueWitness::status()const{
    const auto& s=*impl_;std::lock_guard lock(s.mutex);
    json misses=json::object();for(const auto& [k,v]:s.misses)misses[k]=v;
    return {{"state",s.decided?"decided":"watching"},{"required_streak",s.policy.streak},{"streak",s.streak},
        {"presents",s.presents},{"accepted_frames",s.accepted},{"misses",misses},{"last_miss",s.last_miss},
        {"queue_changes",s.queue_changes},{"contradictions",s.contradictions},{"contradictions_since_decision",s.since_decision},
        {"decisions",s.decisions},{"redecisions",s.redecisions},{"refused_redecisions",s.refused_redecisions},{"rebinds",s.rebinds},
        {"redecision_bound",{{"restarts",s.policy.restarts},{"window_ms",s.policy.restart_window_ms}}},
        {"marks",{{"legacy",s.legacy_marks},{"enhanced",s.enhanced_marks},{"overflow",s.mark_overflow}}},
        {"direct_queue_waits",s.waits_seen},{"enhanced_barriers",s.enhanced_note},
        {"decided_queue",s.decided?json(hex(s.decided.Get())):json(nullptr)},{"hooks",s.hooks}};
}
void PresentQueueWitness::uninstall_for_test(){
    for(auto* t:impl_->targets)if(t){MH_DisableHook(t);MH_RemoveHook(t);}
    Impl::active.store(nullptr);Impl::originals={};
}
}
