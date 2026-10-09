// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The one SL boundary -> NR frame translation. WARP textures give the
// translation real ID3D12Resource
// descriptors; nothing is rendered and no queue is created.
//
// Covered: the accepted profile, the linear-depth profile, every refusal the
// translation can report, and the delivery order a collector depends on
// (admitted_frame_context strictly before frame()).
#include "lab_sl_frame_provider.hpp"
#include <wrl/client.h>
#include <dxgi1_6.h>
#include <iostream>
#include <string>
#include <vector>
#include <limits>

using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("HRESULT="+std::to_string(static_cast<unsigned long>(r)));}

ComPtr<ID3D12Resource> texture(ID3D12Device* d,UINT64 width,UINT height,DXGI_FORMAT format,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE,UINT16 array=1,UINT16 mips=1){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
    desc.DepthOrArraySize=array;desc.MipLevels=mips;desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=flags;
    ComPtr<ID3D12Resource> r;hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)));
    return r;
}

// Records the order in which the provider delivers to its two sinks.
struct Receiver final:lab::live::IFrameReceiver {
    std::vector<std::string> order;
    lab::live::Frame last{};
    lab::RejectedCall rejection{};
    void frame(const lab::live::Frame& f)noexcept override{order.push_back("frame");last=f;}
    void boundary_returned(std::uint64_t call,bool ok)noexcept override{order.push_back(ok?"returned":"returned-failed");}
    void aborted(std::uint64_t)noexcept override{order.push_back("aborted");}
    void rejected(const lab::RejectedCall& why)noexcept override{order.push_back("rejected");rejection=why;}
    void game_binding_enter(std::uint64_t,std::uint64_t,void*)noexcept override{order.push_back("binding-enter");}
    void game_binding_exit(std::uint64_t)noexcept override{order.push_back("binding-exit");}
};
struct Observer final:lab::IResearchObserver {
    std::vector<std::string>* order=nullptr;
    unsigned contexts=0;std::uint64_t last_call=0;
    void admitted_frame_context(const lab::slboundary::Resolution&,const lab::rr::Packet&,const lab::live::Frame& f)noexcept override{
        ++contexts;last_call=f.call;if(order)order->push_back("context");
    }
};
}

int main(){try{
    ComPtr<IDXGIFactory4> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp;hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    auto color=texture(device.Get(),64,32,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto hardware_depth=texture(device.Get(),32,16,DXGI_FORMAT_R32_TYPELESS);
    auto linear_depth=texture(device.Get(),32,16,DXGI_FORMAT_R32_FLOAT);
    auto packed_motion=texture(device.Get(),32,16,DXGI_FORMAT_R16G16_FLOAT);
    auto legacy_motion=texture(device.Get(),32,16,DXGI_FORMAT_R16G16B16A16_FLOAT);
    void* const command=reinterpret_cast<void*>(0x4000);

    // A resolution the adapter would consider ready, with accepted RR options.
    const auto make=[&](bool linear){
        lab::slboundary::Resolution r;r.rejection=lab::slboundary::Rejection::none;
        auto& b=r.binding;b.call=7;b.frame_index=11;b.viewport=0;b.command=command;
        b.constants.mvecScale={-.5f,.5f};b.constants.jitterOffset={.25f,-.125f};
        // The public guide contract these tests are not varying.
        b.constants.depthInverted=sl::eFalse;b.constants.reset=sl::eFalse;b.constants.cameraMotionIncluded=sl::eTrue;
        b.constants.motionVectors3D=sl::eFalse;b.constants.motionVectorsJittered=sl::eFalse;
        b.constants.orthographicProjection=sl::eFalse;b.constants.motionVectorsDilated=sl::eFalse;
        // A finite right-handed perspective view-to-clip matrix that agrees with
        // its own clipping planes, which the linear-depth profile requires.
        const float near_plane=.1f,far_plane=100.f;
        const float projection_a=far_plane/(near_plane-far_plane),projection_b=near_plane*projection_a;
        b.constants.cameraViewToClip[0]={1,0,0,0};b.constants.cameraViewToClip[1]={0,1,0,0};
        b.constants.cameraViewToClip[2]={0,0,projection_a,-1};b.constants.cameraViewToClip[3]={0,0,projection_b,0};
        b.constants.cameraNear=near_plane;b.constants.cameraFar=far_plane;
        b.resources[0]={sl::kBufferTypeScalingOutputColor,sl::eValidUntilPresent,{},color.Get(),sl::ResourceType::eTex2d,0,false,0};
        b.resources[1]={linear?sl::kBufferTypeLinearDepth:sl::kBufferTypeDepth,sl::eValidUntilEvaluate,{},
            linear?linear_depth.Get():hardware_depth.Get(),sl::ResourceType::eTex2d,0,false,0};
        b.resources[2]={sl::kBufferTypeMotionVectors,sl::eValidUntilEvaluate,{},
            linear?packed_motion.Get():legacy_motion.Get(),sl::ResourceType::eTex2d,0,false,0};
        return r;
    };
    lab::rr::Packet packet;packet.issue=lab::rr::Issue::none;packet.viewport=0;
    auto& options=packet.options;options.width=64;options.height=32;options.hdr=sl::eTrue;
    options.pre_exposure=1.f;options.exposure_scale=1.f;options.flip_x=options.flip_y=sl::eFalse;

    const lab::FrameProfile hardware_profile{0,false},linear_profile{0,true};
    lab::live::Frame f{};
    const auto* accepted=lab::translate_rr_frame(make(false),packet,command,hardware_profile,f);
    need(accepted==nullptr,std::string("Accepted hardware-depth profile translates, got: ")+(accepted?accepted:""));
    need(f.call==7&&f.frame==11&&f.command==command,"Call identity carried");
    need(f.width==64&&f.height==32&&f.guide_width==32&&f.guide_height==16,"Extents come from the actual descriptors");
    need(f.color==color.Get()&&f.depth==hardware_depth.Get()&&f.motion==legacy_motion.Get(),"Resource roles in order");
    need(!f.packed_guides&&f.depth_encoding==lab::nr::DepthEncoding::hardware,"Hardware depth keeps the legacy guide layout");
    need(f.motion_x==-.5f*32&&f.motion_y==.5f*16,"Motion scale converted to NR pixel units");
    need(f.abi==lab::live::version&&f.size==sizeof(lab::live::Frame),"Frame carries its own ABI stamp");

    lab::live::Frame linear{};
    need(lab::translate_rr_frame(make(true),packet,command,linear_profile,linear)==nullptr,"Accepted linear-depth profile translates");
    need(linear.packed_guides==1&&linear.depth_encoding==lab::nr::DepthEncoding::view_z_magnitude,"Linear depth requires typed guides");

    // Every refusal the translation can report, one changed variable each.
    const auto refuses=[&](const char* expected,lab::slboundary::Resolution r,lab::rr::Packet p,const lab::FrameProfile& profile){
        lab::live::Frame out{};const auto* why=lab::translate_rr_frame(r,p,command,profile,out);
        need(why!=nullptr&&std::string(why)==expected,std::string("expected refusal '")+expected+"', got '"+(why?why:"accepted")+"'");
    };
    {auto r=make(false);r.rejection=lab::slboundary::Rejection::not_target;
     refuses("Incomplete RR boundary resolution or options packet",r,packet,hardware_profile);}
    {auto p=packet;p.issue=lab::rr::Issue::unseen;
     refuses("Incomplete RR boundary resolution or options packet",make(false),p,hardware_profile);}
    refuses("Depth semantic differs from selected profile",make(false),packet,linear_profile);
    refuses("Depth semantic differs from selected profile",make(true),packet,hardware_profile);
    {auto p=packet;p.options.hdr=sl::eFalse;
     refuses("RR HDR/exposure/flip differs from the admitted experimental profile",make(false),p,hardware_profile);}
    {auto p=packet;p.options.pre_exposure=.5f;
     refuses("RR HDR/exposure/flip differs from the admitted experimental profile",make(false),p,hardware_profile);}
    {auto p=packet;p.options.flip_y=sl::eTrue;
     refuses("RR HDR/exposure/flip differs from the admitted experimental profile",make(false),p,hardware_profile);}
    {auto r=make(false);r.binding.frame_index=0;refuses("Frame or selected game viewport mismatch",r,packet,hardware_profile);}
    {auto r=make(false);r.binding.viewport=1;refuses("Frame or selected game viewport mismatch",r,packet,hardware_profile);}
    {auto r=make(false);r.binding.resources[0].native=nullptr;refuses("Invalid live texture tag",r,packet,hardware_profile);}
    {auto r=make(false);r.binding.resources[1].issues=1;refuses("Invalid live texture tag",r,packet,hardware_profile);}
    {auto r=make(false);r.binding.resources[2].null_resource=true;refuses("Invalid live texture tag",r,packet,hardware_profile);}
    {auto r=make(false);r.binding.resources[0].resource_type=sl::ResourceType::eBuffer;refuses("Invalid live texture tag",r,packet,hardware_profile);}
    {auto r=make(false);r.binding.resources[0].extent={4,0,64,32};refuses("Partial RR extent unsupported",r,packet,hardware_profile);}
    {auto r=make(false);r.binding.resources[1].extent={0,0,16,16};refuses("Partial RR extent unsupported",r,packet,hardware_profile);}
    {auto r=make(false);r.binding.resources[2].native=color.Get();refuses("Guide extent mismatch",r,packet,hardware_profile);}
    {auto p=packet;p.options.width=128;refuses("RR requested/output dimensions differ",make(false),p,hardware_profile);}
    {auto r=make(false);r.binding.constants.mvecScale={std::numeric_limits<float>::infinity(),0};
     refuses("SL guide conventions, extents or converted scales unsupported",r,packet,hardware_profile);}

    // Delivery order, and what each sink is told.
    Receiver receiver;Observer observer;observer.order=&receiver.order;
    lab::SlFrameProvider provider(receiver,hardware_profile,&observer);
    provider.game_binding_enter(make(false),command);
    provider.enter(make(false),packet,command);
    provider.boundary_returned(7,true);
    provider.game_binding_exit(7);
    need(receiver.order==std::vector<std::string>({"binding-enter","context","frame","returned","binding-exit"}),
         "Context strictly before the frame, inside the matched binding scope");
    need(observer.contexts==1&&observer.last_call==7,"Observer sees the translated call once");
    need(receiver.last.color==color.Get()&&!provider.blocked(),"Delivered frame is the translated one; nothing blocked");

    // A refusal reaches the receiver as a rejection, never as a frame, and the
    // reason is readable for the panel.
    Receiver refused;lab::SlFrameProvider blocked_provider(refused,linear_profile);
    blocked_provider.enter(make(false),packet,command);
    need(refused.order==std::vector<std::string>({"rejected"}),"A refusal delivers no frame");
    need(std::string(refused.rejection.stage)=="live-profile"&&
         std::string(refused.rejection.reason)=="Depth semantic differs from selected profile"&&
         refused.rejection.call==7&&refused.rejection.frame==11,"Rejection names the call and the reason");
    need(std::string(blocked_provider.blocked())=="Depth semantic differs from selected profile","Blocked reason is readable");
    // An unavailable pairing is a rejection too, and it clears no earlier block.
    blocked_provider.unavailable();
    need(refused.order.size()==2&&refused.order.back()=="rejected"&&
         std::string(blocked_provider.blocked())=="Live RR callback pairing unavailable","Unavailable pairing reports itself");

    // ---- self-configuration (controller host): the facts come from each call
    {
        const lab::FrameProfile auto_profile{0,false,true};
        Receiver seen;lab::SlFrameProvider self(seen,auto_profile);
        need(self.observed().is_null(),"Nothing observed before the first call");
        self.enter(make(true),packet,command);   // linear depth, R32_FLOAT: derived, not pinned
        need(seen.order==std::vector<std::string>({"frame"}),"Linear depth admitted without a pinned fact");
        need(seen.last.depth_encoding==lab::nr::DepthEncoding::view_z_magnitude&&seen.last.packed_guides==1,"Depth kind derived from the tag type");
        {const auto o=self.observed();
         need(o.value("depth","")=="linear"&&o.value("viewport",-1)==0&&o["formats"].value("depth",0u)==unsigned(DXGI_FORMAT_R32_FLOAT),
              "Observed facts published");}
        self.enter(make(false),packet,command);  // hardware depth, typeless
        need(seen.order.back()=="frame"&&seen.last.depth_encoding==lab::nr::DepthEncoding::hardware&&!seen.last.packed_guides,
             "Hardware typeless depth derived as the legacy layout");
        // The viewport is whatever the binding layer resolved (it selects it now).
        {auto other=make(false);other.binding.viewport=3;self.enter(other,packet,command);
         need(seen.order.back()=="frame"&&self.observed().value("viewport",-1)==3,"The resolved viewport is taken, not a pinned one");}
        // Refusals while self-configuring are skips that keep ON.
        {auto r=make(false);r.binding.resources[1].native=packed_motion.Get();
         self.enter(r,packet,command);
         need(seen.order.back()=="rejected"&&seen.rejection.disposition==lab::RejectedDisposition::skipped_before_insertion,
              "A self-configuring refusal is a skip, never a terminal fault");}
        // Exposure: any finite positive value; HDR required.
        lab::live::Frame out{};
        auto p2=packet;p2.options.pre_exposure=.0038f;p2.options.exposure_scale=2.f;
        need(lab::translate_rr_frame(make(false),p2,command,auto_profile,out)==nullptr,"A game pre-exposure is accepted when self-configuring");
        {auto q=packet;q.options.hdr=sl::eFalse;refuses("Colour is not declared HDR",make(false),q,auto_profile);}
        // The fields after colorBuffersHDR are the DLSS debug indicator's axes,
        // not an image flip: no refusal when self-configuring.
        {auto q=packet;q.options.flip_x=sl::eTrue;q.options.flip_y=sl::eTrue;lab::live::Frame g{};
         need(lab::translate_rr_frame(make(false),q,command,auto_profile,g)==nullptr,"Indicator axes are not a refusal when self-configuring");}
        {auto q=packet;q.options.pre_exposure=0.f;refuses("Exposure values are not finite and positive",make(false),q,auto_profile);}
        // Layout from the FORMAT: hardware depth stored as R32_FLOAT is typed.
        auto typed_hw=make(false);typed_hw.binding.resources[1].native=linear_depth.Get();
        need(lab::translate_rr_frame(typed_hw,packet,command,auto_profile,out)==nullptr&&out.packed_guides==1&&
             out.depth_encoding==lab::nr::DepthEncoding::hardware,"Hardware depth in R32_FLOAT gets the typed layout");
        need(lab::translate_rr_frame(typed_hw,packet,command,hardware_profile,out)==nullptr&&!out.packed_guides,
             "The research host's pinned rule is unchanged");
        {auto r=make(true);r.binding.resources[1].native=hardware_depth.Get();
         refuses("Linear depth in a typeless format is unsupported",r,packet,lab::FrameProfile{0,true,true});} // as the provider derives it
        // Formats the runtime would refuse terminally are refused here by role.
        {auto r=make(false);r.binding.resources[1].native=packed_motion.Get();
         refuses("Depth format is not R32F, R16F, R32 typeless or D32S8 typeless",r,packet,auto_profile);}
        {auto r=make(false);r.binding.resources[2].native=hardware_depth.Get();
         refuses("Motion format is not RG16F or RGBA16F",r,packet,auto_profile);}
        {auto wrong_color=texture(device.Get(),64,32,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);auto r=make(false);r.binding.resources[0].native=wrong_color.Get();
         refuses("Colour format is not RGBA16F or R11G11B10F",r,packet,auto_profile);}
        // Two-channel motion with typeless depth (RE Engine) is accepted.
        {auto r=make(false);r.binding.resources[2].native=packed_motion.Get();
         need(lab::translate_rr_frame(r,packet,command,auto_profile,out)==nullptr&&!out.packed_guides,"Typeless depth with RG16F motion is accepted");}
        // An SR call (binding and options both from SR) translates like RR.
        {auto r=make(false);r.binding.feature=sl::kFeatureDLSS;auto q=packet;q.feature=sl::kFeatureDLSS;
         need(lab::translate_rr_frame(r,q,command,auto_profile,out)==nullptr&&out.color==color.Get(),"An SR call translates like RR");
         refuses("Options belong to the other upscaler",r,packet,auto_profile);
         self.enter(r,q,command);
         need(seen.order.back()=="frame"&&self.observed().value("feature","")=="sr","The provider reports which upscaler was used");}
        // Declared states follow the runtime's rules, refused here as skips.
        {auto r=make(false);r.binding.resources[1].state=D3D12_RESOURCE_STATE_DEPTH_WRITE;
         refuses("Depth guide declared in a state NR cannot read (e.g. DEPTH_WRITE)",r,packet,auto_profile);
         self.enter(r,packet,command);
         need(seen.order.back()=="rejected"&&seen.rejection.disposition==lab::RejectedDisposition::skipped_before_insertion&&
              self.observed()["states"].value("depth",0u)==unsigned(D3D12_RESOURCE_STATE_DEPTH_WRITE),
              "A guide in an unreadable state is a skip, and its state is reported (Alan Wake 2)");}
        {auto r=make(false);r.binding.resources[1].state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_DEPTH_READ;
         need(lab::translate_rr_frame(r,packet,command,auto_profile,out)==nullptr,"Depth read states are accepted");}
        {auto r=make(false);r.binding.resources[2].state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
         refuses("Motion guide declared in a state NR cannot read",r,packet,auto_profile);}
        {auto r=make(false);r.binding.resources[2].state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
         need(lab::translate_rr_frame(r,packet,command,auto_profile,out)==nullptr,"A guide declared UAV on its own is accepted (Alan Wake 2 motion)");}
        {auto r=make(false);r.binding.resources[0].state=D3D12_RESOURCE_STATE_RENDER_TARGET;
         refuses("Output colour not declared UAV/COMMON or not UAV-capable",r,packet,auto_profile);}
        {auto plain=texture(device.Get(),64,32,DXGI_FORMAT_R16G16B16A16_FLOAT);auto r=make(false);r.binding.resources[0].native=plain.Get();
         refuses("Output colour not declared UAV/COMMON or not UAV-capable",r,packet,auto_profile);}
        // The runtime's remaining per-frame rules (shared prescreen): a skip
        // by name here, where the runtime would have stopped NR.
        {auto layered=texture(device.Get(),32,16,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_FLAG_NONE,2);auto r=make(false);r.binding.resources[2].native=layered.Get();
         refuses("Motion is a texture array or not 2D",r,packet,auto_profile);
         need(lab::translate_rr_frame(r,packet,command,hardware_profile,out)==nullptr,"The pinned profile leaves that check to the runtime, unchanged");}
        {auto mipped=texture(device.Get(),32,16,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_FLAG_NONE,1,3);auto r=make(false);r.binding.resources[2].native=mipped.Get();
         refuses("Motion carries a mip chain",r,packet,auto_profile);}
        // The strict profile still refuses a game pre-exposure.
        {auto q=packet;q.options.pre_exposure=.5f;
         refuses("RR HDR/exposure/flip differs from the admitted experimental profile",make(false),q,hardware_profile);}

        // ---- the game's exposure (Live ABI24): carried for auto exposure, never a refusal
        using lab::live::ExposureNote;
        constexpr unsigned nps=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto exposure=texture(device.Get(),1,1,DXGI_FORMAT_R32_FLOAT);
        const auto with=[&](lab::slboundary::Resolution r,ID3D12Resource* x,unsigned state){
            r.binding.exposure={sl::kBufferTypeExposure,sl::eValidUntilPresent,{},x,sl::ResourceType::eTex2d,state,false,0};
            r.binding.exposure_note=0;r.binding.exposure_call=r.binding.call;return r;};
        // 007: no exposure tag, pre-exposure and scale 1 -> the runtime's meter.
        need(lab::translate_rr_frame(make(false),packet,command,hardware_profile,out)==nullptr&&!out.exposure&&
             out.exposure_note==unsigned(ExposureNote::no_texture)&&out.pre_exposure==1.f&&out.exposure_scale==1.f,"No exposure tag: no texture, named");
        // Alan Wake 2's shape: tag type 13 beside linear depth, a game pre-exposure.
        {auto q=packet;q.options.pre_exposure=.25f;q.options.exposure_scale=2.f;
         const auto* why=lab::translate_rr_frame(with(make(true),exposure.Get(),nps),q,command,lab::FrameProfile{0,true,true},out);
         need(why==nullptr&&out.exposure==exposure.Get()&&out.exposure_note==0&&out.exposure_state==nps&&out.pre_exposure==.25f&&out.exposure_scale==2.f,
              std::string("A tagged 1x1 exposure texture is carried with the options' pre-exposure and scale: ")+(why?why:""));
         self.enter(with(make(true),exposure.Get(),nps),q,command);
         need(seen.order.back()=="frame"&&seen.last.exposure==exposure.Get()&&
              self.observed()["exposure_texture"].value("binding","")=="none"&&self.observed()["exposure_texture"].value("format",0u)==unsigned(DXGI_FORMAT_R32_FLOAT),
              "Delivered, and the tag is reported among the observed facts");}
        need(lab::translate_rr_frame(with(make(false),exposure.Get(),nps),packet,command,hardware_profile,out)==nullptr&&out.exposure==exposure.Get(),
             "The pinned profile carries it too (pre-exposure and scale are 1 there)");
        // What the binding layer could not bind is passed on by name.
        {auto r=make(false);r.binding.exposure_note=unsigned(ExposureNote::tag_not_fresh);
         need(lab::translate_rr_frame(r,packet,command,auto_profile,out)==nullptr&&!out.exposure&&out.exposure_note==unsigned(ExposureNote::tag_not_fresh),
              "The binding's note is kept and the frame admitted");}
        // An unusable texture is dropped with its note; the frame is kept.
        {auto wide=texture(device.Get(),2,2,DXGI_FORMAT_R32_FLOAT);auto ldr=texture(device.Get(),1,1,DXGI_FORMAT_R8_UNORM);
         const struct {lab::slboundary::Resolution r;ExposureNote note;const char* what;} cases[]{
            {with(make(false),wide.Get(),nps),ExposureNote::unsupported_shape,"2x2"},
            {with(make(false),ldr.Get(),nps),ExposureNote::unsupported_format,"R8_UNORM"},
            {with(make(false),exposure.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET),ExposureNote::unsupported_state,"render target state"},
            {with(make(false),color.Get(),0),ExposureNote::aliased,"the output colour"}};
         for(const auto& c:cases)need(lab::translate_rr_frame(c.r,packet,command,auto_profile,out)==nullptr&&!out.exposure&&out.exposure_note==unsigned(c.note)&&out.color==color.Get(),
             std::string("Unusable exposure is a note, not a refusal: ")+c.what);
         auto r=with(make(false),exposure.Get(),nps);r.binding.exposure.extent={0,0,4,4};
         need(lab::translate_rr_frame(r,packet,command,auto_profile,out)==nullptr&&!out.exposure&&out.exposure_note==unsigned(ExposureNote::unsupported_shape),
              "A tag extent other than 1x1 is a note");}
    }

    std::cout<<"PASS "<<checks<<" frame translation checks; WARP descriptors, no queue, no NR\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
