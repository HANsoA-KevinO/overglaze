// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_native_resume.hpp"
#include "lab_resume_slots.hpp"
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace lab::resume {
using Microsoft::WRL::ComPtr;
using List=ID3D12GraphicsCommandList;
struct Context::Impl {
    inline static std::atomic<Impl*> active{nullptr};
    inline static thread_local Impl* injected=nullptr;
    inline static std::array<void*,Count> originals{};
    std::array<void*,Count> targets{};
    ComPtr<List> list;ComPtr<ID3D12CommandQueue> queue;
    struct RootInfo {ComPtr<ID3D12RootSignature> root;std::array<ComputeBindings::Parameter,64> layout{};unsigned count=0;};
    std::array<RootInfo,8> roots{};unsigned root_count=0;
    mutable std::mutex mutex;
    ComputeBindings shadow;
    std::uint64_t generation=0,setters=0,resets=0,closes=0,restores=0,unsupported=0,matched=0,signals=0;
    DWORD recording_thread=0;
    bool closed=true,invalid=true,armed=false,submitting=false,submitted=false;
    bool completion_only=false;
    std::uint64_t armed_generation=0;
    std::atomic<CompletionTimeline*> timeline{nullptr};CompletionTimeline::Ticket ticket{};
    std::atomic<bool> stopped{false},fault{false};
    std::atomic<std::uint64_t> losses{0},injected_setters{0};
    json hooks=json::array();
    // Host keeps an armed timeline alive until all native callers quiesce.
    void fatal() noexcept {fault=true;if(auto* t=timeline.load())t->invalidate();}
    template<class Fn> static Fn original(Slot slot){return reinterpret_cast<Fn>(originals[slot]);}
    template<class Work> static void edit(List* list,Work work,bool skip_injected=true,bool reset=false) noexcept {
        auto* s=active.load();if(!s || s->stopped || list!=s->list.Get())return;
        if(skip_injected && injected==s){++s->injected_setters;return;}
        std::unique_lock lock(s->mutex,std::try_to_lock);
        if(!lock.owns_lock()){s->fatal();++s->losses;return;}
        if(!reset && (GetCurrentThreadId()!=s->recording_thread || s->closed)){s->fatal();return;}
        try {work(*s);}catch(...){s->invalid=true;s->shadow.invalidate();}
    }
    template<class Work> static void setter(List* l,Work work) noexcept {
        edit(l,[&](Impl& s){++s.setters;work(s);});
    }
    static void reject(List* l) noexcept {edit(l,[](Impl& s){++s.unsupported;s.invalid=true;s.shadow.invalidate();},false);}
    static HRESULT STDMETHODCALLTYPE reset(List* l,ID3D12CommandAllocator* a,ID3D12PipelineState* p){
        const auto hr=original<HRESULT(STDMETHODCALLTYPE*)(List*,ID3D12CommandAllocator*,ID3D12PipelineState*)>(Reset)(l,a,p);
        edit(l,[&](Impl& s){++s.resets;
            if(FAILED(hr) || (s.armed&&!s.submitted) || s.generation==UINT64_MAX){s.fatal();return;}
            s.recording_thread=GetCurrentThreadId();s.closed=false;s.invalid=false;s.shadow.reset(l,++s.generation);
            if(p)s.shadow.pipeline(p);
        },false,true);return hr;
    }
    static HRESULT STDMETHODCALLTYPE close(List* l){const auto hr=original<HRESULT(STDMETHODCALLTYPE*)(List*)>(Close)(l);
        edit(l,[&](Impl& s){++s.closes;s.closed=true;if(FAILED(hr)){s.invalid=true;s.fatal();}},false);return hr;}
    static void STDMETHODCALLTYPE pipeline(List* l,ID3D12PipelineState* p){original<void(STDMETHODCALLTYPE*)(List*,ID3D12PipelineState*)>(Pipeline)(l,p);setter(l,[&](Impl& s){s.shadow.pipeline(p);});}
    static void STDMETHODCALLTYPE heaps(List* l,UINT n,ID3D12DescriptorHeap* const* h){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,ID3D12DescriptorHeap* const*)>(Heaps)(l,n,h);
        setter(l,[&](Impl& s){if(n>2 || (n&&!h))throw std::logic_error("Heap bound");s.shadow.heaps({h,n});});}
    static void STDMETHODCALLTYPE root(List* l,ID3D12RootSignature* r){original<void(STDMETHODCALLTYPE*)(List*,ID3D12RootSignature*)>(Root)(l,r);
        setter(l,[&](Impl& s){for(unsigned i=0;i<s.root_count;++i)if(s.roots[i].root.Get()==r){s.shadow.root(r,{s.roots[i].layout.data(),s.roots[i].count});return;}
            throw std::logic_error("Unknown root creation layout");});}
    static void STDMETHODCALLTYPE table(List* l,UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h){original<void(STDMETHODCALLTYPE*)(List*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE)>(Table)(l,i,h);setter(l,[&](Impl& s){s.shadow.table(i,h);});}
    template<Slot slot,ComputeBindings::Kind kind> static void STDMETHODCALLTYPE descriptor(List* l,UINT i,D3D12_GPU_VIRTUAL_ADDRESS a){
        original<void(STDMETHODCALLTYPE*)(List*,UINT,D3D12_GPU_VIRTUAL_ADDRESS)>(slot)(l,i,a);setter(l,[&](Impl& s){s.shadow.descriptor(i,kind,a);});}
    static void STDMETHODCALLTYPE constant(List* l,UINT i,UINT v,UINT offset){original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT,UINT)>(Constant)(l,i,v,offset);
        setter(l,[&](Impl& s){s.shadow.constants(i,offset,{&v,1});});}
    static void STDMETHODCALLTYPE constants(List* l,UINT i,UINT n,const void* v,UINT offset){original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT,const void*,UINT)>(Constants)(l,i,n,v,offset);
        setter(l,[&](Impl& s){if(n>64 || !v)throw std::logic_error("Constants bound");s.shadow.constants(i,offset,{static_cast<const UINT*>(v),n});});}
    static void STDMETHODCALLTYPE clear(List* l,ID3D12PipelineState* p){original<void(STDMETHODCALLTYPE*)(List*,ID3D12PipelineState*)>(ClearState)(l,p);reject(l);}
    static void STDMETHODCALLTYPE graphics(List* l,ID3D12RootSignature* r){original<void(STDMETHODCALLTYPE*)(List*,ID3D12RootSignature*)>(GraphicsRoot)(l,r);reject(l);}
    static void STDMETHODCALLTYPE bundle(List* l,List* b){original<void(STDMETHODCALLTYPE*)(List*,List*)>(Bundle)(l,b);reject(l);}
    static void STDMETHODCALLTYPE indirect(List* l,ID3D12CommandSignature* s,UINT n,ID3D12Resource* a,UINT64 o,ID3D12Resource* c,UINT64 co){
        original<void(STDMETHODCALLTYPE*)(List*,ID3D12CommandSignature*,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64)>(Indirect)(l,s,n,a,o,c,co);reject(l);}
    static void STDMETHODCALLTYPE predication(List* l,ID3D12Resource* r,UINT64 o,D3D12_PREDICATION_OP p){
        original<void(STDMETHODCALLTYPE*)(List*,ID3D12Resource*,UINT64,D3D12_PREDICATION_OP)>(Predication)(l,r,o,p);reject(l);}
    static void STDMETHODCALLTYPE state_object(ID3D12GraphicsCommandList4* l,ID3D12StateObject* s){
        original<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,ID3D12StateObject*)>(StateObject)(l,s);reject(l);}
    static void STDMETHODCALLTYPE draw(List* l,UINT a,UINT b,UINT c,UINT d){original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT,UINT,UINT)>(Draw)(l,a,b,c,d);reject(l);}
    static void STDMETHODCALLTYPE indexed(List* l,UINT a,UINT b,UINT c,INT d,UINT e){original<void(STDMETHODCALLTYPE*)(List*,UINT,UINT,UINT,INT,UINT)>(DrawIndexed)(l,a,b,c,d,e);reject(l);}
    static void STDMETHODCALLTYPE execute(ID3D12CommandQueue* q,UINT n,ID3D12CommandList* const* lists){
        auto* s=active.load();bool notify=false;CompletionTimeline* timeline=nullptr;CompletionTimeline::Ticket ticket{};
        if(s&&!s->stopped){
            std::unique_lock lock(s->mutex,std::try_to_lock);
            if(!lock.owns_lock()){s->fatal();++s->losses;}
            else if(s->armed){
                if(n>64 || (n&&!lists))s->fatal();
                else {
                    unsigned found=0;for(unsigned i=0;i<n;++i)found+=lists[i]==s->list.Get();
                    if(found){++s->matched;
                        if(found!=1 || q!=s->queue.Get() || !s->closed || s->invalid || s->fault || s->submitting || s->submitted || s->generation!=s->armed_generation)s->fatal();
                        else {s->submitting=true;timeline=s->timeline.load();ticket=s->ticket;notify=true;}
                    }
                }
            }
        }
        // Never hold the tracking lock across a foreign API. It has no HRESULT;
        // the fence is still checked for real completion and device removal.
        original<void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*)>(Execute)(q,n,lists);
        if(notify){
            std::unique_lock lock(s->mutex,std::try_to_lock);
            if(!lock.owns_lock()){s->fatal();++s->losses;return;}
            if(s->fault || s->stopped || !s->armed || s->generation!=s->armed_generation || !timeline->submitted(ticket,q)){s->fatal();return;}
            ++s->signals;s->submitted=true;s->submitting=false;
        }
    }
};

Context* Context::install(List* list,ID3D12CommandQueue* queue,bool owned_compute_only_host){
    if(!list || !queue || !owned_compute_only_host)throw std::logic_error("Owned compute-only host contract required");
    return install_impl(list,queue,false);
}
Context* Context::install_completion_only(List* list,ID3D12CommandQueue* queue){return install_impl(list,queue,true);}
Context* Context::install_impl(List* list,ID3D12CommandQueue* queue,bool completion_only){
    if(!list||!queue)throw std::logic_error("Native list/queue required");
    // Native DIRECT may record compute work too. This never certifies its other
    // graphics/render-pass/extension state; game integration stays disabled.
    const auto type=list->GetType();
    if((type!=D3D12_COMMAND_LIST_TYPE_COMPUTE && type!=D3D12_COMMAND_LIST_TYPE_DIRECT) || queue->GetDesc().Type!=type)
        throw std::logic_error("Native compute-capable list/queue types must match");
    ComPtr<ID3D12Device> ld,qd;
    if(FAILED(list->GetDevice(IID_PPV_ARGS(&ld)))||FAILED(queue->GetDevice(IID_PPV_ARGS(&qd)))||ld.Get()!=qd.Get())
        throw std::logic_error("Native list and queue devices must match");
    auto* s=new Impl;auto* context=new Context(s);s->list=list;s->queue=queue;s->completion_only=completion_only;
    Impl* empty=nullptr;if(!Impl::active.compare_exchange_strong(empty,s)){s->stopped=true;throw std::logic_error("One process-pinned native resume context only");}
    try {
        HMODULE module=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Impl::execute),&module))throw std::runtime_error("Cannot pin hook code");
        ComPtr<ID3D12GraphicsCommandList4> v4;if(FAILED(list->QueryInterface(IID_PPV_ARGS(&v4))) || v4.Get()!=list)throw std::logic_error("Unverified native interface alias");
        auto indices=slots();auto** cv=*reinterpret_cast<void***>(list);auto** qv=*reinterpret_cast<void***>(queue);
        const std::array<void*,Count> callbacks{reinterpret_cast<void*>(Impl::reset),reinterpret_cast<void*>(Impl::close),reinterpret_cast<void*>(Impl::pipeline),reinterpret_cast<void*>(Impl::heaps),
            reinterpret_cast<void*>(Impl::root),reinterpret_cast<void*>(Impl::table),reinterpret_cast<void*>(Impl::descriptor<Cbv,ComputeBindings::Kind::cbv>),reinterpret_cast<void*>(Impl::descriptor<Srv,ComputeBindings::Kind::srv>),reinterpret_cast<void*>(Impl::descriptor<Uav,ComputeBindings::Kind::uav>),
            reinterpret_cast<void*>(Impl::constant),reinterpret_cast<void*>(Impl::constants),reinterpret_cast<void*>(Impl::clear),reinterpret_cast<void*>(Impl::graphics),reinterpret_cast<void*>(Impl::bundle),reinterpret_cast<void*>(Impl::indirect),
            reinterpret_cast<void*>(Impl::predication),reinterpret_cast<void*>(Impl::state_object),reinterpret_cast<void*>(Impl::draw),reinterpret_cast<void*>(Impl::indexed),reinterpret_cast<void*>(Impl::execute)};
        const auto init=MH_Initialize();if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("MinHook initialization");
        unsigned created=0;
        for(unsigned i=0;i<Count;++i){s->targets[i]=(i==Execute?qv:cv)[indices[i]];
            if(completion_only&&i!=Reset&&i!=Close&&i!=Execute){s->targets[i]=nullptr;continue;}
            const auto result=MH_CreateHook(s->targets[i],callbacks[i],&Impl::originals[i]);
            s->hooks.push_back({{"slot",i},{"vtable_slot",indices[i]},{"address",reinterpret_cast<UINT64>(s->targets[i])},{"create",static_cast<int>(result)}});
            if(result!=MH_OK){for(unsigned j=0;j<i;++j)if(s->targets[j])MH_RemoveHook(s->targets[j]);throw std::runtime_error("Native hook conflict/create failed");}++created;
        }
        unsigned diagnostic=0;
        for(unsigned i=0;i<Count;++i){if(!s->targets[i])continue;const auto result=MH_EnableHook(s->targets[i]);s->hooks[diagnostic++]["enable"]=static_cast<int>(result);
            if(result!=MH_OK)throw std::runtime_error("Native partial enable retained, context stopped");}
        return context;
    }catch(...){s->stopped=true;s->fault=true;throw;}
}
void Context::register_root(ID3D12RootSignature* root,ComputeBindings::Layout layout){
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    if(s.completion_only || s.stopped || s.armed || !root || layout.size()>64 || s.root_count==s.roots.size())throw std::logic_error("Root registration rejected");
    for(unsigned i=0;i<s.root_count;++i)if(s.roots[i].root.Get()==root)throw std::logic_error("Duplicate root registration");
    // Validate layout without pretending a root or descriptor is bound.
    ComputeBindings check;check.reset(s.list.Get(),1);check.root(root,layout);
    auto& r=s.roots[s.root_count++];r.root=root;r.count=static_cast<unsigned>(layout.size());std::copy(layout.begin(),layout.end(),r.layout.begin());
}
std::uint64_t Context::generation()const{std::unique_lock lock(impl_->mutex,std::try_to_lock);if(!lock.owns_lock())throw std::logic_error("Recording generation busy");return impl_->generation;}
bool Context::ready()const{auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);return lock.owns_lock()&&!s.completion_only&&!s.stopped&&!s.fault&&!s.invalid&&!s.closed&&!s.armed&&s.shadow.ready(s.list.Get(),s.generation);}
bool Context::completion_only()const noexcept{return impl_->completion_only;}
bool Context::recording_ready()const{auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);return lock.owns_lock()&&!s.stopped&&!s.fault&&!s.invalid&&!s.closed&&!s.armed&&s.generation&&GetCurrentThreadId()==s.recording_thread;}
Context::Snapshot Context::begin(){
    auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock.owns_lock() || s.completion_only || s.stopped || s.fault || s.invalid || s.closed || s.armed || GetCurrentThreadId()!=s.recording_thread || Impl::injected || !s.shadow.ready(s.list.Get(),s.generation))throw std::logic_error("Native state not ready for insertion");
    Snapshot saved{s.shadow,s.generation};Impl::injected=&s;return saved;
}
void Context::finish(const Snapshot& saved){
    auto& s=*impl_;
    {std::unique_lock lock(s.mutex,std::try_to_lock);
        if(!lock.owns_lock() || s.stopped || s.fault || s.invalid || s.closed || s.generation!=saved.generation || Impl::injected!=&s)throw std::logic_error("Native recording changed during insertion");}
    saved.bindings.restore(s.list.Get(),saved.generation);
    {std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock() || s.fault || s.invalid)throw std::logic_error("Native restore observation incomplete");++s.restores;}
    Impl::injected=nullptr;
}
void Context::abort()noexcept{impl_->fatal();Impl::injected=nullptr;}
void Context::arm_last_use(CompletionTimeline& timeline,CompletionTimeline::Ticket ticket,std::uint64_t generation){
    auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock.owns_lock() || s.stopped || s.fault || s.invalid || s.closed || s.armed || generation!=s.generation || GetCurrentThreadId()!=s.recording_thread ||
        timeline.inspect(ticket)!=CompletionTimeline::State::recorded)throw std::logic_error("Cannot arm unknown/stale NR last use");
    s.timeline=&timeline;s.ticket=ticket;s.armed_generation=generation;s.armed=true;s.submitted=false;s.submitting=false;
}
void Context::release_completed(){
    auto& s=*impl_;std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock.owns_lock() || s.stopped || s.fault || !s.armed || !s.submitted ||
        s.timeline.load()->inspect(s.ticket)!=CompletionTimeline::State::invalid)throw std::logic_error("Owner has not retired completion ticket");
    s.armed=false;s.timeline=nullptr;s.ticket={};
}
void Context::stop()noexcept{impl_->stopped=true;}
json Context::status()const{
    auto& s=*impl_;std::lock_guard lock(s.mutex);
    return {{"version",1},{"profile",s.completion_only?"single-native-completion-context":"single-native-compute-context"},{"generation",s.generation},{"setters",s.setters},{"resets",s.resets},{"closes",s.closes},{"restores",s.restores},
        {"unsupported",s.unsupported},{"matched_submissions",s.matched},{"signals",s.signals},{"callback_losses",s.losses.load()},{"injected_setters",s.injected_setters.load()},
        {"invalid_recording",s.invalid},{"terminal_fault",s.fault.load()},{"armed",s.armed},{"stopped",s.stopped.load()},
        {"hooks",s.hooks},{"process_pinned",true},{"game_control_available",false},{"graphics_state_verified",false},{"event_files",0}};
}
}
