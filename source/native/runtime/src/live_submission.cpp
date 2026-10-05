// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_live_submission.hpp"
#include "lab_resume_slots.hpp"
#include <MinHook.h>
#include <atomic>
#include <mutex>
namespace lab {
using Microsoft::WRL::ComPtr;
namespace {using List=ID3D12GraphicsCommandList;
// Public ReShade 6.8 com_utils.hpp unwrap interface, not a memory offset.
constexpr GUID unwrap={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
void checked(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Live submission D3D12 call failed");}
}
struct LiveSubmission::Impl {
    inline static std::atomic<Impl*> active{nullptr};
    inline static std::array<void*,3> originals{};
    inline static thread_local unsigned reset_depth=0;
    std::array<void*,3> targets{};
    std::atomic<InsertionBindings*> bindings{nullptr};
    ComPtr<ID3D12Device> device;ComPtr<List> list;ComPtr<ID3D12CommandQueue> queue;
    std::atomic<List*> watched{nullptr};HMODULE proxy=nullptr;
    mutable std::mutex mutex;
    bool probe=false,closed=false,submitting=false,submitted=false,armed=false,discarded=false;
    std::atomic<bool> fault{false},stopped{false};
    std::atomic<const char*> first_failure{nullptr};
    std::atomic<CompletionTimeline*> timeline{nullptr};CompletionTimeline::Ticket ticket{};
    DWORD thread=0,close_thread=0;std::uint64_t enrollments=0,probes=0,closes=0,submits=0,signals=0,retired=0,discards=0;
    std::atomic<std::uint64_t> losses{0},signal_retries{0};json hooks=json::array();
    void fail(const char* why="submission-abandoned")noexcept{const char* empty=nullptr;first_failure.compare_exchange_strong(empty,why);fault=true;if(auto* t=timeline.load())t->invalidate();}
    template<class F>static F original(unsigned slot){return reinterpret_cast<F>(originals[slot]);}
    static HRESULT STDMETHODCALLTYPE reset(List* list,ID3D12CommandAllocator* a,ID3D12PipelineState* p){
        // Debug/runtime wrappers may re-enter this same implementation on an
        // internal list. Track the outer API object only, just as Create does;
        // otherwise its hidden child consumes a second recording slot.
        struct Scope{Scope(){++reset_depth;}~Scope(){--reset_depth;}} scope;
        auto* s=active.load();
        if(s&&!s->stopped&&s->watched==list){std::unique_lock lock(s->mutex,std::try_to_lock);
            if(!lock.owns_lock()){++s->losses;s->fail("reset-lock-contention");}
            else if(!s->submitted){
                // Reset destroys the recording. For an ARMED NR frame that is
                // proof the game abandoned the frame (a dropped frame at a scene
                // change), not execution of unknown work: hand the ticket to the
                // timeline as discarded and stop watching the list, whose next
                // contents are a new recording. Probe lists keep their own
                // recovery; everything else stays fatal, and a busy timeline
                // lock is named as such rather than mistaken for proof.
                auto* t=s->timeline.load();auto verdict=CompletionTimeline::Discard::refused;
                if(!s->probe&&s->armed&&!s->submitting&&!s->fault&&t)
                    for(unsigned spin=0;spin<256;++spin){verdict=t->discard_before_submission(s->ticket);if(verdict!=CompletionTimeline::Discard::busy)break;YieldProcessor();}
                if(verdict==CompletionTimeline::Discard::discarded){s->discarded=true;++s->discards;s->watched=nullptr;}
                else s->fail(verdict==CompletionTimeline::Discard::busy?"reset-before-submission-timeline-busy":"reset-before-submission"); // Never submit a replaced recording as old NR work.
            }
            else s->watched=nullptr; // Reset starts a NEW recording, not another execution of old work.
        }
        const auto hr=original<HRESULT(STDMETHODCALLTYPE*)(List*,ID3D12CommandAllocator*,ID3D12PipelineState*)>(0)(list,a,p);
        if(reset_depth==1&&s)if(auto* b=s->bindings.load())b->reset(list,hr,p);
        return hr;
    }
    static HRESULT STDMETHODCALLTYPE close(List* list){
        const auto hr=original<HRESULT(STDMETHODCALLTYPE*)(List*)>(1)(list);auto* s=active.load();
        if(s&&!s->stopped&&s->watched==list){std::unique_lock lock(s->mutex,std::try_to_lock);
            if(!lock.owns_lock()){++s->losses;s->fail("close-lock-contention");}
            else if(!s->submitted){++s->closes;s->close_thread=GetCurrentThreadId();
                // Sequential ownership may move to another CPU thread. D3D12
                // forbids concurrent list mutation, not sequential handoff.
                if(FAILED(hr))s->fail("native-close-failed");else if(s->closed)s->fail("duplicate-close");else s->closed=true;}
        }if(s)if(auto* b=s->bindings.load())b->close(list);return hr;
    }
    static void STDMETHODCALLTYPE execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists){
        auto* s=active.load();bool notify=false;ComPtr<ID3D12CommandQueue> hold;
        CompletionTimeline* timeline=nullptr;CompletionTimeline::Ticket ticket{};
        if(s&&!s->stopped)if(auto* target=s->watched.load()){
            unsigned found=0;if(count<=256&&(!count||lists))for(unsigned i=0;i<count;++i)found+=lists[i]==target;
            else s->fail("submission-array-outside-bound");
            if(found){ // Validate native queue outside our lock/foreign Execute.
                ComPtr<ID3D12Device> device;
                const bool valid=queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT&&
                    SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))&&device.Get()==s->device.Get()&&
                    (*reinterpret_cast<void***>(queue))[resume::slots()[resume::Execute]]==s->targets[2];
                if(valid)hold=queue;
                std::unique_lock lock(s->mutex,std::try_to_lock);
                if(!lock.owns_lock()){++s->losses;s->fail("execute-entry-lock-contention");}
                else if(s->watched==target){
                    if(!valid)s->fail("queue-device-type-or-method-mismatch");
                    else if(found!=1)s->fail("duplicate-list-in-submission");
                    else if(!s->closed)s->fail("close-not-observed");
                    else if(s->submitting)s->fail("overlapping-submission");
                    else if(s->submitted)s->fail("repeated-recording-submission");
                    else if(s->fault)s->fail("earlier-submission-fault");
                    else if(!s->probe&&!s->armed)s->fail("completion-not-armed");
                    else if(!s->probe&&s->queue.Get()!=queue)s->fail("unexpected-render-queue");
                    else{s->submitting=true;notify=true;timeline=s->timeline.load();ticket=s->ticket;}
                }
            }
        }
        original<void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*)>(2)(queue,count,lists);
        if(notify){std::unique_lock lock(s->mutex,std::try_to_lock);
            if(!lock.owns_lock()){++s->losses;s->fail("execute-return-lock-contention");return;}
            if(s->fault||s->stopped){s->fail("stopped-during-submit");return;}
            if(s->probe){if(s->queue&&s->queue.Get()!=queue){s->fail("probe-queue-changed");return;}s->queue=std::move(hold);++s->probes;}
            else if(!timeline){s->fail("completion-timeline-gone");return;}
            else{
                // The work is already on the GPU: the original ExecuteCommandLists
                // ran above. There is no "skip this frame" available here -- the
                // fence MUST be signalled or the scratch can never be reclaimed.
                // So a try-lock miss against the polling owner is retried rather
                // than declared fatal, and every other reason is named. The
                // timeline holder never waits on this mutex, so the spin ends.
                auto why=CompletionTimeline::Signal::busy;
                for(unsigned spin=0;spin<64;++spin){
                    why=timeline->submit_signal(ticket,queue);
                    if(why!=CompletionTimeline::Signal::busy)break;
                    ++s->signal_retries;SwitchToThread();
                }
                if(why!=CompletionTimeline::Signal::ok){
                    s->fail(CompletionTimeline::signal_name(why));return;
                }
                ++s->signals;
            }
            ++s->submits;s->submitted=true;s->submitting=false;
        }
    }
};
LiveSubmission* LiveSubmission::install(ID3D12CommandQueue* seed,HMODULE proxy){
    if(!seed||seed->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)throw std::logic_error("Native DIRECT seed required");
    auto* s=new Impl;auto* host=new LiveSubmission(s);s->proxy=proxy;
    checked(seed->GetDevice(IID_PPV_ARGS(&s->device)));
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<List> method;
    checked(s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    checked(s->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&method)));
    checked(method->Close()); // Private method-discovery list is never submitted.
    const auto slots=resume::slots();auto** cv=*reinterpret_cast<void***>(method.Get());auto** qv=*reinterpret_cast<void***>(seed);
    s->targets={cv[slots[resume::Reset]],cv[slots[resume::Close]],qv[slots[resume::Execute]]};
    Impl* empty=nullptr;if(!Impl::active.compare_exchange_strong(empty,s))throw std::logic_error("Only one live submission router");
    try{
        HMODULE pin{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Impl::execute),&pin))throw std::runtime_error("Pin native hook code");
        const auto init=MH_Initialize();if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("Native hook init failed");
        const std::array<void*,3> callbacks{reinterpret_cast<void*>(Impl::reset),reinterpret_cast<void*>(Impl::close),reinterpret_cast<void*>(Impl::execute)};
        for(unsigned i=0;i<3;++i){const auto r=MH_CreateHook(s->targets[i],callbacks[i],&Impl::originals[i]);
            s->hooks.push_back({{"slot",i},{"address",reinterpret_cast<UINT64>(s->targets[i])},{"create",r}});
            if(r!=MH_OK)throw std::runtime_error("Native hook conflict");}
        for(unsigned i=0;i<3;++i){const auto r=MH_EnableHook(s->targets[i]);s->hooks[i]["enable"]=r;if(r!=MH_OK)throw std::runtime_error("Native hook enable failed");}
        return host;
    }catch(...){s->stopped=true;s->fault=true;throw;}
}
ComPtr<List> LiveSubmission::resolve(IUnknown* source)const{
    ComPtr<List> list;if(!source)return list;const auto slots=resume::slots();
    const auto match=[&](List* p){if(!p)return false;auto** v=*reinterpret_cast<void***>(p);return v[slots[resume::Reset]]==impl_->targets[0]&&v[slots[resume::Close]]==impl_->targets[1];};
    if(SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&list)))&&match(list.Get()))return list;
    list.Reset();MEMORY_BASIC_INFORMATION page{};auto* query=(*reinterpret_cast<void***>(source))[0];
    if(!impl_->proxy||!VirtualQuery(query,&page,sizeof(page))||page.AllocationBase!=impl_->proxy)return list;
    ComPtr<IUnknown> base;
    if(FAILED(source->QueryInterface(unwrap,reinterpret_cast<void**>(base.GetAddressOf())))||!base||FAILED(base.As(&list))||!match(list.Get()))list.Reset();
    return list;
}
InsertionBindings* LiveSubmission::enable_binding_preservation(){
    auto& s=*impl_;if(s.proxy||s.stopped||s.fault||s.enrollments||s.bindings)throw std::logic_error("Enable native preservation before enrollment only");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<List> method;
    checked(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    checked(s.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&method)));
    checked(method->Close());
    // Agility runtimes can switch setter implementations on Reset. Discover
    // both lifecycle tables on private, never-submitted lists in this worker.
    // Do not assume a closed discovery list describes an active game recording.
    ComPtr<ID3D12CommandAllocator> reset_allocator;ComPtr<List> reset_method;
    checked(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&reset_allocator)));
    checked(s.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,reset_allocator.Get(),nullptr,IID_PPV_ARGS(&reset_method)));
    checked(reset_method->Close());checked(reset_method->Reset(reset_allocator.Get(),nullptr));
    auto* b=InsertionBindings::install(method.Get(),reset_method.Get());s.bindings=b;checked(reset_method->Close());return b;
}
bool LiveSubmission::enroll(List* list,bool probe){
    if(!list||impl_->stopped||impl_->fault)return false;
    auto resolved=resolve(list);ComPtr<ID3D12Device> device;
    if(resolved.Get()!=list||list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT||FAILED(list->GetDevice(IID_PPV_ARGS(&device)))||device.Get()!=impl_->device.Get())return false;
    ComPtr<List> old;
    std::unique_lock lock(impl_->mutex,std::try_to_lock);if(!lock.owns_lock()||impl_->stopped||impl_->fault||impl_->list||(!probe&&!impl_->queue))return false;
    auto& s=*impl_;old=std::move(s.list);s.list=std::move(resolved);s.probe=probe;s.closed=s.submitting=s.submitted=s.armed=s.discarded=false;
    s.thread=GetCurrentThreadId();++s.enrollments;s.watched=list;return true;
}
ComPtr<ID3D12CommandQueue> LiveSubmission::observed_queue()const{
    std::lock_guard lock(impl_->mutex);return !impl_->stopped&&!impl_->fault?impl_->queue:ComPtr<ID3D12CommandQueue>{};
}
bool LiveSubmission::arm(CompletionTimeline& t,CompletionTimeline::Ticket ticket){
    auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock.owns_lock()||s.stopped||s.fault||!s.watched||s.probe||s.closed||s.armed||s.thread!=GetCurrentThreadId()||t.inspect(ticket)!=CompletionTimeline::State::recorded)return false;
    s.ticket=ticket;s.timeline=&t;s.armed=true;return true;
}
bool LiveSubmission::release_completed(){
    ComPtr<List> release;auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock.owns_lock()||s.fault)return false;if(!s.list)return true;
    if(!s.submitted||s.submitting||(!s.probe&&(!s.armed||s.timeline.load()->inspect(s.ticket)!=CompletionTimeline::State::invalid)))return false;
    s.watched=nullptr;s.timeline=nullptr;s.armed=false;s.ticket={};release=std::move(s.list);++s.retired;return true;
}
bool LiveSubmission::release_discarded(){
    ComPtr<List> release;auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock.owns_lock()||s.fault||!s.discarded||!s.list||s.probe)return false;
    auto* t=s.timeline.load();if(!t||t->inspect(s.ticket)!=CompletionTimeline::State::invalid)return false; // owner retires the ticket first
    s.discarded=false;s.watched=nullptr;s.timeline=nullptr;s.armed=false;s.closed=false;s.ticket={};release=std::move(s.list);return true;
}
std::uint64_t LiveSubmission::signal_retries()const noexcept{return impl_->signal_retries.load();}
const char* LiveSubmission::first_failure()const noexcept{return impl_->first_failure.load();}
bool LiveSubmission::forget_queue_for_rebuild(){
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(s.fault||s.stopped||s.list||s.watched||s.timeline||s.submitting)return false;
    s.queue.Reset();return true;
}
void LiveSubmission::abandon()noexcept{impl_->fail();}
bool LiveSubmission::retry_discarded_probe(){
    auto& s=*impl_;ComPtr<List> release;
    {std::lock_guard lock(s.mutex);const auto why=s.first_failure.load();
        if(!s.probe||s.submitted||s.submitting||!s.fault||!why||std::string_view(why)!="reset-before-submission")return false;
        s.watched=nullptr;release=std::move(s.list);s.closed=s.armed=false;s.first_failure=nullptr;s.fault=false;}
    return true;
}
void LiveSubmission::stop()noexcept{impl_->stopped=true;if(auto* b=impl_->bindings.load())b->stop();}
json LiveSubmission::snapshot()const{auto& s=*impl_;
    struct Copy{std::uint64_t enrollments,probes,closes,submits,signals,retired,discards;DWORD thread,close_thread;bool queue,pending,submitted,probe,armed,discarded;} v;
    {std::lock_guard lock(s.mutex);v={s.enrollments,s.probes,s.closes,s.submits,s.signals,s.retired,s.discards,s.thread,s.close_thread,
        s.queue!=nullptr,s.list!=nullptr,s.submitted,s.probe,s.armed,s.discarded};}
    // JSON allocation/copy is never performed while callbacks need the lock.
    const char* why=s.first_failure.load();
    return {{"profile","live-single-pending-recording"},{"enrollment_source","matched-RR-outer-return-not-inferred-reset"},{"enrollments",v.enrollments},
        {"probes",v.probes},{"closes",v.closes},{"submits",v.submits},{"signals",v.signals},{"retired",v.retired},{"callback_losses",s.losses.load()},{"signal_retries",s.signal_retries.load()},
        {"record_thread",v.thread},{"close_thread",v.close_thread},{"failure_reason",why?json(why):json(nullptr)},
        {"queue_observed",v.queue},{"pending",v.pending},{"submitted",v.submitted},{"probe",v.probe},{"armed",v.armed},{"discarded",v.discarded},{"discards",v.discards},
        {"discard_scope","armed NR recording whose list the game Reset before any ExecuteCommandLists; nothing executed; owner retires the ticket then releases the lease"},{"terminal_fault",s.fault.load()},
        {"stopped",s.stopped.load()},{"hooks",s.hooks},{"event_files",0},{"state_restoration",false}};
}
void LiveSubmission::uninstall_for_test(){stop();if(auto* b=impl_->bindings.load())b->uninstall_for_test();for(auto* t:impl_->targets){MH_DisableHook(t);MH_RemoveHook(t);}if(Impl::active==impl_)Impl::active=nullptr;}
}
