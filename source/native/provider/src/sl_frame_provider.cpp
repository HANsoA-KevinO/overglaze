// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The SL boundary -> NR frame translation, shared by both tracks: the research
// host and the controller host each own one of these in front of their bridge
// client.
// Nothing here schedules work: it validates the resource roles, the accepted RR
// options and the guide conventions, and fills a live::Frame or names the
// reason it cannot. A verified resource role plus accepted options defines the
// narrow test profile; it is not a proof of pixel gamut or official exposure.
#include "lab_sl_frame_provider.hpp"
#include "lab_sl_guide_contract.hpp"
#include "lab_live_frame_prescreen.hpp"
#include <cmath>
namespace lab {

const char* translate_rr_frame(const slboundary::Resolution& r,const rr::Packet& p,void* command,
                               const FrameProfile& profile,live::Frame& f) noexcept {
    if(!r.ready()||!p.valid())return "Incomplete RR boundary resolution or options packet";
    const auto& b=r.binding;const auto& c=b.constants;const auto& o=p.options;
    const bool linear=profile.linear_depth;
    if(p.feature!=b.feature)return "Options belong to the other upscaler";
    if(b.resources[1].type!=(linear?sl::kBufferTypeLinearDepth:sl::kBufferTypeDepth))return "Depth semantic differs from selected profile";
    if(profile.self_configure){
        if(o.hdr!=sl::eTrue)return "Colour is not declared HDR";
        // No flip check here. The two DLSSDOptions fields after colorBuffersHDR
        // are indicatorInvertAxisX/Y -- the orientation of DLSS's on-screen
        // DEBUG indicator (NVIDIA Streamline include/sl_dlss_d.h) -- not an
        // image flip. Refusing on them refused a debug-overlay setting; the
        // pinned profile below still requires both false, unchanged.
        if(!(std::isfinite(o.pre_exposure)&&o.pre_exposure>0.f)||!(std::isfinite(o.exposure_scale)&&o.exposure_scale>0.f))
            return "Exposure values are not finite and positive";
    }else if(o.hdr!=sl::eTrue||o.pre_exposure!=1.f||o.exposure_scale!=1.f||o.flip_x!=sl::eFalse||o.flip_y!=sl::eFalse)
        return "RR HDR/exposure/flip differs from the admitted experimental profile";
    f={};f.call=b.call;f.frame=b.frame_index;f.command=static_cast<ID3D12GraphicsCommandList*>(command);
    if(!f.frame||b.viewport!=profile.viewport)return "Frame or selected game viewport mismatch";
    ID3D12Resource** output[]{&f.color,&f.depth,&f.motion};D3D12_RESOURCE_DESC desc[3]{};
    for(unsigned i=0;i<3;++i){const auto& tag=b.resources[i];*output[i]=static_cast<ID3D12Resource*>(tag.native);
        if(!*output[i]||tag.issues||tag.null_resource||tag.resource_type!=sl::ResourceType::eTex2d)return "Invalid live texture tag";
        desc[i]=(*output[i])->GetDesc();const auto& e=tag.extent;
        if(e.left||e.top||((e.width||e.height)&&(e.width!=desc[i].Width||e.height!=desc[i].Height)))return "Partial RR extent unsupported";}
    if(desc[0].Width>8192||desc[1].Width>8192||desc[2].Width!=desc[1].Width||desc[2].Height!=desc[1].Height)return "Guide extent mismatch";
    f.width=static_cast<unsigned>(desc[0].Width);f.height=desc[0].Height;
    f.guide_width=static_cast<unsigned>(desc[1].Width);f.guide_height=desc[1].Height;
    if(o.width!=f.width||o.height!=f.height)return "RR requested/output dimensions differ";
    f.color_state=b.resources[0].state;f.depth_state=b.resources[1].state;f.motion_state=b.resources[2].state;
    if(profile.self_configure){
        // Everything the runtime would otherwise refuse as a terminal format
        // fault, refused here by role; the actual numbers are in observed().
        const auto color=desc[0].Format,depth=desc[1].Format,motion=desc[2].Format;
        if(color!=DXGI_FORMAT_R16G16B16A16_FLOAT&&color!=DXGI_FORMAT_R11G11B10_FLOAT)
            return "Colour format is not RGBA16F or R11G11B10F";
        if(motion!=DXGI_FORMAT_R16G16_FLOAT&&motion!=DXGI_FORMAT_R16G16B16A16_FLOAT)
            return "Motion format is not RG16F or RGBA16F";
        const bool typed=depth==DXGI_FORMAT_R32_FLOAT||depth==DXGI_FORMAT_R16_FLOAT;
        const bool typeless=depth==DXGI_FORMAT_R32_TYPELESS||depth==DXGI_FORMAT_R32G8X24_TYPELESS;
        if(!typed&&!typeless)return "Depth format is not R32F, R16F, R32 typeless or D32S8 typeless";
        if(linear&&!typed)return "Linear depth in a typeless format is unsupported";
        // Declared resource states, with exactly the runtime's rules: it would
        // otherwise stop NR for good on the first frame that breaks them. Alan
        // Wake 2 declared its R16_FLOAT linear depth as DEPTH_WRITE (16) while
        // path tracing was being switched off -- a state that format cannot even
        // have -- and NR ended there. We never transition out of a state we do
        // not believe, so such a frame is skipped, by name, and ON stays.
        constexpr unsigned read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto guide_ok=[&](unsigned state,bool is_depth){
            const unsigned allowed=read|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|(is_depth?D3D12_RESOURCE_STATE_DEPTH_READ:0u);
            return state==D3D12_RESOURCE_STATE_COMMON||state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS||((state&read)&&!(state&~allowed));};
        if(!guide_ok(b.resources[1].state,true))return "Depth guide declared in a state NR cannot read (e.g. DEPTH_WRITE)";
        if(!guide_ok(b.resources[2].state,false))return "Motion guide declared in a state NR cannot read";
        const unsigned out_state=b.resources[0].state;
        if((out_state!=D3D12_RESOURCE_STATE_UNORDERED_ACCESS&&out_state!=D3D12_RESOURCE_STATE_COMMON)||
           !(desc[0].Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return "Output colour not declared UAV/COMMON or not UAV-capable";
        f.packed_guides=typed?1u:0u;
    }else f.packed_guides=linear?1u:0u;
    f.depth_encoding=linear?nr::DepthEncoding::view_z_magnitude:nr::DepthEncoding::hardware;
    // The live path must use the same extent/finite/sign rules as the tested
    // independent adapter. Refuse overflow/invalid extents before delivery.
    try{const auto guides=nr::sl_guide_constants(c,f.guide_width,f.guide_height);
        f.motion_x=guides.scale_x;f.motion_y=guides.scale_y;f.depth_inverted=guides.depth_inverted;f.reset=guides.reset;
        if(linear){f.depth_projection=nr::sl_view_depth_projection(c);
            // Also checked by the runtime, terminally; here it is one skip.
            if(profile.self_configure){f.depth_projection.validate();
                if(f.depth_projection.inverted!=f.depth_inverted)return "Linear depth direction disagrees with the depth constants";}}
    }catch(const std::logic_error&){return linear&&profile.self_configure
        ?"SL guide conventions or linear depth projection (near/far/clip) unsupported"
        :"SL guide conventions, extents or converted scales unsupported";}
    // Every remaining per-frame rule of the runtime, each of which would end
    // NR for good there (nr_live_core.hpp validate): here one skipped frame.
    if(profile.self_configure)if(const auto* why=live::prescreen(f))return why;
    return nullptr;
}

void SlFrameProvider::game_binding_enter(const slboundary::Resolution& r,void* command) noexcept {
    if(r.ready())receiver_.game_binding_enter(r.binding.call,r.binding.frame_index,command);
}
void SlFrameProvider::game_binding_exit(std::uint64_t call) noexcept {receiver_.game_binding_exit(call);}

void SlFrameProvider::self_configure(const slboundary::Resolution& r,const rr::Packet& p,FrameProfile& profile) noexcept {
    // The binding layer already selected the viewport and the depth tag; the
    // depth kind is that tag's own advertised type.
    profile.viewport=r.binding.viewport;
    profile.linear_depth=r.binding.resources[1].type==sl::kBufferTypeLinearDepth;
    try{
        const auto format=[&](unsigned i)->json{auto* res=static_cast<ID3D12Resource*>(r.binding.resources[i].native);
            return res?json(static_cast<unsigned>(res->GetDesc().Format)):json(nullptr);};
        json facts{{"feature",r.binding.feature==sl::kFeatureDLSS?"sr":"rr"},{"viewport",r.binding.viewport},{"depth",profile.linear_depth?"linear":"hardware"},
            {"formats",{{"color",format(0)},{"depth",format(1)},{"motion",format(2)}}},
            {"states",{{"color",r.binding.resources[0].state},{"depth",r.binding.resources[1].state},{"motion",r.binding.resources[2].state}}},
            {"output",json::array({p.options.width,p.options.height})},
            {"hdr",p.options.hdr==sl::eTrue},{"pre_exposure",p.options.pre_exposure},{"exposure_scale",p.options.exposure_scale},
            {"scope","read from the game's own RR or SR calls, not from the package; formats are DXGI numbers"}};
        std::lock_guard lock(self_mutex_);facts["calls"]=++self_calls_;observed_=std::move(facts);
    }catch(...){}
}
json SlFrameProvider::observed()const{std::lock_guard lock(self_mutex_);return observed_;}

void SlFrameProvider::enter(const slboundary::Resolution& r,const rr::Packet& p,void* command) noexcept {
    live::Frame f;
    auto profile=profile_;
    if(profile_.self_configure&&r.ready()&&p.valid())self_configure(r,p,profile);
    if(const auto* why=translate_rr_frame(r,p,command,profile,f)){
        blocked_=why;
        // Nothing of ours was recorded in this call; report it as a refusal so
        // the backend can keep the user's ON gate and reset history instead.
        RejectedCall rejection;rejection.call=r.ready()?r.binding.call:0;rejection.frame=r.ready()?r.binding.frame_index:0;
        strcpy_s(rejection.stage,"live-profile");strncpy_s(rejection.reason,why,_TRUNCATE);
        // Self-configuring, a refusal names a property of the GAME's call, not a
        // wrong pinned fact, and it follows a successful original return with
        // nothing of ours recorded: a skip that keeps ON and shows its reason.
        if(profile_.self_configure)rejection.disposition=RejectedDisposition::skipped_before_insertion;
        receiver_.rejected(rejection);
        return;
    }
    blocked_=nullptr;
    // The collector publishes its own per-call context before the frame.
    if(observer_)observer_->admitted_frame_context(r,p,f);
    receiver_.frame(f);
}
void SlFrameProvider::boundary_returned(std::uint64_t call,bool success) noexcept {receiver_.boundary_returned(call,success);}
void SlFrameProvider::aborted(std::uint64_t call) noexcept {receiver_.aborted(call);}
void SlFrameProvider::unavailable() noexcept {unavailable(RejectedCall{});}
void SlFrameProvider::unavailable(const RejectedCall& why) noexcept {
    blocked_="Live RR callback pairing unavailable";
    receiver_.rejected(why);
}
}
