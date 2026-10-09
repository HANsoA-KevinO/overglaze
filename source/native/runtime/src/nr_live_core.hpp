// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// NR runtime core of the live bridge, shared by both bridge variants. It owns
// the original-model session, the Evaluate pipeline, guide/depth preparation,
// exposure, submission enrollment, completion retirement and the mode/settings
// state machine. Everything research-only reaches it through LiveExtension, so
// the controller bridge links none of those objects (see nr_live_extension.hpp).
#include "lab_nr_parameters.hpp"
#include "lab_nr_live_api.hpp"
#include "nr_live_extension.hpp"
#include "lab_nr_pipeline.hpp"
#include "lab_nr_color_profile_report.hpp"
#include "lab_live_submission.hpp"
#include "lab_frame_retirement.hpp"
#include "lab_depth_codec.hpp"
#include "lab_motion_codec.hpp"
#include "lab_exposure_meter.hpp"
#include "lab_exposure_control.hpp"
#include "lab_game_exposure.hpp"
#include <wrl/client.h>
#include <memory>
#include <cstring>
#include <cstdio>
namespace lab::live {
using Microsoft::WRL::ComPtr;
using lab::nr::Parameters;using lab::nr::checked;
constexpr auto read_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto write_state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
inline void demand(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
inline void demand(bool ok,const std::string& error){if(!ok)throw std::runtime_error(error);}
struct Borrowed {
    // Colour, depth, motion and -- when it is read on this frame -- the game's
    // exposure texture (null otherwise), held until the GPU retires the frame.
    std::array<ComPtr<ID3D12Resource>,4> textures;ComPtr<ID3D12GraphicsCommandList> command;
    Borrowed()=default;explicit Borrowed(const Frame& f):textures{f.color,f.depth,f.motion,f.exposure},command(f.command){}
    bool held()const{return command!=nullptr;}
    void abandon(){for(auto& t:textures)(void)t.Detach();(void)command.Detach();}
};
// Process-pinned, one pending GPU frame. The worker prepares the Feature; game
// callbacks never load modules, compile shaders, create textures or write files.
struct Live {
    lab::SerialCallGate gate;std::atomic<State> state{State::waiting_frame};
    std::atomic<bool> stopping{false};
    std::atomic<const char*> asynchronous_failure{nullptr};
    // Only an entry admitted under the serial gate owns an exit obligation.
    // Preparation deliberately skips entries while a worker owns that gate.
    // Unmatched exits must not wait for the worker or poison the ready runtime.
    std::atomic<std::uint64_t> game_binding_call{0};
    std::atomic<DWORD> game_binding_thread{0};
    Parameters parameters{false};ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> init_queue;
    ComPtr<ID3D12CommandAllocator> init_allocator;ComPtr<ID3D12GraphicsCommandList> init_commands;
    ComPtr<ID3D12Fence> init_fence;lab::Handle event,sample_lock;
    // The unrecognized model the installation pinned (LabNrLivePinModelV1); empty:
    // reviewed versions only. Set once, before the model is first loaded.
    std::string pinned_model;
    static bool sha256_text(const char* s){
        if(!s)return false;unsigned n=0;
        for(;n<65&&s[n];++n)if(!((s[n]>='0'&&s[n]<='9')||(s[n]>='a'&&s[n]<='f')))return false;
        return n==64;}
    std::filesystem::path sample,data;
    lab::LiveSubmission* submission=nullptr;
    lab::InsertionBindings* bindings=nullptr;
    ULONGLONG binding_wait_started=0;
    lab::live::BindingStatus binding_status;
    const char* binding_reason=nullptr;
    // Research collectors (capture, display pair, binding/preparation probes,
    // boundary audit) live behind this seam; null in the controller bridge.
    std::unique_ptr<LiveExtension> extension;
    std::uint64_t recorded_call=0,recorded_frame=0;char retire_error[256]{};
    // Grace for a submission racing this RR return (the game's submit thread
    // may be inside ExecuteCommandLists) or a timeline try-lock miss. Longer
    // waits could stall the game's own submit/reset of that list.
    static constexpr std::uint64_t submission_grace_ms=2;
    // An armed research diagnostic or a busy display capture owns the frame;
    // NR ON stays refused until it finishes. Always false without an extension.
    bool diagnostics_block_on()noexcept{return extension&&extension->blocks_nr_on();}
    void update_diagnostics()noexcept{if(extension)extension->expire(*this);}
    std::unique_ptr<lab::nr::Session> session;
    HMODULE nr_module=nullptr;
    std::array<ComPtr<ID3D12Resource>,6> scratch;
    std::unique_ptr<lab::nr::PrePostFramePipeline> pipeline;
    std::unique_ptr<lab::nr::DepthCodec> depth_codec;
    // The depth guide of the configured frame is a two-plane depth/stencil
    // resource, converted by a depth_stencil_plane0 codec.
    bool depth_plane_extract=false;
    static bool two_plane_depth(ID3D12Resource* r){return r&&r->GetDesc().Format==DXGI_FORMAT_R32G8X24_TYPELESS;}
    // A hardware depth NR cannot be handed directly: two planes (Resident Evil
    // Requiem) or a mip chain (A Plague Tale: Resonance). 0 none, 1 plane 0,
    // 2 level 0. A change of kind rebuilds, like any other layout change.
    // A cropped frame (ABI23) whose depth resource is larger than the guide
    // region needs the level-0 copy too: NR is handed exactly the guide extent.
    static unsigned depth_copy_kind(ID3D12Resource* r,const Frame& f){
        if(!r)return 0;const auto d=r->GetDesc();if(d.Format==DXGI_FORMAT_R32G8X24_TYPELESS)return 1;if(d.MipLevels>1)return 2u;
        return f.region_crop&&(d.Width!=f.guide_width||d.Height!=f.guide_height)?2u:0u;}
    unsigned depth_copy=0;
    // ABI23. Motion NR cannot be handed as it is -- another resolution than the
    // guide grid (display-resolution motion) or a larger resource (a render
    // subrect) -- is resampled onto the guide grid by motion_codec from this
    // region. 0x0: NR takes the game's motion resource directly, as before.
    std::unique_ptr<lab::nr::MotionCodec> motion_codec;
    unsigned motion_region_w=0,motion_region_h=0;
    static unsigned motion_region_width(const Frame& f){return f.motion_width?f.motion_width:f.guide_width;}
    static unsigned motion_region_height(const Frame& f){return f.motion_height?f.motion_height:f.guide_height;}
    static bool needs_motion_codec(const Frame& f){
        if(!f.motion)return false;const auto d=f.motion->GetDesc();
        return motion_region_width(f)!=f.guide_width||motion_region_height(f)!=f.guide_height||d.Width!=f.guide_width||d.Height!=f.guide_height;}
    std::unique_ptr<lab::nr::ExposureMeter> meter;lab::nr::ExposureController auto_exposure;
    bool meter_snap=false;float applied_exposure_stops=0;
    // ABI24. Auto exposure prefers the game's own exposure: the reader copies its
    // texel on the GPU with the frame, the selector decides game or meter from
    // what retires. A reader that cannot be built only means "meter".
    std::unique_ptr<lab::nr::GameExposureReader> exposure_reader;
    lab::nr::GameExposureSelector exposure_selector;
    // The recorded auto-exposure frame whose readings are still in flight.
    struct ExposureFrame {bool pending=false;ExposureNote note=ExposureNote::waiting;float pre=1,scale=1;} exposure_frame;
    void observe_exposure(const lab::nr::ExposureReading& metered){
        if(!exposure_frame.pending)return;exposure_frame.pending=false;
        lab::nr::GameExposureReading game;
        if(exposure_reader&&exposure_reader->pending())game=exposure_reader->acknowledge_completion();
        const auto before=exposure_selector.switches;
        exposure_selector.observe(game.read,exposure_frame.note,game.value,exposure_frame.pre,exposure_frame.scale,metered.valid,metered.mean_log2_luminance);
        status.exposure_source_switches+=exposure_selector.switches-before;
        status.game_exposure_valid=game.read?1u:0u;
        if(game.read){status.game_exposure=game.value;status.game_pre_exposure=exposure_frame.pre;status.game_exposure_scale=exposure_frame.scale;}
        status.game_exposed_valid=exposure_selector.exposed_valid?1u:0u;
        if(exposure_selector.exposed_valid)status.game_exposed_log2_luminance=exposure_selector.exposed_log2;
    }
    void discard_exposure_read(){
        if(exposure_reader&&exposure_reader->pending())exposure_reader->discard_recording();
        exposure_frame.pending=false;
    }
    std::unique_ptr<lab::CompletionTimeline> timeline;
    std::unique_ptr<lab::nr::FrameRetirement<lab::nr::PrePostFramePipeline,Borrowed>> retirement;
    Status status;Frame config;lab::nr::PrePostColorConstants colors;
    // config deliberately drops the frame's resource pointers so it can outlive
    // them, so the colour FORMAT is kept here as a value instead.
    DXGI_FORMAT color_format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    std::uint64_t last_call=0,last_frame=0,boundary_call=0,boundary_revision=0;
    // Our own per-frame sequence. The pipeline, colour handoff and completion
    // timeline need a strictly increasing token; the game's frame index is the
    // game's counter and need not be one (a reload may restart it).
    std::uint64_t sequence=0;
    unsigned boundary_on=0;
    bool needs_reset=true,init_incomplete=false;
    lab::nr::SettingsReads boundary_settings;
    bool boundary_settings_reset=false;
    unsigned last_evaluated_style=0;
    std::uint64_t boundary_settings_revision=0;
    std::uint64_t drain_started=0;
    explicit Live(ID3D12CommandQueue* seed,HMODULE proxy,const wchar_t* dll,const wchar_t* path):sample(dll),data(path){
        demand(sample.is_absolute()&&data.is_absolute(),"Absolute sample and data paths required");
        checked(seed->GetDevice(IID_PPV_ARGS(&device)));
        submission=lab::LiveSubmission::install(seed,proxy);
        event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);demand(event.valid(),"Completion event failed");
        extension.reset(create_live_extension()); // null in the controller bridge
    }
    void fail(const char* message)noexcept{
        if(extension)extension->on_fail();
        if(state==State::failed&&status.error[0])return; // Keep the first fault.
        state=State::failed;status.state=State::failed;status.observed_on=0;
        strncpy_s(status.error,message,_TRUNCATE);if(submission)submission->abandon();
        if(status.request_revision>status.ack_revision){status.ack_revision=status.request_revision;
            status.ack_frame=last_frame;status.ack_success=status.ack_evaluated=status.ack_selected=0;}
        // Context and GPU dependencies stay alive. No retry of a possibly
        // partially recorded frame; fail closed without touching the game API result.
    }
    void validate(const Frame& f){
        demand(f.size==sizeof(Frame)&&f.abi==lab::live::version&&f.call&&f.frame&&f.command&&f.color&&f.depth&&f.motion,"Incomplete live frame");
        demand(lab::nr::guide_extent_supported(f.width,f.height,f.guide_width,f.guide_height),"Unsupported live extents");
        demand(f.depth_inverted<=1&&f.reset<=1&&std::isfinite(f.motion_x)&&std::isfinite(f.motion_y),"Invalid guide constants");
        demand(f.region_crop<=1&&(f.motion_width==0)==(f.motion_height==0)&&f.motion_width<=8192&&f.motion_height<=8192,"Invalid live frame regions");
        demand(f.packed_guides<=1&&(f.depth_encoding==lab::nr::DepthEncoding::hardware||f.depth_encoding==lab::nr::DepthEncoding::view_z_magnitude),"Unknown live guide layout/semantic");
        if(f.depth_encoding==lab::nr::DepthEncoding::view_z_magnitude){
            demand(f.packed_guides==1&&f.depth_projection.inverted==f.depth_inverted,"Linear depth requires typed guides and matching direction");f.depth_projection.validate();}
        auto native=submission->resolve(f.command);demand(native.Get()==f.command,"Native live command required");
        const unsigned states[]{f.color_state,f.depth_state,f.motion_state};
        const std::array<ID3D12Resource*,3> textures{f.color,f.depth,f.motion};
        for(unsigned i=0;i<3;++i){ComPtr<ID3D12Device> d;checked(textures[i]->GetDevice(IID_PPV_ARGS(&d)));demand(d.Get()==device.Get(),"Different resource device");
            // Colour and depth may carry a mip chain: the colour is met through
            // its level 0 only (color_handoff copy_top) and the depth is copied
            // from level 0 by the depth codec. Motion is handed to NR directly.
            const auto desc=textures[i]->GetDesc();
            // ABI23: with region_crop a resource may be larger than its region;
            // motion is measured against its own region, which may be another
            // resolution than the guide grid.
            const unsigned ew=i==0?f.width:i==1?f.guide_width:motion_region_width(f),eh=i==0?f.height:i==1?f.guide_height:motion_region_height(f);
            const bool extent_ok=f.region_crop?(desc.Width>=ew&&desc.Height>=eh):(desc.Width==ew&&desc.Height==eh);
            demand(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&desc.DepthOrArraySize==1&&
                (i<2?desc.MipLevels>=1:desc.MipLevels==1)&&desc.SampleDesc.Count==1&&extent_ok,
                std::string("Live descriptor mismatch: ")+(i==0?"color":i==1?"depth":"motion")+
                " "+std::to_string(desc.Width)+"x"+std::to_string(desc.Height)+
                " expected "+(f.region_crop?"at least ":"")+std::to_string(ew)+"x"+std::to_string(eh)+
                " dim="+std::to_string(unsigned(desc.Dimension))+" mips="+std::to_string(desc.MipLevels)+
                " array="+std::to_string(desc.DepthOrArraySize)+" samples="+std::to_string(desc.SampleDesc.Count));
            // The colour role accepts either the full float format or the packed
            // 11/11/10 float some games use for their RR output; the guides are
            // exact. A packed colour has no alpha and no sign bit, which is the
            // game's own decision about its buffer, not something we discard.
            const bool packed_color=i==0&&desc.Format==DXGI_FORMAT_R11G11B10_FLOAT;
            // Linear depth may be single or half precision; the depth codec reads
            // either through a typed view and its own output stays R32_FLOAT.
            const bool half_depth=i==1&&f.packed_guides&&desc.Format==DXGI_FORMAT_R16_FLOAT;
            // A depth/stencil resource (R32G8X24_TYPELESS) is accepted ONLY
            // because it is converted. Its plane 0 holds the same 32-bit depth as
            // R32_TYPELESS, but letting RE Engine's spelling through unconverted
            // (Resident Evil Requiem) made the GPU work of the first Evaluate
            // crash inside nvwgf2umx.dll: identical memory layout says nothing
            // about VIEWS and TRANSITIONS on a TWO-PLANE resource, which
            // everything downstream assumes is one. Passing it through is
            // therefore never done; it is CONVERTED to single-plane R32_FLOAT
            // the way linear depth already is, with plane-aware transitions. A
            // hardware-depth guide in this spelling makes the runtime build a
            // depth_stencil_plane0 codec, and pipeline_source hands NR that
            // codec's single-plane R32_FLOAT instead of the game's resource.
            // guide_transitions touches subresource 0 only, which for a
            // two-plane resource is exactly the depth plane.
            const bool depth_stencil=i==1&&!f.packed_guides&&f.depth_encoding==lab::nr::DepthEncoding::hardware&&
                desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS;
            // Motion vectors are two-dimensional. A game may tag them as the two-
            // channel float form or the four-channel one and NR reads X and Y
            // either way, so the spelling is NOT tied to the guide layout: RE
            // Engine pairs typeless depth with two-channel motion (Resident Evil
            // Requiem), a combination one packed_guides flag cannot
            // express. Both spellings are accepted for this role on either path.
            const bool motion_pair=i==2&&desc.Format==DXGI_FORMAT_R16G16_FLOAT;
            const bool motion_quad=i==2&&desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
            const auto format=i==0?(packed_color?DXGI_FORMAT_R11G11B10_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT):
                i==1?(f.packed_guides?(half_depth?DXGI_FORMAT_R16_FLOAT:DXGI_FORMAT_R32_FLOAT):(depth_stencil?DXGI_FORMAT_R32G8X24_TYPELESS:DXGI_FORMAT_R32_TYPELESS)):
                motion_pair?DXGI_FORMAT_R16G16_FLOAT:motion_quad?DXGI_FORMAT_R16G16B16A16_FLOAT:
                (f.packed_guides?DXGI_FORMAT_R16G16_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT);
            // Name the role and both formats: a bare refusal here can only be
            // answered by trying formats until one stops failing, which is
            // guessing at a game's contract rather than reading it.
            demand(desc.Format==format&&desc.SampleDesc.Quality==0&&!(desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE),
                std::string("Unverified live format/access: ")+(i==0?"color":i==1?"depth":"motion")+
                " format="+std::to_string(unsigned(desc.Format))+" expected="+std::to_string(unsigned(format))+
                " quality="+std::to_string(desc.SampleDesc.Quality)+" flags="+std::to_string(unsigned(desc.Flags))+
                " guides="+(f.packed_guides?"packed":"unpacked"));
            if(i){const unsigned allowed=read_state|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|(i==1?D3D12_RESOURCE_STATE_DEPTH_READ:0);
                // A guide may also arrive in UNORDERED_ACCESS: guide_transitions
                // moves each one from its declared state to the read state and
                // restores it afterwards, and UAV -> NON_PIXEL_SHADER_RESOURCE is
                // an ordinary transition. UNORDERED_ACCESS is exclusive, so it is
                // accepted only on its own, never combined with a read state.
                const bool readable=(states[i]&read_state)&&!(states[i]&~allowed);
                demand(states[i]==D3D12_RESOURCE_STATE_COMMON||states[i]==D3D12_RESOURCE_STATE_UNORDERED_ACCESS||readable,
                    std::string("Unverified guide state: ")+(i==1?"depth":"motion")+
                    " state="+std::to_string(states[i])+" allowed=COMMON|UNORDERED_ACCESS|"+std::to_string(allowed));}
            else demand((states[0]==write_state||states[0]==D3D12_RESOURCE_STATE_COMMON)&&(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),"RR output must be declared UAV or COMMON");
        }
        demand(f.color!=f.depth&&f.color!=f.motion&&f.depth!=f.motion,"Aliased live resources");
    }
    void prepare(){
        state=State::preparing;
        demand(std::filesystem::is_directory(data)&&std::filesystem::space(data).available>=30ULL*1024*1024*1024,"Existing data directory and disk reserve required");
        if(!session){
        sample_lock.value=CreateFileW(sample.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        // Any reviewed version (lab_model_versions.hpp) -- or exactly the
        // unrecognized model the installation pinned with the user's opt-in --
        // checked on disk while the file is held open, then again for the
        // module actually mapped.
        demand(sample_lock.valid(),"NR model missing or unreadable");
        const auto sample_sha=lab::sha256(sample);
        demand(lab::model::accepted(sample_sha,pinned_model),pinned_model.empty()?"NR model is not a reviewed version":
            "NR model is neither a reviewed version nor the one pinned at install");
        // The host must exclude the legacy renderer BEFORE this context starts.
        demand(!GetModuleHandleW(L"renodx-dlss5.addon64")&&!GetModuleHandleW(L"overglaze_preview.addon64")&&!GetModuleHandleW(L"dlsslab_preview.addon64"),"Conflicting legacy NR or preview loaded");
        auto nr=LoadLibraryExW(sample.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        demand(nr&&lab::module_identity(nr).value("sha256","")==sample_sha,"Actually loaded NR identity rejected");
        D3D12_COMMAND_QUEUE_DESC q{};checked(device->CreateCommandQueue(&q,IID_PPV_ARGS(&init_queue)));
        checked(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&init_allocator)));
        checked(device->CreateCommandList(0,q.Type,init_allocator.Get(),nullptr,IID_PPV_ARGS(&init_commands)));
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&init_fence)));
        session=std::make_unique<lab::nr::Session>(lab::nr::Api::from_verified_module(nr),&gate);
        demand(session->initialize(device.Get(),data.c_str(),&parameters)==NVSDK_NGX_Result_Success,"NR Init failed");
        nr_module=nr;
        }else{
            demand(!session->pending()&&!session->has_feature(),"Rebuild requires retired and released Feature");
            checked(init_allocator->Reset());checked(init_commands->Reset(init_allocator.Get(),nullptr));
        }
        for(auto name:{"Width","OutWidth","DLSSNR.Width"})parameters.Set(name,config.width);
        for(auto name:{"Height","OutHeight","DLSSNR.Height"})parameters.Set(name,config.height);
        parameters.Set("DLSSNR.ScalingRatio",1.f);parameters.Set("PerfQualityValue",static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA));
        parameters.Set("CreationNodeMask",1u);parameters.Set("VisibilityNodeMask",1u);parameters.Set("DLSSNR.Hint.Render.Preset",0u);
        init_incomplete=true;const auto result=session->create(init_commands.Get(),&parameters);checked(init_commands->Close());
        const auto create_fence=status.feature_generation+1;
        ID3D12CommandList* command=init_commands.Get();init_queue->ExecuteCommandLists(1,&command);checked(init_queue->Signal(init_fence.Get(),create_fence));
        checked(init_fence->SetEventOnCompletion(create_fence,event.value));demand(WaitForSingleObject(event.value,30000)==WAIT_OBJECT_0,"NR Create GPU timeout");
        checked(device->GetDeviceRemovedReason());session->acknowledge_outer_queue_completion();init_incomplete=false;
        demand(result==NVSDK_NGX_Result_Success,"NR Create failed");
        for(auto v:{std::pair{"DLSSNR.Intensity",1.f},std::pair{"DLSSNR.LocalToneStrength",1.f},std::pair{"DLSSNR.LocalStructureStrength",1.f},std::pair{"DLSSNR.SkinStructureStrength",-1.f}})parameters.Set(v.first,v.second);
        parameters.Set("DLSSNR.UseAutoMask",0);parameters.Set("DLSSNR.UICorrection",0);parameters.Set("DLSSNR.Style",0);
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=config.width;d.Height=config.height;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        // Roles 0 and 3 meet the game through CopyResource, which demands
        // identical formats, so they follow the game. The four internal images
        // keep the working format: the model still receives scratch[1], and the
        // HDR and SDR images keep full precision for the ratio composite.
        for(unsigned i=0;i<6;++i){d.Flags=i?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
            d.Format=(i==0||i==3)?color_format:DXGI_FORMAT_R16G16B16A16_FLOAT;
            checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,i?write_state:read_state,nullptr,IID_PPV_ARGS(&scratch[i])));}
        lab::nr::PrePostColorInputs input{scratch[0].Get(),scratch[1].Get(),scratch[2].Get(),scratch[3].Get(),scratch[4].Get(),scratch[5].Get(),true,false,true};
        pipeline=std::make_unique<lab::nr::PrePostFramePipeline>(*session,parameters,device.Get(),config.width,config.height,input,lab::DeviceIdentity{},config.region_crop!=0);
        if(config.depth_encoding==lab::nr::DepthEncoding::view_z_magnitude)
            depth_codec=std::make_unique<lab::nr::DepthCodec>(device.Get(),config.guide_width,config.guide_height,&gate,
                lab::nr::DepthCodecMode::view_z_to_hardware,config.region_crop!=0);
        else if(depth_plane_extract)
            depth_codec=std::make_unique<lab::nr::DepthCodec>(device.Get(),config.guide_width,config.guide_height,&gate,
                lab::nr::DepthCodecMode::depth_stencil_plane0,config.region_crop!=0);
        else if(depth_copy==2)
            depth_codec=std::make_unique<lab::nr::DepthCodec>(device.Get(),config.guide_width,config.guide_height,&gate,
                lab::nr::DepthCodecMode::hardware_mip0_copy,config.region_crop!=0);
        if(motion_region_w)motion_codec=std::make_unique<lab::nr::MotionCodec>(device.Get(),config.guide_width,config.guide_height,&gate);
        meter=std::make_unique<lab::nr::ExposureMeter>(device.Get(),config.width,config.height,&gate);auto_exposure.reset();meter_snap=false;
        exposure_reader.reset();exposure_selector.reset();exposure_frame={};
        try{exposure_reader=std::make_unique<lab::nr::GameExposureReader>(device.Get(),&gate);}catch(...){} // null: auto exposure stays on the meter
        // Named experimental colour constants for this extent. The research
        // collector records them in its manifest; the maths is the same in both
        // bridges and is not a claim about the game's own colour contract.
        colors=lab::nr::experimental_color_constants(config.width,config.height);
        auto queue=submission->observed_queue();demand(queue!=nullptr,"Actual RR submission queue unavailable");
        timeline=std::make_unique<lab::CompletionTimeline>(queue.Get());retirement=std::make_unique<lab::nr::FrameRetirement<lab::nr::PrePostFramePipeline,Borrowed>>(*pipeline,*timeline,&gate);
        status.width=config.width;status.height=config.height;status.guide_width=config.guide_width;status.guide_height=config.guide_height;
        status.region_crop=config.region_crop;status.motion_width=motion_region_w;status.motion_height=motion_region_h;
        status.feature_generation=create_fence;
        status.scratch_bytes=static_cast<std::uint64_t>(config.width)*config.height*8*6+
            (depth_codec?static_cast<std::uint64_t>(config.guide_width)*config.guide_height*4:0)+
            (motion_codec?static_cast<std::uint64_t>(config.guide_width)*config.guide_height*4:0)+lab::nr::ExposureMeter::footprint_bytes()+
            (exposure_reader?lab::nr::GameExposureReader::footprint_bytes():0);
        // Collectors are built last, from the finished Feature generation. A
        // collector that cannot be built never stops NR (it reports its own note).
        if(extension)extension->on_prepared(*this);
        state=State::ready;
    }
    static const char* timeline_state_name(lab::CompletionTimeline::State s){
        using S=lab::CompletionTimeline::State;
        switch(s){case S::invalid:return "invalid";case S::reserved:return "reserved";case S::recorded:return "recorded";case S::submitted:return "submitted";
            case S::complete:return "complete";case S::busy:return "busy";case S::failed:return "failed";case S::discarded:return "discarded";}
        return "unknown";
    }
    // Five timeline states must not share one sentence. Name the state and the
    // submission observer's first failure so the next fault is diagnosable.
    [[noreturn]] void fail_retirement(const char* what,lab::CompletionTimeline::Ticket ticket,lab::CompletionTimeline::State seen){
        const char* why=submission?submission->first_failure():nullptr;
        std::snprintf(retire_error,sizeof(retire_error),"%s: frame %llu (sequence %llu) timeline=%s submission=%s",what,
            static_cast<unsigned long long>(recorded_frame),static_cast<unsigned long long>(ticket.frame),timeline_state_name(seen),why?why:"no-fault");
        throw std::runtime_error(retire_error);
    }
    // The game Reset the list holding the previous recording before executing
    // it (proven by the native Reset hook, which handed the ticket to the
    // timeline as discarded). Nothing of that frame ran: drop every same-list
    // companion the same way, release the lease, reset history, keep the ON
    // gate. No completion is claimed. False: nothing discarded, or try-locks
    // were busy (retried on the next call).
    bool discard_abandoned_recording(){
        if(!retirement)return false;const auto ticket=retirement->ticket();
        if(!retirement->discard_unsubmitted())return false;
        if(depth_codec)depth_codec->discard_recording();
        if(motion_codec)motion_codec->discard_recording();
        if(meter&&meter->pending())meter->discard_recording();
        discard_exposure_read();
        // The extension's own readback copies were in the same discarded list.
        const bool extension_owned=extension&&extension->on_recording_discarded();
        bool released=false;for(unsigned spin=0;spin<64&&!(released=submission->release_discarded());++spin)SwitchToThread();
        demand(released,"Discarded recording lease release failed");
        if(!extension_owned)++status.discarded_recordings;
        needs_reset=true;status.pending_gpu=0;
        status.rejection={};status.rejection.call=recorded_call;status.rejection.frame=recorded_frame;
        status.rejection.disposition=lab::RejectedDisposition::recording_discarded_by_game_reset;
        strcpy_s(status.rejection.stage,"live-submission");strcpy_s(status.rejection.reason,"previous-recording-discarded-by-game-reset-before-submission");
        return true;
    }
    // A pending previous frame is still unsubmitted on the game's own submit
    // thread, proven discarded, or in flight on the GPU. Only the last may
    // block this thread. The first is reported (false) after a short grace so
    // the caller skips this frame with the ON gate kept: the game may Reset or
    // submit that list only several frames later and must not be blocked.
    bool retire(bool wait){
        if(!retirement||retirement->ready())return true;
        if(!retirement->poll()){
            if(discard_abandoned_recording())return true;
            using S=lab::CompletionTimeline::State;const auto ticket=retirement->ticket();auto seen=timeline->inspect(ticket);
            if(!wait){if(seen==S::failed||seen==S::invalid)fail_retirement("Live retirement context failed",ticket,seen);return false;}
            const auto grace=GetTickCount64();
            while((seen==S::recorded||seen==S::busy)&&GetTickCount64()-grace<submission_grace_ms){SwitchToThread();seen=timeline->inspect(ticket);}
            if(seen==S::discarded&&discard_abandoned_recording())return true;
            if(seen==S::recorded||seen==S::busy||seen==S::discarded)return false;
            if(seen!=S::submitted&&seen!=S::complete)fail_retirement("Previous injected list cannot complete",ticket,seen);
            const auto start=GetTickCount64();checked(timeline->binding_fence()->SetEventOnCompletion(ticket.value,event.value));
            ++status.waits;const auto result=WaitForSingleObject(event.value,2000);status.wait_ms+=GetTickCount64()-start;
            demand(result==WAIT_OBJECT_0,"Live NR GPU timeout; context retained");
            demand(retirement->poll(),"Live NR retirement failed after fence");
        }
        if(retirement->ready()){if(depth_codec)depth_codec->acknowledge_completion();if(motion_codec)motion_codec->acknowledge_completion();
            lab::nr::ExposureReading reading;
            if(meter&&meter->pending()){reading=meter->acknowledge_completion();
                if(reading.valid){auto_exposure.update(reading.mean_log2_luminance,meter_snap);meter_snap=false;
                    status.metered_log2_luminance=reading.mean_log2_luminance;status.meter_samples=reading.samples;}}
            observe_exposure(reading); // the same frame's game exposure, checked against that meter reading
            demand(submission->release_completed(),"Submission lease retirement failed");
            if(!(extension&&extension->on_retired()))++status.retired;
            status.pending_gpu=0;return true;}
        demand(!retirement->failed(),"Live retirement context failed");return false;
    }
    void finish(){
        if(extension)extension->on_finish(*this);
        demand(!init_incomplete,"Initialization completion still unknown");
        if(session&&session->pending())throw std::runtime_error("Pending NGX work cannot be released");
        retirement.reset();pipeline.reset();timeline.reset();depth_codec.reset();motion_codec.reset();meter.reset();exposure_reader.reset();
        if(session&&session->has_feature()){demand(session->release()==NVSDK_NGX_Result_Success,"Live Release failed");++status.feature_releases;}
        if(session&&session->initialized())demand(session->shutdown()==NVSDK_NGX_Result_Success,"Live Shutdown failed");
        for(auto& t:scratch)t.Reset();submission->stop();state=State::stopped;
    }
    void begin_rebuild(const Frame& f,const char* reason){
        if(extension)extension->on_rebuild_begin();
        // No NR/convert/copy is recorded on the changed-size frame. Retain all
        // old dependencies until both its matched host return and GPU retire.
        ++status.rebuild_serial;status.rebuild_frame=f.frame;status.requested_on=0;
        strncpy_s(status.rebuild_reason,reason,_TRUNCATE);
        status.settings_read_mask=status.settings_style_read=status.settings_reset_requested=status.settings_skin_read=status.settings_automask_read=0;status.settings_observed_revision=status.settings_frame=0;
        needs_reset=true;drain_started=GetTickCount64();
        state=State::draining;
    }
    void drain_for_rebuild(){
        if(extension&&!extension->rebuild_drained())return;
        if(boundary_call||!retire(false)){
            demand(GetTickCount64()-drain_started<3000,"Rebuild completion unavailable; dependencies retained");return;
        }
        state=State::preparing; // Callbacks bypass while worker releases/joins.
        demand(!init_incomplete&&!session->pending(),"Rebuild cannot release unknown GPU work");
        if(extension)extension->on_rebuild_release(*this);
        retirement.reset();pipeline.reset();timeline.reset();depth_codec.reset();motion_codec.reset();meter.reset();exposure_reader.reset();
        demand(session->release()==NVSDK_NGX_Result_Success,"Rebuild Release failed; no retry");++status.feature_releases;
        // A fresh Create must not see old Evaluate resource pointers/subrects
        // after their allocations are released. User settings live in Status
        // and are rebound only after the new Feature is ready.
        parameters.Reset();
        for(auto& t:scratch)t.Reset();status.scratch_bytes=0;
        demand(submission->forget_queue_for_rebuild(),"Old submission still referenced during rebuild");
        state=State::waiting_rebuild_frame;
    }
    lab::nr::Frame pipeline_source(const Frame& f){
        lab::nr::Frame source;source.token=sequence;source.commands=f.command;source.color=f.color;source.depth=f.depth;source.motion=f.motion;
        source.width=f.width;source.height=f.height;source.guide_width=f.guide_width;source.guide_height=f.guide_height;
        // A converted depth is our own typed R32_FLOAT whatever the game tagged,
        // so the frame NR sees uses the typed layout whenever a codec produced it.
        source.guide_layout=(f.packed_guides||depth_codec)?lab::nr::GuideLayout::packed_float:lab::nr::GuideLayout::r32_typeless_rgba16_float;
        source.motion_scale_x=f.motion_x;source.motion_scale_y=f.motion_y;
        if(depth_codec){depth_codec->validate(f.command,f.depth,f.depth_projection);source.depth=depth_codec->output();}
        // Resampled motion keeps the game's values, which are in pixels of the
        // motion region; NR's scale is taken onto the guide grid instead.
        if(motion_codec){motion_codec->validate(f.command,f.motion,motion_region_w,motion_region_h);source.motion=motion_codec->output();
            source.motion_scale_x=f.motion_x*static_cast<float>(f.guide_width)/static_cast<float>(motion_region_w);
            source.motion_scale_y=f.motion_y*static_cast<float>(f.guide_height)/static_cast<float>(motion_region_h);}
        source.depth_inverted=f.depth_inverted!=0;
        source.color_domain=lab::nr::ColorDomain::linear_working_rgb;
        source.states_established=source.distinct_allocations_established=true;return source;
    }
    void guide_transitions(const Frame& f,bool restore){
        std::array<D3D12_RESOURCE_BARRIER,2> barriers{};unsigned count=0;
        for(unsigned i=0;i<2;++i){const auto before=static_cast<D3D12_RESOURCE_STATES>(i?f.motion_state:f.depth_state);
            if(before!=read_state){auto& b=barriers[count++];b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition={i?f.motion:f.depth,0,restore?read_state:before,restore?before:read_state};}}
        if(count)f.command->ResourceBarrier(count,barriers.data());
    }
    void enter_owned(Frame f){
        if(stopping||state==State::failed||state==State::stopped)return;
        auto native=submission->resolve(f.command);demand(native!=nullptr,"Live command unwrap failed");f.command=native.Get();
        validate(f);demand(f.call>last_call,"Repeated live RR call");
        // The frame index belongs to the game. Loading a save or returning to a
        // menu may restart it, and a game may Evaluate twice in one frame.
        // Neither may end NR. A LOWER index is a history break (the
        // interrupted rule below resets it); a REPEATED one is skipped, so NR
        // never runs twice on one game frame. Neither is a fault of ours.
        const bool repeated_frame=last_frame&&f.frame==last_frame;
        ++sequence;
        static constexpr const char* untracked_label="untracked-command-list";
        if(bindings){const bool tracked=bindings->watch(f.command);
            const char* why=!tracked?untracked_label:(state==State::ready?bindings->game_call_blocker(f.call,f.frame,f.command):bindings->blocker(f.command));
            status.binding_preservation_ready=why?0:1;
            binding_status.enabled=1;binding_status.ready=why?0:1;binding_status.frame=f.frame;binding_status.call=f.call;
            binding_status.consecutive_ready=why?0:binding_status.consecutive_ready+1;
            if(why)++binding_status.blocked_frames;
            if(why!=binding_reason||!binding_status.reason_first_frame){
                binding_status.reason_first_frame=f.frame;
                if(why==untracked_label)strncpy_s(binding_status.reason,bindings->watch_error().c_str(),_TRUNCATE);
                else if(why)strncpy_s(binding_status.reason,why,_TRUNCATE);
                else binding_status.reason[0]=0;
                binding_reason=why;
            }}
        if(extension)extension->rr_admitted(f.call);
        // A colour FORMAT change matters as much as an extent change: the two
        // game-facing scratch images were allocated to match, and CopyResource
        // would refuse a mismatch. Reading the descriptor is cheap and happens
        // only while ready, on a frame whose resources were already verified.
        const bool color_format_changed=state==State::ready&&f.color&&f.color->GetDesc().Format!=color_format;
        // Likewise for the depth plane layout: the codec was built for one of them.
        const bool depth_layout_changed=state==State::ready&&(two_plane_depth(f.depth)!=depth_plane_extract||depth_copy_kind(f.depth,f)!=depth_copy||
            needs_motion_codec(f)!=(motion_region_w!=0)||(motion_region_w&&(motion_region_width(f)!=motion_region_w||motion_region_height(f)!=motion_region_h)));
        const bool resized=state==State::ready&&(f.width!=config.width||f.height!=config.height||f.guide_width!=config.guide_width||f.guide_height!=config.guide_height||
            f.packed_guides!=config.packed_guides||f.depth_encoding!=config.depth_encoding||f.region_crop!=config.region_crop||
            f.motion_width!=config.motion_width||f.motion_height!=config.motion_height||color_format_changed||depth_layout_changed);
        // A complete future RR frame may follow a period without RR (for
        // example loading). Never bridge that history gap with an ON Evaluate.
        // Its cause is unknown: suspend/rebuild, do not label it a scene reset.
        const bool interrupted=state==State::ready&&status.requested_on&&last_frame&&f.frame!=last_frame+1;
        last_call=f.call;last_frame=f.frame;++status.frames;
        if(state==State::waiting_frame||state==State::waiting_rebuild_frame){
            color_format=f.color?f.color->GetDesc().Format:DXGI_FORMAT_R16G16B16A16_FLOAT;
            depth_plane_extract=two_plane_depth(f.depth);depth_copy=depth_copy_kind(f.depth,f);
            motion_region_w=needs_motion_codec(f)?motion_region_width(f):0;motion_region_h=motion_region_w?motion_region_height(f):0;
            config=f;config.command=nullptr;config.color=config.depth=config.motion=config.exposure=nullptr;
            if(status.rebuild_serial){boundary_call=f.call;boundary_on=false;boundary_revision=0;++status.off_frames;}
            demand(submission->enroll(f.command,true),"Live queue probe enrollment failed");state=State::probing_queue;return;}
        if(state==State::draining||state==State::probing_queue){
            if(status.rebuild_serial){demand(!boundary_call,"Missing bypass host return during rebuild");boundary_call=f.call;boundary_on=false;boundary_revision=0;++status.off_frames;}return;}
        if(state!=State::ready)return;
        demand(!boundary_call,"Previous matched host boundary acknowledgement missing");
        if(resized){begin_rebuild(f,"resource-extents-changed");boundary_call=f.call;boundary_on=false;boundary_revision=0;++status.off_frames;return;}
        // A gap in RR frames (menu, loading, skipped calls) is never bridged
        // with stale history: the next admitted frame requests DLSSNR.Reset.
        // Resources are re-validated per frame, so the Feature is kept and the
        // user's ON gate stays; only an extent change rebuilds.
        if(interrupted){needs_reset=true;++status.history_gaps;}
        if(!status.requested_on){binding_wait_started=0;++status.off_frames;needs_reset=true;boundary_call=f.call;boundary_on=false;status.consecutive_skips=0;
            // Binding/preparation probes and the display pair capture are research
            // collectors. They only ever run on an OFF frame and only exist in the
            // research bridge; the controller bridge has no extension at all.
            if(extension)extension->off_frame(*this,f);
            boundary_revision=status.request_revision;return;}
        if(repeated_frame){skip_admitted_frame(f,"repeated-game-frame-index");return;}
        if(!retire(true)){
            // The previous recording is still unsubmitted on the game's side (or
            // a proven discard met a busy lock). Recording another NR frame over
            // a live slot is never allowed, and blocking this thread until the
            // game submits or Resets that list could stall the game itself.
            // Skip this frame with a history reset; the ON gate is kept and the
            // panel shows the streak and reason. Never a terminal fault.
            ++status.submission_skips;skip_admitted_frame(f,"previous-recording-not-yet-submitted");return;
        }
        if(bindings&&!status.binding_preservation_ready){
            // The adapter admitted this call but our state tracker cannot vouch
            // for its command list (loading bursts exceed the slot count, or
            // the recording history is unknown). Nothing is recorded: the frame
            // is skipped with a history reset and the ON gate is kept. The
            // panel shows the streak and reason; it is never a terminal fault.
            ++status.binding_wait_frames;skip_admitted_frame(f,binding_status.reason);return;
        }
        binding_wait_started=0;
        // Only mutate the parameter object after the previous GPU use retired.
        parameters.Set("DLSSNR.LocalToneStrength",status.requested_settings.tone);
        parameters.Set("DLSSNR.LocalStructureStrength",status.requested_settings.structure);
        parameters.Set("DLSSNR.Style",status.requested_settings.style);
        parameters.Set("DLSSNR.SkinStructureStrength",status.requested_settings.skin);
        parameters.Set("DLSSNR.UseAutoMask",static_cast<int>(status.requested_settings.automask));
        // Host colour preparation exposure: manual absolute stops; or, with auto
        // on, the game's own exposure (log2 of E x scale / pre-exposure) when it
        // has given a plausible one, else the metered gain -- the user offset on
        // top either way. Applied to this frame's prepare/composite constants
        // only; never written to the DLL. The game's texture is read on this
        // frame only with auto on; manual exposure is exactly as before.
        const bool auto_exposure_on=status.requested_settings.exposure_auto!=0;
        auto frame_exposure_note=ExposureNote::none;
        if(auto_exposure_on){
            screen_exposure(f);
            if(f.exposure){ComPtr<ID3D12Device> owner;
                if(FAILED(f.exposure->GetDevice(IID_PPV_ARGS(&owner)))||owner.Get()!=device.Get()){f.exposure=nullptr;f.exposure_note=static_cast<unsigned>(ExposureNote::other_device);}}
            if(f.exposure&&(!exposure_reader||exposure_reader->pending())){f.exposure=nullptr;f.exposure_note=static_cast<unsigned>(ExposureNote::reader_unavailable);}
            frame_exposure_note=static_cast<ExposureNote>(f.exposure_note);
            const bool game=exposure_selector.using_game();
            const float base=game?exposure_selector.stops(f.pre_exposure,f.exposure_scale):auto_exposure.gain_stops();
            applied_exposure_stops=lab::nr::ExposureController::clamp_applied(base+status.requested_settings.exposure_stops);
            // On the meter, say why: this frame's own reason when it has one
            // (007 passes no texture at all), else the selector's.
            const auto why=game?ExposureNote::none:frame_exposure_note!=ExposureNote::none?frame_exposure_note:exposure_selector.note;
            status.exposure_source=static_cast<unsigned>(game?ExposureSource::game:ExposureSource::meter);
            status.exposure_note=static_cast<unsigned>(why);
            if(game)++status.game_exposure_frames;
            else{++status.meter_exposure_frames;if(static_cast<unsigned>(why)<exposure_note_count)++status.exposure_notes[static_cast<unsigned>(why)];}
        }else{
            f.exposure=nullptr;
            applied_exposure_stops=status.requested_settings.exposure_stops;auto_exposure.reset();exposure_selector.reset();
            status.exposure_source=static_cast<unsigned>(ExposureSource::manual);status.exposure_note=0;
        }
        colors.exposure=std::exp2(applied_exposure_stops);
        pipeline->set_compare_split(status.requested_settings.compare_split!=0);
        const bool style_changed=last_evaluated_style!=status.requested_settings.style;
        auto source=pipeline_source(f);source.history_reset_requested=needs_reset||style_changed||f.reset!=0;
        if(source.history_reset_requested)meter_snap=true; // the reading retired with this frame snaps the controller
        boundary_settings_reset=source.history_reset_requested;
        const auto target_state=static_cast<D3D12_RESOURCE_STATES>(f.color_state);
        pipeline->validate_on_frame(source,target_state,true,colors);Borrowed held(f);
        demand(submission->enroll(f.command,false),"Live render enrollment failed");
        lab::nr::Selection selected;
        auto record=[&]{selected=retirement->record_owned(sequence,std::move(held),[&]{
            guide_transitions(f,false);
            // The game's exposure texel, read for the NEXT frame's decision.
            if(f.exposure)exposure_reader->record(f.command,f.exposure,f.exposure_state);
            if(depth_codec)depth_codec->record(f.command,f.depth,f.depth_projection);
            if(motion_codec)motion_codec->record(f.command,f.motion,motion_region_w,motion_region_h);
            parameters.begin_settings_reads();
            struct CaptureCall {Live* live;const Frame* frame;bool active=false;} capture_call{this,&f};
            lab::nr::PipelineCaptureHooks hooks{&capture_call,
                [](void* opaque,const lab::nr::Frame& nr){auto& c=*static_cast<CaptureCall*>(opaque);auto& s=*c.live;
                    if(s.extension)c.active=s.extension->begin_inputs(s,nr);
                    if(c.active)s.parameters.begin_scene_reads();},
                [](void* opaque,const lab::nr::Frame& nr){auto& c=*static_cast<CaptureCall*>(opaque);
                    if(c.active&&c.live->extension)c.live->extension->record_output(*c.live,nr);},
                [](void* opaque,ID3D12GraphicsCommandList* cmd,ID3D12Resource* original){auto& c=*static_cast<CaptureCall*>(opaque);
                    if(c.live->meter)c.live->meter->record(cmd,original);}};
            auto result=pipeline->record(static_cast<lab::nr::Mode>(status.requested_on),source,target_state,true,colors,hooks);
            std::string captured_getters;if(capture_call.active)captured_getters=parameters.end_scene_reads().dump();
            boundary_settings=parameters.end_settings_reads();boundary_settings_revision=status.settings_requested_revision;
            // Observed settings mirror the request (the DLL never reads exposure);
            // the gain actually applied is reported separately and declared in
            // the capture manifest so offline audits use the real value.
            boundary_settings.values.exposure_stops=status.requested_settings.exposure_stops;
            boundary_settings.values.exposure_auto=status.requested_settings.exposure_auto;status.applied_exposure_stops=applied_exposure_stops;
            boundary_settings.values.compare_split=status.requested_settings.compare_split;
            // ABI22 optional controls: an unread Skin/UseAutoMask mirrors the request
            // (like exposure) and is flagged unread; a read value is kept and matched.
            if(!boundary_settings.skin_observed)boundary_settings.values.skin=status.requested_settings.skin;
            if(!boundary_settings.automask_observed)boundary_settings.values.automask=status.requested_settings.automask;
            auto recorded_settings=boundary_settings.values;recorded_settings.exposure_stops=applied_exposure_stops;
            demand(boundary_settings.matches(status.requested_settings),"NR did not read the requested Tone/Structure/Style (or read a different Skin/AutoMask) values");
            // What this bridge knows about the frame it just recorded. The research
            // collector merges it with the SL-side context the host published; a
            // split composite is a diagnostic view whose writeback is not the
            // wrapper contract the offline audit predicts, so it records nothing.
            const RecordedFrameFacts facts{f.frame,f.call,f.command,status.requested_on,source.history_reset_requested?1u:0u,
                (needs_reset||style_changed)?1u:0u,boundary_settings.style_observed?1u:0u,boundary_settings.mask,boundary_settings_revision};
            if(result.resource&&extension)extension->record_result(*this,facts,recorded_settings,
                status.requested_settings.compare_split!=0,std::move(captured_getters));
            if(result.resource)guide_transitions(f,true);
            return result;
        });
        }; // Entire conversion/NR/copy-back interval preserves native bindings.
        if(bindings){bindings->preserving_game_call(f.call,f.frame,f.command,record);++status.binding_restores;}else record();
        demand(submission->arm(*timeline,retirement->ticket()),"Live completion arm failed");
        demand(selected.nr_recorded&&selected.resource==f.color,"NR did not select in-place game output");
        recorded_call=f.call;recorded_frame=f.frame;
        // What retires with this frame for the exposure decision: the game's
        // texel when it was read, otherwise this frame's reason.
        exposure_frame={auto_exposure_on,frame_exposure_note,f.pre_exposure,f.exposure_scale};
        needs_reset=false;last_evaluated_style=status.requested_settings.style;++status.evaluates;status.pending_gpu=1;boundary_call=f.call;boundary_on=status.requested_on;boundary_revision=status.request_revision;
        status.consecutive_skips=0; // An admitted, recorded frame ends the transient-miss streak.
    }
    // Admitted by the adapter, refused by our own checks before any recording.
    void skip_admitted_frame(const Frame& f,const char* reason){
        needs_reset=true;++status.skipped_frames;++status.consecutive_skips;status.bypass_frame=f.frame;
        status.rejection={};status.rejection.call=f.call;status.rejection.frame=f.frame;status.rejection.disposition=lab::RejectedDisposition::skipped_before_insertion;
        strcpy_s(status.rejection.stage,"live-bindings");strncpy_s(status.rejection.reason,reason&&reason[0]?reason:"binding-not-ready",_TRUNCATE);
        boundary_call=f.call;boundary_on=false;boundary_revision=0;
    }
    void boundary_returned_owned(std::uint64_t call,unsigned ok){
        if(extension)extension->boundary_returned(call,ok==1&&state==State::ready);
        if(state==State::failed||state==State::stopped||call!=boundary_call||!boundary_call)return;
        demand(ok==1,"Matched host call failed after insertion");status.observed_on=boundary_on;
        if(!boundary_on)status.bypass_frame=last_frame;
        if(boundary_on&&(!status.settings_read_mask||status.settings_observed_revision!=boundary_settings_revision)){status.observed_settings=boundary_settings.values;status.settings_read_mask=boundary_settings.mask;
            status.settings_style_read=boundary_settings.style_observed;status.settings_reset_requested=boundary_settings_reset;
            status.settings_skin_read=boundary_settings.skin_observed;status.settings_automask_read=boundary_settings.automask_observed;
            status.settings_observed_revision=boundary_settings_revision;status.settings_frame=last_frame;}
        if(boundary_revision>status.ack_revision){status.ack_revision=boundary_revision;status.ack_frame=last_frame;status.ack_success=1;
            status.ack_evaluated=boundary_on!=0;status.ack_selected=boundary_on==1;}
        boundary_call=boundary_revision=0;
    }
    // Do not drop an ON frame because a worker briefly copied status. The
    // explicit bounded CPU admission wait is separate from GPU completion.
    template<class Fn> void callback(Fn&& fn)noexcept{
        const auto start=GetTickCount64();
        for(;;){
            {auto lease=gate.try_enter();if(lease){
                if(asynchronous_failure.load()){fail(asynchronous_failure.load());return;}
                try{fn();}catch(const std::exception& e){fail(e.what());}catch(...){fail("Foreign live callback exception");}return;
            }}
            if(state==State::preparing||stopping)return;
            if(GetTickCount64()-start>=10){asynchronous_failure="Live CPU callback admission timeout";state=State::failed;return;}
            SwitchToThread();
        }
    }
};
}
