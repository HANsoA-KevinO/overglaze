// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The game's latency markers (lab_latency_markers.hpp, shared provider code),
// CPU only.
// Synthetic slPCLSetMarker / slReflexSetMarker functions in this executable are
// resolved through a stand-in slGetFeatureFunction and hooked for real (MinHook):
//   * observe-only: the original runs exactly once with the very same arguments
//     and the caller gets its result unchanged; a forwarding wrapper is one marker;
//   * a function never provided (or no resolver) is settled after 60 polls;
//   * the frame index comes only from observed token issues (unknown otherwise), and
//     an address reissued before its earlier frame started makes the next
//     ePresentStart through it ambiguous, once (aliasing);
//   * the Present binding: same-thread open ePresentStart -> bound, closed by
//     ePresentEnd -> no marker, only another thread open -> other-thread, old -> stale;
//   * no allocation on any marker, binding or token path (global operator new counted);
//   * an unsigned module (game path) or a second owner is refused without hooking;
//   * concurrent writers and readers stay consistent;
//   * through the shared adapter -- a token return feeds the table, and
//     a Present with this thread's open ePresentStart reaches the binding layer as
//     a declared frame, one without it as the ordinary boundary.
#include "lab_latency_markers.hpp"
#include "lab_workbench_adapter.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <new>
#include <cstdlib>
#include <cstring>
namespace {
std::atomic<std::uint64_t> allocations{0};
unsigned checks=0;
void need(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
using lab::chain::LatencyMarkers;using lab::chain::PresentMarkerState;using lab::chain::MarkerSource;
struct Seen {std::atomic<unsigned> calls{0};std::uint32_t marker=UINT32_MAX;const sl::FrameToken* token=nullptr;};
Seen pcl_seen,reflex_seen,unsigned_seen;sl::Result give_back=sl::Result::eErrorNotInitialized;bool reflex_forwards=false;
__declspec(noinline) sl::Result synthetic_pcl(std::uint32_t marker,const sl::FrameToken& frame){pcl_seen.marker=marker;pcl_seen.token=&frame;++pcl_seen.calls;return give_back;}
__declspec(noinline) sl::Result synthetic_reflex(std::uint32_t marker,const sl::FrameToken& frame){reflex_seen.marker=marker;reflex_seen.token=&frame;++reflex_seen.calls;
    if(reflex_forwards){auto volatile p=&synthetic_pcl;return p(marker,frame);}return give_back;}
__declspec(noinline) sl::Result synthetic_unsigned(std::uint32_t marker,const sl::FrameToken& frame){unsigned_seen.marker=marker;unsigned_seen.token=&frame;++unsigned_seen.calls;return give_back;}
sl::Result resolve_none(sl::Feature,const char*,void*& out){out=nullptr;return sl::Result::eErrorFeatureMissing;}
sl::Result resolve_both(sl::Feature f,const char* name,void*& out){out=nullptr;
    if(f==sl::kFeaturePCL&&!std::strcmp(name,"slPCLSetMarker"))out=reinterpret_cast<void*>(&synthetic_pcl);
    else if(f==sl::kFeatureReflex&&!std::strcmp(name,"slReflexSetMarker"))out=reinterpret_cast<void*>(&synthetic_reflex);
    return out?sl::Result::eOk:sl::Result::eErrorFeatureMissing;}
sl::Result resolve_unsigned(sl::Feature f,const char*,void*& out){out=f==sl::kFeaturePCL?reinterpret_cast<void*>(&synthetic_unsigned):nullptr;return out?sl::Result::eOk:sl::Result::eErrorFeatureMissing;}
struct Token:sl::FrameToken {operator uint32_t()const override{throw std::runtime_error("the token object is never read");}};
Token token_a,token_b,token_c;
sl::Result pcl(std::uint32_t marker,const sl::FrameToken& t){auto volatile p=&synthetic_pcl;return p(marker,t);}
sl::Result reflex(std::uint32_t marker,const sl::FrameToken& t){auto volatile r=&synthetic_reflex;return r(marker,t);}
std::uint64_t qpc(){LARGE_INTEGER q{};QueryPerformanceCounter(&q);return static_cast<std::uint64_t>(q.QuadPart);}
unsigned me(){return GetCurrentThreadId();}
// The detours refer to the owning instance for the life of the process.
LatencyMarkers& markers(){static auto* m=new LatencyMarkers();return *m;}
}
void* operator new(std::size_t n){++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
int main(){try{
    auto& m=markers();
    // 1. Nothing resolvable: nothing hooked, every Present says so.
    m.poll(&resolve_none,true,0);
    need(m.functions()==0&&m.at_present(me(),qpc()).state==PresentMarkerState::unavailable,"Unresolved: no hook, Presents report no-game-latency-markers");
    need(m.snapshot()["functions"]["slPCLSetMarker"]["state"]=="unresolved"&&!m.due(500)&&m.due(1000),"Unresolved is retried after one second");
    // 1b. An SDK that never provides a function (or a process with
    // no verified resolver) is settled after kGiveUp polls; polling then stops.
    {auto* none=new LatencyMarkers();auto* bare=new LatencyMarkers();std::uint64_t t=0;
        for(unsigned i=0;i+1<LatencyMarkers::kGiveUp;++i,t+=1000){none->poll(&resolve_none,true,t);bare->poll(nullptr,true,t);}
        need(none->due(t)&&bare->due(t)&&none->snapshot()["functions"]["slPCLSetMarker"]["state"]=="unresolved","Still polled before the limit");
        none->poll(&resolve_none,true,t);bare->poll(nullptr,true,t);
        const auto s=none->snapshot(),b=bare->snapshot();
        need(!none->due(UINT64_MAX)&&!bare->due(UINT64_MAX)&&s["functions"]["slPCLSetMarker"]["state"]=="not-provided-by-this-sdk"&&
             s["functions"]["slReflexSetMarker"]["state"]=="not-provided-by-this-sdk"&&b["functions"]["slPCLSetMarker"]["state"]=="no-public-resolver"&&
             s["attempts"]==LatencyMarkers::kGiveUp&&none->functions()==0,"Settled as not provided after the limit; polling stops");}
    // 2. Resolved through the stand-in resolver: both hooked, once.
    m.poll(&resolve_both,true,1000);
    need(m.functions()==3&&m.snapshot()["hooked"].size()==2,"Both marker functions hooked");
    need(!m.due(1000000),"Settled: no further resolution");
    // 3. Observe-only: same arguments, result unchanged, original exactly once.
    {const auto before=m.snapshot()["markers"].get<std::uint64_t>();
        const auto r=pcl(7,token_a);
        need(r==sl::Result::eErrorNotInitialized&&pcl_seen.calls==1&&pcl_seen.marker==7&&pcl_seen.token==&token_a,"PCL: original once, same marker and token, its result returned");
        give_back=sl::Result::eOk;const auto r2=reflex(13,token_b);
        need(r2==sl::Result::eOk&&reflex_seen.calls==1&&reflex_seen.marker==13&&reflex_seen.token==&token_b,"Reflex: original once, same arguments, its result returned");
        need(m.snapshot()["markers"].get<std::uint64_t>()==before+2,"Both calls recorded");}
    // 4. A wrapper forwarding to the other hooked function is one marker.
    {reflex_forwards=true;const auto before=m.snapshot()["markers"].get<std::uint64_t>();const auto calls=pcl_seen.calls.load();
        need(reflex(1,token_a)==sl::Result::eOk&&pcl_seen.calls==calls+1,"The forwarded inner original still runs");reflex_forwards=false;
        const auto s=m.snapshot();need(s["markers"].get<std::uint64_t>()==before+1&&s["nested_calls"]==1,"Recorded once (outer), the inner call counted as nested");}
    const auto start_alloc=allocations.load();
    // 5. Frame index only from observed token issues.
    need(pcl(4,token_a)==sl::Result::eOk,"Marker");
    {const auto b=m.at_present(me(),qpc());need(b.state==PresentMarkerState::bound&&b.source==MarkerSource::pcl&&!b.frame_known&&b.marker==4,"Open ePresentStart binds; an address never seen issued has no frame");}
    m.token_returned(&token_a,41,true);m.token_returned(&token_b,40,true);m.token_returned(&token_c,99,false);
    need(pcl(4,token_a)==sl::Result::eOk,"Marker");
    {const auto b=m.at_present(me(),qpc());need(b.state==PresentMarkerState::bound&&b.frame_known&&b.frame==41&&b.thread==me(),"The issued index of that token address");}
    m.token_returned(&token_a,45,true);need(pcl(4,token_a)==sl::Result::eOk,"Marker");
    {const auto b=m.at_present(me(),qpc());need(b.frame==45,"Reissued address: the newer frame, as Streamline itself reports it");}
    need(reflex(4,token_c)==sl::Result::eOk,"Marker");
    {const auto b=m.at_present(me(),qpc());need(b.state==PresentMarkerState::bound&&b.source==MarkerSource::reflex&&!b.frame_known,"Issued without a known index: unknown, not guessed");}
    // 6. Non-Present markers never bind; ePresentEnd closes.
    need(pcl(4,token_b)==sl::Result::eOk&&pcl(2,token_a)==sl::Result::eOk&&pcl(3,token_a)==sl::Result::eOk,"Markers");
    {const auto b=m.at_present(me(),qpc());need(b.state==PresentMarkerState::bound&&b.frame==40,"Render-submit markers after ePresentStart do not move the binding");}
    need(pcl(5,token_b)==sl::Result::eOk,"Marker");
    need(m.at_present(me(),qpc()).state==PresentMarkerState::no_marker,"After ePresentEnd this thread has no open marker");
    need(allocations.load()==start_alloc,"No allocation on the marker, token or binding paths");
    // 7. Another thread's open marker is reported, never bound.
    {std::thread other([]{(void)pcl(4,token_a);});other.join();}
    const auto thread_alloc=allocations.load();
    need(m.at_present(me(),qpc()).state==PresentMarkerState::no_marker,"This thread's own closed marker decides over another thread's open one");
    {const auto b=m.at_present(1u,qpc());need(b.state==PresentMarkerState::other_thread&&b.frame==45&&b.thread!=1u,"A thread that never marked sees only another thread's open marker");}
    // 8. Window.
    need(pcl(4,token_a)==sl::Result::eOk,"Marker");
    {LARGE_INTEGER f{};QueryPerformanceFrequency(&f);need(m.at_present(me(),qpc()+std::uint64_t(f.QuadPart)).state==PresentMarkerState::stale,"An ePresentStart older than 100 ms is stale");}
    need(pcl(5,token_a)==sl::Result::eOk,"Marker");
    need(allocations.load()==thread_alloc,"No allocation on the other-thread and window paths either");
    // 9. Bounded scan: markers of other threads push this thread's out of reach.
    need(pcl(4,token_b)==sl::Result::eOk,"Marker");
    for(unsigned i=0;i<LatencyMarkers::kScan+4;++i)m.record(MarkerSource::pcl,2,&token_c,7777,qpc());
    need(m.at_present(me(),qpc()).state==PresentMarkerState::no_marker,"Beyond the scan window nothing binds (no guess from older history)");
    need(pcl(5,token_b)==sl::Result::eOk,"Marker");
    // 10. Token table capacity: the oldest issue is forgotten, never confused.
    {std::vector<Token> many(LatencyMarkers::kTokens+8);for(unsigned i=0;i<many.size();++i)m.token_returned(&many[i],1000+i,true);
        std::uint32_t index=0;need(!m.lookup(&many[0],index),"Evicted token address: unknown");
        need(m.lookup(&many.back(),index)&&index==1000+many.size()-1,"Recent token address: its index");
        need(!m.lookup(nullptr,index),"Null token: unknown");}
    // 10b. Token aliasing: an address reissued while the frame it
    // carried has not had its ePresentStart yet. A late ePresentStart through it would
    // otherwise name the NEW frame; it is ambiguous instead, once, then known again.
    {static Token reused,other;const auto before=m.snapshot();
        m.token_returned(&other,500,true);need(pcl(4,other)==sl::Result::eOk&&pcl(5,other)==sl::Result::eOk,"Markers");   // frame 500 started
        m.token_returned(&reused,501,true);m.token_returned(&reused,505,true);                                             // 501 not started: pending
        need(pcl(4,reused)==sl::Result::eOk,"Marker");
        {const auto b=m.at_present(me(),qpc());need(b.state==PresentMarkerState::bound&&b.frame_known==2,"A late ePresentStart through a reissued address is ambiguous, not frame 505");}
        need(pcl(5,reused)==sl::Result::eOk&&pcl(4,reused)==sl::Result::eOk,"Markers");
        {const auto b=m.at_present(me(),qpc());need(b.frame_known==1&&b.frame==505,"Consumed once: the next ePresentStart through it is frame 505");}
        need(pcl(5,reused)==sl::Result::eOk,"Marker");
        // Not an alias: the earlier frame had started before the reissue.
        m.token_returned(&other,506,true);need(pcl(4,other)==sl::Result::eOk,"Marker");
        {const auto b=m.at_present(me(),qpc());need(b.frame_known==1&&b.frame==506,"Reissue after the earlier frame started: known");}
        need(pcl(5,other)==sl::Result::eOk,"Marker");
        // The same index issued again (several requests for one frame) is no reissue.
        m.token_returned(&reused,507,true);need(pcl(4,reused)==sl::Result::eOk,"Marker");m.token_returned(&reused,507,true);m.token_returned(&reused,508,true);
        need(pcl(5,reused)==sl::Result::eOk,"Marker");
        const auto after=m.snapshot();
        need(after["ambiguous_present_starts"].get<std::uint64_t>()==before["ambiguous_present_starts"].get<std::uint64_t>()+1&&
             after["token_reissued_before_previous_frame_started"].get<std::uint64_t>()==before["token_reissued_before_previous_frame_started"].get<std::uint64_t>()+1&&
             after["latest_started_frame"]==507,("Exactly one ambiguity, from exactly one early reissue: "+after.dump()).c_str());}
    // 11. Game path: an unsigned module is refused, nothing hooked; a second
    // instance cannot take the process's hooks.
    {auto* other=new LatencyMarkers();other->poll(&resolve_unsigned,false,0);
        need(other->functions()==0&&other->snapshot()["functions"]["slPCLSetMarker"]["state"]=="failed","Unsigned owner module refused on the game path");
        auto volatile u=&synthetic_unsigned;const auto calls=unsigned_seen.calls.load();need(u(4,token_a)==sl::Result::eOk&&unsigned_seen.calls==calls+1,"Refused function untouched");
        auto* second=new LatencyMarkers();second->poll(&resolve_unsigned,true,0);
        need(second->functions()==0&&second->snapshot()["functions"]["slPCLSetMarker"]["error"].get<std::string>().find("Another marker observer")==0,"A second owner is refused");}
    // 12. Concurrency: writers, token issues and Present bindings at once.
    {std::atomic<bool> stop{false},crossed{false};std::atomic<std::uint64_t> bindings{0};const auto before=m.snapshot()["markers"].get<std::uint64_t>();
        std::vector<std::thread> writers;for(unsigned t=0;t<4;++t)writers.emplace_back([t]{
            for(unsigned i=0;i<20000;++i){auto& tok=t%2?token_a:token_b;if(i%7==0)markers().token_returned(&tok,i,true);(void)pcl(i%2?5u:4u,tok);}});
        std::thread reader([&]{while(!stop){const auto b=markers().at_present(me(),qpc());if(b.state==PresentMarkerState::bound)crossed=true;++bindings;}});
        for(auto& w:writers)w.join();stop=true;reader.join();
        need(!crossed&&bindings.load()>0,"A thread that never marked is never bound to another thread's marker");
        need(m.snapshot()["markers"].get<std::uint64_t>()==before+80000,"Every concurrent marker recorded");
        need(m.snapshot()["token_table_untrusted"]==false,"Concurrent token issues never lose an update (no process-wide distrust)");
        m.token_returned(&token_a,7,true);std::uint32_t index=0;need(m.lookup(&token_a,index)&&index==7,"After the storm the newest issue wins");}
    // 13. The adapter of both hosts. The token feed is the adapter's
    // own (a successful slGetNewFrameToken return), and its Present reads this
    // thread's marker: declared once while the ePresentStart is open, never after
    // the ePresentEnd closed it.
    {static Token fed;auto* adapter=new lab::WorkbenchAdapter();adapter->set_latency_markers(&m);
        lab::slboundary::Call c;c.api=lab::slboundary::Api::token;c.id=1;c.thread=me();c.result=sl::Result::eOk;
        c.token=&fed;c.frame_index=900;c.frame_index_known=true;adapter->entering(c);adapter->returned(c);
        std::uint32_t index=0;need(m.lookup(&fed,index)&&index==900,"The adapter feeds every successful token return to the markers");
        need(pcl(4,fed)==sl::Result::eOk,"Marker");adapter->present(1);
        auto stats=adapter->snapshot()["binding_stats"];
        need(stats.value("presents_declared_by_game_marker",0ULL)==1&&stats.value("present_expiries_spared_previous_frame",1ULL)==0,
             ("A Present with this thread's open ePresentStart is a declared frame (nothing live: not spared): "+stats.dump()).c_str());
        need(pcl(5,fed)==sl::Result::eOk,"Marker");adapter->present(1);
        stats=adapter->snapshot()["binding_stats"];
        need(stats.value("presents_declared_by_game_marker",0ULL)==1,"After ePresentEnd a Present declares nothing: the ordinary boundary");
        need(adapter->snapshot()["present_callbacks"]==2,"Both Presents reached the adapter");}
    std::cout<<"PASS latency markers: "<<checks<<" checks; observe-only hooks, issued-index lookup, same-thread binding, no allocation, refusals, concurrency, adapter feed and Present reading; "<<m.snapshot().dump()<<'\n';
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
