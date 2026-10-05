// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The game's own latency markers (shared provider code); see
// lab_latency_markers.hpp for the contract.
#include "lab_latency_markers.hpp"
#include <MinHook.h>
#include <cstring>
namespace lab::chain {
namespace {
// The detours are plain functions; one instance per process owns them.
std::atomic<LatencyMarkers*> owner{nullptr};
std::array<std::atomic<MarkerFunction*>,2> originals{};
thread_local unsigned depth=0;
// Observe-only: record, then the original with the very same arguments, its
// result returned unchanged. A wrapper that calls the other hooked function
// (an SDK whose Reflex marker forwards to PCL) is one marker: only the outer
// call is recorded, the inner one is counted as nested.
template<unsigned Which> sl::Result detour(std::uint32_t marker,const sl::FrameToken& frame){
    auto* original=originals[Which].load(std::memory_order_acquire); // stored before the hook was enabled
    if(auto* m=owner.load(std::memory_order_acquire)){
        if(depth)m->nested();
        else{LARGE_INTEGER q{};QueryPerformanceCounter(&q);
            m->record(Which?MarkerSource::reflex:MarkerSource::pcl,marker,&frame,GetCurrentThreadId(),static_cast<std::uint64_t>(q.QuadPart));}}
    ++depth;struct Leave{~Leave(){--depth;}} leave;
    return original(marker,frame);
}
static_assert(std::is_same_v<decltype(&detour<0>),MarkerFunction*>&&std::is_same_v<decltype(&detour<1>),MarkerFunction*>);
bool executable(const void* p) noexcept {MEMORY_BASIC_INFORMATION m{};
    return p&&VirtualQuery(p,&m,sizeof(m))&&m.State==MEM_COMMIT&&!(m.Protect&PAGE_GUARD)&&
        (m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));}
}
LatencyMarkers::LatencyMarkers() noexcept {
    LARGE_INTEGER f{};QueryPerformanceFrequency(&f);frequency_=static_cast<std::uint64_t>(f.QuadPart);
    window_qpc_=frequency_/10; // 100 ms: a game sets ePresentStart right before it calls Present
    fn_[0].name="slPCLSetMarker";fn_[0].feature=sl::kFeaturePCL;fn_[1].name="slReflexSetMarker";fn_[1].feature=sl::kFeatureReflex;
}
// One more poll without an entry for this function; after kGiveUp in a row it is
// settled (an SDK without the function is never polled forever).
void LatencyMarkers::miss(unsigned which,const char* settled_state) noexcept {
    auto& f=fn_[which];f.state="unresolved";
    if(++f.misses>=kGiveUp){f.state=settled_state;settled_|=1u<<which;}
}
void LatencyMarkers::poll(PFun_slGetFeatureFunction* resolver,bool fixture,std::uint64_t now) noexcept {
    try{std::lock_guard lock(worker_mutex_);
        if(now<next_poll_ms_||settled_.load()==3u)return;
        ++attempts_;next_poll_ms_=now+1000;
        if(!resolver){resolver_error_="slGetFeatureFunction unavailable: no interposer, export missing or outside the verified interposer image";
            for(unsigned which=0;which<2;++which)if(!(settled_.load()&(1u<<which)))miss(which,"no-public-resolver");return;}
        resolver_error_.clear();
        for(unsigned which=0;which<2;++which){auto& f=fn_[which];if(settled_.load()&(1u<<which))continue;
            void* target=nullptr;sl::Result r=sl::Result::eErrorInvalidParameter;
            try{r=resolver(f.feature,f.name,target);}catch(...){r=sl::Result::eErrorInvalidParameter;target=nullptr;}
            f.last_result=static_cast<unsigned>(r);
            if(r!=sl::Result::eOk||!target){miss(which,"not-provided-by-this-sdk");continue;}
            f.misses=0;
            try{install(which,target,fixture);}
            catch(const std::exception& e){f.state="failed";f.error=e.what();settled_|=1u<<which;}
        }
    }catch(...){}
}
void LatencyMarkers::install(unsigned which,void* target,bool fixture){
    auto& f=fn_[which];f.target=target;const unsigned other=which^1u;
    // An SDK may hand back the same entry for both names: hook it once.
    if(fn_[other].target==target&&(functions_.load()&(1u<<other))){f.state="alias-of-the-other-marker-function";settled_|=1u<<which;return;}
    if(!executable(target))throw std::runtime_error("Marker function is not executable code; not hooked");
    HMODULE module{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(target),&module))
        throw std::runtime_error("Marker function lies in no loaded module; not hooked");
    if(fixture){if(module!=GetModuleHandleW(nullptr))throw std::runtime_error("Fixture marker function outside the fixture executable; not hooked");
        f.module={{"origin","synthetic-fixture"},{"scope","inside the launcher-verified fixture executable (its resolver was checked at attach)"}};}
    else{f.module=module_identity(module);
        if(f.module.value("signature","")!="ValidCachedTrust")throw std::runtime_error("Marker function's module is not validly signed; not hooked");}
    LatencyMarkers* expected=nullptr;
    if(!owner.compare_exchange_strong(expected,this)&&expected!=this)throw std::runtime_error("Another marker observer owns this process's hooks");
    const auto init=MH_Initialize();if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("MinHook unavailable");
    void* trampoline=nullptr;
    const auto created=MH_CreateHook(target,which?reinterpret_cast<void*>(&detour<1>):reinterpret_cast<void*>(&detour<0>),&trampoline);
    if(created!=MH_OK)throw std::runtime_error("Marker hook create failed (MinHook "+std::to_string(int(created))+"); not retried");
    originals[which].store(reinterpret_cast<MarkerFunction*>(trampoline),std::memory_order_release);
    const auto enabled=MH_EnableHook(target);
    if(enabled!=MH_OK){(void)MH_RemoveHook(target); // never enabled: nobody runs through it
        throw std::runtime_error("Marker hook enable failed (MinHook "+std::to_string(int(enabled))+"); not retried");}
    functions_|=1u<<which;settled_|=1u<<which;f.state="hooked";
}
void LatencyMarkers::token_returned(const void* token,std::uint32_t index,bool known) noexcept {
    if(!token)return;const auto key=reinterpret_cast<std::uintptr_t>(token);
    // Per-slot sequence: a writer owns a slot by moving its version from even to
    // odd; readers never wait. lookup takes the NEWEST write of an address, so a
    // writer that finds this address's slot busy writes a fresh slot instead of
    // waiting: two writers can cost an eviction ("unknown"), never a wrong index.
    for(unsigned attempt=0;attempt<256;++attempt){
        TokenSlot* target=nullptr;std::uint64_t newest=0,oldest=UINT64_MAX;
        // This address's NEWEST settled slot (a busy write may have left an older duplicate).
        for(auto& s:tokens_)if(s.pointer.load(std::memory_order_acquire)==key&&!(s.version.load(std::memory_order_acquire)&1u)){
            const auto seen=s.seen.load(std::memory_order_relaxed);if(!target||seen>newest){newest=seen;target=&s;}}
        if(!target)for(auto& s:tokens_){if(s.version.load(std::memory_order_acquire)&1u)continue;
            const auto seen=s.seen.load(std::memory_order_relaxed);if(seen<oldest){oldest=seen;target=&s;}}
        auto v=target?target->version.load(std::memory_order_acquire):1u;
        if((v&1u)||!target->version.compare_exchange_strong(v,v+1,std::memory_order_acq_rel)){if(attempt>=8)SwitchToThread();continue;}
        std::atomic_thread_fence(std::memory_order_release);
        // Aliasing: the address carried another frame that has not
        // started yet; a late ePresentStart of that frame through this address would
        // name the new one. Leave the address pending (record() consumes it). An
        // older pending that is still unstarted is kept: one ambiguity per address.
        const auto stamp=token_stamp_.fetch_add(1)+1;std::uint64_t pending=0;std::uint32_t pending_frame=0;
        if(known&&target->pointer.load(std::memory_order_relaxed)==key&&target->known.load(std::memory_order_relaxed)){
            const auto before=target->index.load(std::memory_order_relaxed),held=target->pending_frame.load(std::memory_order_relaxed);
            if(target->pending.load(std::memory_order_relaxed)&&not_started(held)){pending=stamp;pending_frame=held;}
            else if(before!=index&&not_started(before)){pending=stamp;pending_frame=before;}}
        target->pointer.store(key,std::memory_order_relaxed);target->index.store(index,std::memory_order_relaxed);
        target->known.store(known?1u:0u,std::memory_order_relaxed);target->pending_frame.store(pending_frame,std::memory_order_relaxed);
        target->pending.store(pending,std::memory_order_relaxed);target->seen.store(stamp,std::memory_order_relaxed);
        target->version.store(v+2,std::memory_order_release);++token_updates_;if(pending)pending_set_.fetch_add(1,std::memory_order_relaxed);return;
    }
    // An issue could not be stored: some address may now map to an older frame.
    // No marker frame index is trusted again in this process.
    token_untrusted_=true;
}
bool LatencyMarkers::resolve(const void* token,Resolved& out) const noexcept {
    if(!token||token_untrusted_.load(std::memory_order_acquire))return false;
    const auto key=reinterpret_cast<std::uintptr_t>(token);std::uint64_t best=0;bool found=false;
    for(unsigned n=0;n<kTokens;++n){const auto& s=tokens_[n];bool read=false;
        for(unsigned retry=0;retry<4&&!read;++retry){
            const auto v=s.version.load(std::memory_order_acquire);if(v&1u)continue;
            const auto p=s.pointer.load(std::memory_order_relaxed);const auto i=s.index.load(std::memory_order_relaxed);
            const auto k=s.known.load(std::memory_order_relaxed);const auto seen=s.seen.load(std::memory_order_relaxed);
            const auto pending=s.pending.load(std::memory_order_relaxed);const auto held=s.pending_frame.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(s.version.load(std::memory_order_relaxed)!=v)continue;
            read=true;if(p==key&&seen>best){best=seen;found=k!=0;out.index=i;out.pending=pending;out.pending_frame=held;out.slot=n;}
        }
        if(!read)return false; // a slot being rewritten could be this very token's: unknown, not a guess
    }
    out.known=found;return found;
}
bool LatencyMarkers::lookup(const void* token,std::uint32_t& index) const noexcept {Resolved r;if(!resolve(token,r))return false;index=r.index;return true;}
bool LatencyMarkers::not_started(std::uint32_t frame) const noexcept {
    const auto last=last_started_.load(std::memory_order_acquire);
    return (last>>32)!=0&&static_cast<std::int32_t>(frame-static_cast<std::uint32_t>(last))>0;
}
void LatencyMarkers::started(std::uint32_t frame) noexcept {
    auto current=last_started_.load(std::memory_order_relaxed);const std::uint64_t want=(1ull<<32)|frame;
    while((!(current>>32)||static_cast<std::int32_t>(frame-static_cast<std::uint32_t>(current))>0)&&
          !last_started_.compare_exchange_weak(current,want,std::memory_order_acq_rel,std::memory_order_relaxed)){}
}
void LatencyMarkers::record(MarkerSource source,std::uint32_t marker,const void* token,unsigned thread,std::uint64_t qpc) noexcept {
    Resolved r;unsigned known=resolve(token,r)?1u:0u;const std::uint32_t frame=known?r.index:0u;
    if(marker==kMarkerPresentStart&&known){
        // An address left pending (token_returned) whose earlier frame has still not
        // started: this ePresentStart is that frame's late one or the new frame's --
        // ambiguous (2), never the new frame. Consumed once; at least the earlier
        // frame has now started or been skipped (Presents start in frame order).
        if(r.pending&&not_started(r.pending_frame)){known=2;aliased_.fetch_add(1,std::memory_order_relaxed);
            auto expected=r.pending;(void)tokens_[r.slot].pending.compare_exchange_strong(expected,0,std::memory_order_acq_rel);started(r.pending_frame);}
        else started(frame);}
    if(!known)unknown_tokens_.fetch_add(1,std::memory_order_relaxed);
    const auto n=head_.fetch_add(1,std::memory_order_relaxed)+1;auto& s=ring_[n%kRing];
    s.seq.store(2*n+1,std::memory_order_relaxed);std::atomic_thread_fence(std::memory_order_release);
    s.marker.store(marker,std::memory_order_relaxed);s.frame.store(known==1?frame:0u,std::memory_order_relaxed);s.known.store(known,std::memory_order_relaxed);
    s.thread.store(thread,std::memory_order_relaxed);s.source.store(static_cast<std::uint32_t>(source),std::memory_order_relaxed);s.qpc.store(qpc,std::memory_order_relaxed);
    s.seq.store(2*n+2,std::memory_order_release);
    counts_[marker<kCounted?marker:kCounted].fetch_add(1,std::memory_order_relaxed);
    if(marker==kMarkerPresentStart)present_starts_.fetch_add(1,std::memory_order_release);
}
int LatencyMarkers::read(std::uint64_t i,Entry& e) const noexcept {
    const auto& s=ring_[i%kRing];const auto want=2*i+2;
    const auto before=s.seq.load(std::memory_order_acquire);
    if(before!=want)return before>want?-1:0; // a later lap overwrote it / its writer is still writing
    e.marker=s.marker.load(std::memory_order_relaxed);e.frame=s.frame.load(std::memory_order_relaxed);e.known=s.known.load(std::memory_order_relaxed);
    e.thread=s.thread.load(std::memory_order_relaxed);e.source=s.source.load(std::memory_order_relaxed);e.qpc=s.qpc.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto after=s.seq.load(std::memory_order_relaxed);
    if(after!=want)return after>want?-1:0;
    e.sequence=i;return 1;
}
PresentMarker LatencyMarkers::at_present(unsigned thread,std::uint64_t now) const noexcept {
    PresentMarker out;out.functions=functions_.load(std::memory_order_acquire);out.present_starts=present_starts_.load(std::memory_order_acquire);
    out.token_untrusted=token_untrusted_.load()?1u:0u;
    if(!out.functions)return out; // unavailable: no marker function hooked
    const auto fill=[&](const Entry& e,PresentMarkerState s){out.state=s;out.source=static_cast<MarkerSource>(e.source);out.marker=e.marker;
        out.frame=e.frame;out.frame_known=e.known;out.thread=e.thread;out.qpc=e.qpc;out.sequence=e.sequence;};
    // Newest first. This thread's newest Present marker decides: an ePresentStart
    // (inside the window) is the open marker this Present belongs to; an
    // ePresentEnd closed the last one, so nothing declares this Present. Other
    // threads only ever give "open-marker-on-another-thread", never a binding.
    const auto head=head_.load(std::memory_order_acquire);
    std::array<unsigned,8> others{};unsigned other_count=0;bool other=false;Entry other_entry{};
    for(std::uint64_t i=head;i>0&&head-i<kScan;--i){Entry e;const int r=read(i,e);
        if(r<0){out.state=PresentMarkerState::lost;return out;} // lapped: everything older is gone too
        if(r==0)continue; // another thread is writing it right now
        if(e.marker!=kMarkerPresentStart&&e.marker!=kMarkerPresentEnd)continue;
        const bool open=e.marker==kMarkerPresentStart;const bool inside=(now>e.qpc?now-e.qpc:0)<=window_qpc_;
        // A closing ePresentEnd is kept (marker 5, its sequence and QPC): the series
        // reads from it that a game window closed.
        if(e.thread==thread){fill(e,open?(inside?PresentMarkerState::bound:PresentMarkerState::stale):PresentMarkerState::no_marker);return out;}
        bool first=true;for(unsigned k=0;k<other_count;++k)if(others[k]==e.thread){first=false;break;}
        if(first){if(other_count<others.size())others[other_count++]=e.thread;if(open&&inside&&!other){other=true;other_entry=e;}}
    }
    if(other)fill(other_entry,PresentMarkerState::other_thread);else out.state=PresentMarkerState::no_marker;
    return out;
}
json LatencyMarkers::snapshot() const {
    std::lock_guard lock(worker_mutex_);json functions=json::object(),hooked=json::array();
    for(unsigned which=0;which<2;++which){const auto& f=fn_[which];
        functions[f.name]={{"state",f.state},{"feature",f.feature},{"resolver_result",f.last_result==UINT32_MAX?json(nullptr):json(f.last_result)},
            {"consecutive_polls_without_entry",f.misses},{"settled_after_polls_without_entry",kGiveUp},
            {"address",f.target?json(reinterpret_cast<std::uint64_t>(f.target)):json(nullptr)},{"module",f.module},{"error",f.error.empty()?json(nullptr):json(f.error)}};
        if(functions_.load()&(1u<<which))hooked.push_back(which?"reflex":"pcl");}
    json by=json::object();for(unsigned i=0;i<=kCounted;++i)if(const auto n=counts_[i].load())by[i<kCounted?std::to_string(i):std::string("other")]=n;
    return {{"functions",functions},{"hooked",hooked},{"attempts",attempts_},{"resolver_error",resolver_error_.empty()?json(nullptr):json(resolver_error_)},
        {"markers",head_.load()},{"by_marker",by},{"present_starts",present_starts_.load()},{"nested_calls",nested_.load()},
        {"unknown_token_markers",unknown_tokens_.load()},{"token_updates",token_updates_.load()},{"token_table_untrusted",token_untrusted_.load()},
        {"token_reissued_before_previous_frame_started",pending_set_.load()},{"ambiguous_present_starts",aliased_.load()},
        {"latest_started_frame",(last_started_.load()>>32)?json(static_cast<std::uint32_t>(last_started_.load())):json(nullptr)},
        {"present_window_ms",100},
        {"scope","observe-only hooks on the game's own latency-marker functions (called with the same arguments, result returned unchanged); a marker's frame is the index its token address was seen issued with; nothing is written to the game"}};
}
json LatencyMarkers::summary() const {
    json hooked=json::array();const auto f=functions_.load();if(f&1u)hooked.push_back("pcl");if(f&2u)hooked.push_back("reflex");
    return {{"hooked",hooked},{"settled",settled_.load()==3u},{"present_starts",present_starts_.load()},{"markers",head_.load()},
        {"unknown_token_markers",unknown_tokens_.load()},{"ambiguous_present_starts",aliased_.load()},{"token_table_untrusted",token_untrusted_.load()}};
}
}
