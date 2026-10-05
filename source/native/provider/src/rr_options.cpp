// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_rr_options.hpp"
#include <MinHook.h>
#include <cmath>
#include <cstring>

namespace lab::rr {
namespace {
bool bytes(void* dst,const void* src,size_t size) noexcept {
    if(!src)return false;
    __try {memcpy(dst,src,size);return true;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
        ?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
}
bool same(const sl::BaseStructure& a,const sl::BaseStructure& b) noexcept {
    return a.structType==b.structType && a.structVersion==b.structVersion && a.next==b.next;
}
}
const char* name(Issue v) noexcept {
    switch(v){case Issue::none:return "accepted-request";case Issue::unseen:return "not-observed";
    case Issue::invalid:return "unsupported-values";case Issue::version:return "unsupported-version";
    case Issue::extension:return "unsupported-extension";case Issue::fault:return "unreadable-or-changed-input";
    case Issue::failed:return "setter-failed-or-aborted";case Issue::overlap:return "overlapping-options-call";
    case Issue::changed:return "options-changed-during-evaluate";case Issue::stopped:return "stopped";}
    return "unknown";
}
Packet decode(const sl::ViewportHandle* viewport,const OptionsV3* options,bool relaxed) noexcept {
    Packet p;p.thread=GetCurrentThreadId();p.issue=Issue::fault;
    sl::BaseStructure vh({},0),oh({},0);sl::ViewportHandle v(0u);
    if(!bytes(&vh,viewport,sizeof(vh)) || !bytes(&oh,options,sizeof(oh)))return p;
    if(vh.structType!=sl::ViewportHandle::s_structType || oh.structType!=OptionsV3::type){p.issue=Issue::invalid;return p;}
    if(vh.structVersion!=1 || (relaxed?oh.structVersion<3:oh.structVersion!=3)){p.issue=Issue::version;return p;}
    if(vh.next || (oh.next&&!relaxed)){p.issue=Issue::extension;return p;}
    if(!bytes(&v,viewport,sizeof(v)) || !bytes(&p.options,options,sizeof(p.options)) || !same(vh,v) || !same(oh,p.options))return p;
    p.options.next=nullptr;p.viewport=static_cast<unsigned>(v);
    const auto& o=p.options;const auto finite=[](float f){return std::isfinite(f)&&f!=sl::INVALID_FLOAT;};
    const auto boolean=[](sl::Boolean b){return b==sl::eFalse || b==sl::eTrue;};
    p.issue=Issue::invalid;
    // Viewport IDs are opaque. The game-specific admission layer selects one
    // and matches it exactly to the frozen resources, options and inner call.
    if(p.viewport==UINT32_MAX || o.mode>6 || !o.width || !o.height || o.width>8192 || o.height>8192 ||
       !finite(o.pre_exposure) || o.pre_exposure<=0 || !finite(o.exposure_scale) || o.exposure_scale<=0 ||
       !finite(o.sharpness) || !boolean(o.hdr) || !boolean(o.flip_x) || !boolean(o.flip_y) || !boolean(o.alpha))return p;
    p.issue=Issue::none;return p;
}
Packet decode_sr(const sl::ViewportHandle* viewport,const void* options,bool relaxed) noexcept {
    Packet p;p.feature=sl::kFeatureDLSS;p.thread=GetCurrentThreadId();p.issue=Issue::fault;
    sl::BaseStructure vh({},0),oh({},0);sl::ViewportHandle v(0u);
    if(!bytes(&vh,viewport,sizeof(vh)) || !bytes(&oh,options,sizeof(oh)))return p;
    if(vh.structType!=sl::ViewportHandle::s_structType || oh.structType!=SrOptionsPrefix::type){p.issue=Issue::invalid;return p;}
    // Any version from 1 up: the prefix read below exists in all of them.
    if(vh.structVersion!=1 || oh.structVersion<1){p.issue=Issue::version;return p;}
    if(vh.next || (oh.next&&!relaxed)){p.issue=Issue::extension;return p;}
    SrOptionsPrefix s;
    if(!bytes(&v,viewport,sizeof(v)) || !bytes(&s,options,sr_prefix_bytes) || !same(vh,v) || !same(oh,s))return p;
    p.viewport=static_cast<unsigned>(v);
    auto& o=p.options;o.mode=s.mode;o.width=s.width;o.height=s.height;o.sharpness=s.sharpness;
    o.pre_exposure=s.pre_exposure;o.exposure_scale=s.exposure_scale;o.hdr=s.hdr;
    o.flip_x=o.flip_y=o.alpha=sl::eFalse; // SR declares no flip; alpha is not read
    const auto finite=[](float f){return std::isfinite(f)&&f!=sl::INVALID_FLOAT;};
    p.issue=Issue::invalid;
    if(p.viewport==UINT32_MAX || o.mode>6 || !o.width || !o.height || o.width>8192 || o.height>8192 ||
       !finite(o.pre_exposure) || o.pre_exposure<=0 || !finite(o.exposure_scale) || o.exposure_scale<=0 ||
       !finite(o.sharpness) || !(o.hdr==sl::eFalse || o.hdr==sl::eTrue))return p;
    p.issue=Issue::none;return p;
}
std::atomic<Watch*> Watch::installed_{nullptr};
std::atomic<Watch*> Watch::installed_sr_{nullptr};
struct Invocation {
    Watch& w;std::uint64_t revision;Packet packet;bool done=false,overlap=false;
    Invocation(Watch& w,const sl::ViewportHandle& v,const void* o):w(w){
        overlap=w.active_.fetch_add(1)!=0;revision=++w.revision_;
        packet=w.super_resolution_?decode_sr(&v,o,w.relaxed_):decode(&v,static_cast<const OptionsV3*>(o),w.relaxed_);packet.revision=revision;
    }
    void finish(sl::Result result) noexcept {
        if(done)return;done=true;
        if(result!=sl::Result::eOk)packet.issue=Issue::failed;
        // Relaxed, the per-viewport record takes this request whatever other
        // setters ran beside it: they concern their own viewport's slot. Two
        // setters for the SAME viewport racing is the game's race in SL too.
        if(w.relaxed_){auto own=packet;w.store_view(own);}
        if(overlap || w.active_!=1 || w.revision_!=revision)packet.issue=Issue::overlap;
        std::unique_lock lock(w.mutex_,std::try_to_lock);
        if(lock.owns_lock()){if(w.revision_==revision)w.latest_.store(packet);}
        else ++w.loss_;
        --w.active_;
    }
    ~Invocation(){if(!done)finish(sl::Result::eErrorInvalidParameter);}
};
sl::Result Watch::hook(const sl::ViewportHandle& v,const OptionsV3& o) {
    auto* w=installed_.load();auto original=reinterpret_cast<SetOptions*>(w->original_);
    if(w->stopped_)return original(v,o);
    Invocation invocation(*w,v,&o);
    const auto result=original(v,o); // Foreign API is never under our mutex/SEH.
    invocation.finish(result);return result;
}
sl::Result Watch::hook_sr(const sl::ViewportHandle& v,const SrOptionsPrefix& o) {
    auto* w=installed_sr_.load();auto original=reinterpret_cast<SetSrOptions*>(w->original_);
    if(w->stopped_)return original(v,o);
    Invocation invocation(*w,v,&o);
    const auto result=original(v,o);
    invocation.finish(result);return result;
}
bool Watch::install(void* target,json& diagnostics) {
    if(!target || target_ || stopped_)return false;
    auto& slot=super_resolution_?installed_sr_:installed_;
    Watch* empty=nullptr;if(!slot.compare_exchange_strong(empty,this))return false;
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&hook),&pinned)){slot=nullptr;return false;}
    const auto init=MH_Initialize();if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED){slot=nullptr;return false;}
    const auto create=MH_CreateHook(target,super_resolution_?reinterpret_cast<void*>(&hook_sr):reinterpret_cast<void*>(&hook),&original_);
    diagnostics={{"target",reinterpret_cast<std::uint64_t>(target)},{"create",create},{"process_pinned",true},
        {"setter",super_resolution_?"slDLSSSetOptions":"slDLSSDSetOptions"}};
    if(create!=MH_OK){slot=nullptr;return false;}
    target_=target;const auto enable=MH_EnableHook(target);diagnostics["enable"]=enable;
    if(enable!=MH_OK){stop();return false;} // Keep trampoline/object alive; no hot-unload.
    return true;
}
void Watch::store_view(const Packet& p) noexcept {
    if(p.viewport==UINT32_MAX)return;
    for(auto& slot:views_){const auto old=slot.load();if(old.revision&&old.viewport==p.viewport){slot.store(p);return;}}
    for(auto& slot:views_)if(!slot.load().revision){slot.store(p);return;}
    views_[next_view_.fetch_add(1)%kViews].store(p);
}
Packet Watch::view(unsigned viewport) const noexcept {
    for(const auto& slot:views_){auto p=slot.load();if(p.revision&&p.viewport==viewport)return p;}
    return {};
}
Packet Watch::freeze(unsigned viewport) const noexcept {
    if(!relaxed_)return freeze();
    Packet out;if(stopped_){out.issue=Issue::stopped;return out;}
    return view(viewport);
}
Packet Watch::freeze() const noexcept {
    Packet out;if(stopped_){out.issue=Issue::stopped;return out;}
    const auto revision=revision_.load();if(active_){out.issue=Issue::overlap;return out;}
    out=latest_.load();if(active_ || revision!=revision_ || out.revision!=revision)out.issue=Issue::changed;
    return out;
}
bool Watch::unchanged(const Packet& p) const noexcept {
    // Relaxed: a game that re-sends the same request every frame, from another
    // thread while this Evaluate runs, has not changed anything.
    if(relaxed_){if(!p.valid()||stopped_)return false;const auto now=view(p.viewport);
        return now.valid()&&(now.revision==p.revision||std::memcmp(&now.options,&p.options,sizeof(p.options))==0);}
    return p.valid() && !stopped_ && !active_ && p.revision==revision_;
}
json describe(const Packet& p) {
    json out={{"status",name(p.issue)},{"feature",p.feature==sl::kFeatureDLSS?"sr":"rr"},{"revision",p.revision},{"values_observed",p.valid()},
        {"interpretation","successful-setter-request-not-model-effective-values"},
        {"linear_rgb_verified",false},{"exposure_texture_read",false},{"actual_preset_verified",false}};
    if(p.valid()){const auto& o=p.options;out.update({{"viewport",p.viewport},{"thread",p.thread},{"mode_raw",o.mode},
        {"output_width",o.width},{"output_height",o.height},{"color_buffers_hdr",o.hdr==sl::eTrue},
        {"pre_exposure",o.pre_exposure},{"exposure_scale",o.exposure_scale},{"sharpness",o.sharpness},
        {"presets_raw",o.presets},{"preset_order",{"dlaa","quality","balanced","performance","ultra-performance","ultra-quality"}}});}
    return out;
}
json Watch::snapshot() const {
    auto out=describe(freeze());out["hook_installed"]=target_!=nullptr;
    out["callback_losses"]=loss_.load();out["active_calls"]=active_.load();out["event_files"]=0;
    out["stopped"]=stopped_.load();return out;
}
}
