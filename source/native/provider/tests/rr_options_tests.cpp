// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_rr_options.hpp"
#include <cstring>
#include <iostream>
#include <thread>
#include <limits>
namespace lab::rr {
struct WatchTestAccess {
    static Packet reader_while_metadata_locked(Watch& watch){
        std::lock_guard guard(watch.mutex_);return watch.freeze();
    }
    // The per-viewport record a finished relaxed setter writes.
    static void store(Watch& watch,const Packet& p){watch.store_view(p);}
};
}
namespace {
unsigned checks=0;std::atomic<unsigned> originals=0;
HANDLE entered=nullptr,release_call=nullptr;
void need(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
__declspec(noinline) sl::Result setter(const sl::ViewportHandle& v,const lab::rr::OptionsV3&) {
    ++originals;
    const auto id=static_cast<unsigned>(v);
    if(id==100){SetEvent(entered);if(WaitForSingleObject(release_call,3000)!=WAIT_OBJECT_0)throw std::runtime_error("Fixture wait timeout");}
    if(id==101)throw std::runtime_error("Fixture foreign exception");
    return id==102?sl::Result::eErrorInvalidParameter:sl::Result::eOk;
}
__declspec(noinline) sl::Result sr_setter(const sl::ViewportHandle& v,const lab::rr::SrOptionsPrefix&) {
    ++originals;return static_cast<unsigned>(v)==102?sl::Result::eErrorInvalidParameter:sl::Result::eOk;
}
__declspec(noinline) sl::Result sr_setter_second(const sl::ViewportHandle&,const lab::rr::SrOptionsPrefix&) {++originals;return sl::Result::eErrorInvalidParameter;}
lab::rr::SrOptionsPrefix valid_sr(){lab::rr::SrOptionsPrefix s;s.mode=2;s.width=3840;s.height=2160;s.pre_exposure=.0038f;s.exposure_scale=1;s.hdr=sl::eTrue;return s;}
lab::rr::OptionsV3 valid(){lab::rr::OptionsV3 o;o.mode=1;o.width=5120;o.height=2160;o.pre_exposure=.25f;o.exposure_scale=2;
    o.hdr=sl::eTrue;o.flip_x=o.flip_y=o.alpha=sl::eFalse;o.presets={4,5,6,7,8,9};return o;}
}
int main(){try{
    using namespace lab::rr;
    auto* watch=new Watch;lab::json hooks;need(watch->install(reinterpret_cast<void*>(&setter),hooks),"Install actual native hook");
    auto volatile call=reinterpret_cast<SetOptions*>(&setter);sl::ViewportHandle viewport(0u);auto o=valid();
    need(watch->freeze().issue==Issue::unseen,"No default observed values");
    need(!watch->snapshot().contains("pre_exposure"),"Unseen omits numeric values");
    need(call(viewport,o)==sl::Result::eOk,"Passthrough result");auto first=watch->freeze();
    need(first.valid()&&watch->unchanged(first)&&first.options.pre_exposure==.25f,"Actual values captured");
    need(WatchTestAccess::reader_while_metadata_locked(*watch).valid(),"Read-only snapshot activity must not reject unchanged options");
    std::atomic<bool> reader_error=false;
    std::array<std::thread,4> readers;
    for(auto& reader:readers)reader=std::thread([&]{for(unsigned i=0;i<2000;++i){const auto p=watch->freeze();if(!p.valid()||p.revision!=first.revision||p.options.width!=5120||p.options.pre_exposure!=.25f)reader_error=true;(void)watch->snapshot();}});
    for(auto& reader:readers)reader.join();
    need(!reader_error&&watch->snapshot()["callback_losses"]==0,"Concurrent status readers cannot invalidate unchanged RR options");
    auto description=describe(first);need(description["linear_rgb_verified"]==false&&description["actual_preset_verified"]==false,"No semantic promotion");
    call(sl::ViewportHandle(1u),o);need(watch->freeze().valid()&&watch->freeze().viewport==1,"Nonzero opaque viewport preserved, not rewritten to zero");
    call(sl::ViewportHandle(UINT32_MAX),o);need(!watch->freeze().valid(),"Invalid viewport sentinel rejected");
    o.pre_exposure=.5f;call(viewport,o);need(!watch->unchanged(first)&&watch->freeze().options.pre_exposure==.5f,"Revision invalidation");
    for(unsigned version:{0u,1u,2u,4u,UINT32_MAX}){o.structVersion=version;call(viewport,o);need(watch->freeze().issue==Issue::version,"Unknown/short version rejected");}
    o=valid();o.next=&o;call(viewport,o);need(watch->freeze().issue==Issue::extension,"Extension rejected");
    o=valid();o.structType={};call(viewport,o);need(watch->freeze().issue==Issue::invalid,"Wrong type rejected");
    for(float bad:{0.f,-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),sl::INVALID_FLOAT}){
        o=valid();o.pre_exposure=bad;call(viewport,o);need(watch->freeze().issue==Issue::invalid,"Invalid exposure rejected");}
    o=valid();o.hdr=sl::eInvalid;call(viewport,o);need(!watch->freeze().valid(),"Uninitialized HDR rejected");
    o=valid();o.width=9000;call(viewport,o);need(!watch->freeze().valid(),"Extent bounded");
    o=valid();call(sl::ViewportHandle(102u),o);need(watch->freeze().issue==Issue::failed,"Failure revokes previous values");
    bool caught=false;try{call(sl::ViewportHandle(101u),o);}catch(const std::runtime_error&){caught=true;}
    need(caught&&watch->freeze().issue==Issue::failed&&watch->snapshot()["active_calls"]==0,"Foreign exception propagates and invalidates");
    call(viewport,o);need(watch->freeze().valid(),"New valid setter recovers");
    // A v2 header at the end of a committed page: reject BEFORE reading a v3 tail.
    auto* pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));need(pages!=nullptr,"Guard allocation");
    DWORD old{};need(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old)!=FALSE,"Guard protection");
    auto* header=reinterpret_cast<sl::BaseStructure*>(pages+4096-sizeof(sl::BaseStructure));
    sl::BaseStructure short_input(OptionsV3::type,2);memcpy(header,&short_input,sizeof(short_input));
    need(decode(&viewport,reinterpret_cast<OptionsV3*>(header)).issue==Issue::version,"No read beyond known short version");
    header->structVersion=3;need(decode(&viewport,reinterpret_cast<OptionsV3*>(header)).issue==Issue::fault,"Truncated v3 caught");
    need(decode(nullptr,&o).issue==Issue::fault,"Invalid viewport pointer caught");
    VirtualFree(pages,0,MEM_RELEASE);
    entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);release_call=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    need(entered&&release_call,"Fixture events");
    std::thread worker([&]{call(sl::ViewportHandle(100u),o);});
    const bool in=WaitForSingleObject(entered,3000)==WAIT_OBJECT_0;
    bool pending_invalid=false,overlap_invalid=false;
    if(in){pending_invalid=!watch->freeze().valid();call(viewport,o);overlap_invalid=!watch->freeze().valid();}
    SetEvent(release_call);worker.join();need(in&&pending_invalid&&overlap_invalid,"In-progress/overlap never accepted");
    need(!watch->freeze().valid(),"Older late return cannot overwrite overlap");
    call(viewport,o);need(watch->freeze().valid(),"Serial setter recovers after overlap");
    // ---- DLSS super resolution: the public DLSSOptions prefix, its own watch
    {auto* sr_watch=new Watch(true);lab::json sr_hooks;
     need(sr_watch->install(reinterpret_cast<void*>(&sr_setter),sr_hooks),"Install the SR options hook");
     need(sr_hooks.value("setter","")=="slDLSSSetOptions","The SR hook names its setter");
     need(!(new Watch(true))->install(reinterpret_cast<void*>(&sr_setter_second),sr_hooks),"One SR watch per process");
     auto volatile sr_call=reinterpret_cast<SetSrOptions*>(&sr_setter);auto s=valid_sr();
     need(sr_call(sl::ViewportHandle(1u),s)==sl::Result::eOk,"SR passthrough");
     const auto sp=sr_watch->freeze();
     need(sp.valid()&&sp.feature==sl::kFeatureDLSS&&sp.viewport==1&&sp.options.width==3840&&sp.options.height==2160&&
          sp.options.pre_exposure==.0038f&&sp.options.hdr==sl::eTrue&&sp.options.flip_x==sl::eFalse&&sp.options.flip_y==sl::eFalse,
          "SR values decoded into the shared option fields");
     need(describe(sp)["feature"]=="sr"&&describe(watch->freeze())["feature"]=="rr","Packets say which upscaler they came from");
     need(watch->freeze().valid()&&watch->freeze().feature==sl::kFeatureDLSS_RR,"The RR watch is untouched by SR calls");
     for(unsigned version:{1u,2u,3u,7u}){s=valid_sr();s.structVersion=version;sr_call(sl::ViewportHandle(1u),s);
         need(sr_watch->freeze().valid(),"Every SR version shares the prefix that is read");}
     s=valid_sr();s.structVersion=0;sr_call(sl::ViewportHandle(1u),s);need(sr_watch->freeze().issue==Issue::version,"SR version 0 rejected");
     s=valid_sr();s.next=&s;sr_call(sl::ViewportHandle(1u),s);need(sr_watch->freeze().issue==Issue::extension,"SR extension rejected");
     s=valid_sr();s.structType={};sr_call(sl::ViewportHandle(1u),s);need(sr_watch->freeze().issue==Issue::invalid,"SR wrong type rejected");
     s=valid_sr();s.hdr=sl::eInvalid;sr_call(sl::ViewportHandle(1u),s);need(!sr_watch->freeze().valid(),"SR uninitialized HDR rejected");
     s=valid_sr();s.pre_exposure=0;sr_call(sl::ViewportHandle(1u),s);need(!sr_watch->freeze().valid(),"SR invalid exposure rejected");
     s=valid_sr();s.mode=7;sr_call(sl::ViewportHandle(1u),s);need(!sr_watch->freeze().valid(),"SR mode bounded");
     s=valid_sr();sr_call(sl::ViewportHandle(102u),s);need(sr_watch->freeze().issue==Issue::failed,"SR setter failure revokes values");
     s=valid_sr();sr_call(sl::ViewportHandle(1u),s);need(sr_watch->freeze().valid(),"SR recovers");
     // Only the documented prefix is read: a prefix ending at a page boundary decodes.
     auto* sr_pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));need(sr_pages!=nullptr,"SR guard allocation");
     DWORD sr_old{};need(VirtualProtect(sr_pages+4096,4096,PAGE_NOACCESS,&sr_old)!=FALSE,"SR guard protection");
     auto* tail=sr_pages+4096-sr_prefix_bytes;const auto prefix=valid_sr();std::memcpy(tail,&prefix,sr_prefix_bytes);
     sl::ViewportHandle one(1u);
     need(decode_sr(&one,tail).valid(),"SR decode reads no byte past the documented prefix");
     VirtualFree(sr_pages,0,MEM_RELEASE);
     sr_watch->stop();}
    // ---- relaxed (controller, self-configuring)
    {auto v4=valid();v4.structVersion=4;v4.next=&v4;
     need(decode(&viewport,&v4).issue==Issue::version,"Strict decode keeps v3 only");
     v4.structVersion=3;need(decode(&viewport,&v4).issue==Issue::extension,"Strict decode keeps refusing a chained extension");
     v4.structVersion=4;const auto relaxed=decode(&viewport,&v4,true);
     need(relaxed.valid()&&relaxed.options.width==5120&&relaxed.options.next==nullptr,"Relaxed decode reads the v3 prefix of a later version and ignores the chain");
     v4.structVersion=2;need(decode(&viewport,&v4,true).issue==Issue::version,"Relaxed decode never reads a shorter older version as v3");
     Watch relaxed_watch;relaxed_watch.relax_before_install();need(relaxed_watch.relaxed(),"Relaxation is chosen before install");
     Packet a;a.issue=Issue::none;a.viewport=0;a.revision=1;a.options=valid();
     Packet b=a;b.viewport=1;b.revision=2;b.options.width=1920;
     WatchTestAccess::store(relaxed_watch,a);WatchTestAccess::store(relaxed_watch,b);
     const auto frozen=relaxed_watch.freeze(0u);
     need(frozen.valid()&&frozen.viewport==0&&frozen.options.width==5120,"Each viewport keeps its own request");
     need(relaxed_watch.unchanged(frozen),"Another viewport's later request does not make ours changed");
     auto again=a;again.revision=3;WatchTestAccess::store(relaxed_watch,again);
     need(relaxed_watch.unchanged(frozen),"The same request sent again (a new revision) is not a change");
     auto moved=a;moved.revision=4;moved.options.width=2560;WatchTestAccess::store(relaxed_watch,moved);
     need(!relaxed_watch.unchanged(frozen),"A different request for our viewport is a change");
     need(relaxed_watch.freeze(7u).issue==Issue::unseen,"A viewport with no request is unseen");}
    const auto before=watch->freeze();watch->stop();call(viewport,o);
    need(!watch->unchanged(before)&&watch->snapshot()["status"]=="stopped","Stop revokes and forwards late calls");
    CloseHandle(entered);CloseHandle(release_call);
    std::cout<<lab::json{{"passed",true},{"checks",checks},{"original_calls",originals.load()},{"hooks",hooks},
        {"actual_game_or_sl_loaded",false},{"nr_executed",false},{"event_files",0},{"raw_textures",0},{"final",watch->snapshot()}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
