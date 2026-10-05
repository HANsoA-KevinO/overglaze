// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_nr_session.hpp"
#include "lab_nr_mode.hpp"
#include "lab_depth_projection.hpp"
#include "lab_device_identity.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cmath>

namespace lab::nr {
// This first adapter consumes already prepared sample-domain inputs. It does
// NOT reinterpret a game's linear HDR target as a display-ready NR input.
enum class ColorDomain { unknown, prepared_sample_input, linear_working_rgb };
// Explicit layouts, not format guesses. The second profile matches the RR
// resource descriptions observed in 2077; units/color/lifetimes remain separate
// adapter obligations. It does not grant a game-control capability.
enum class GuideLayout { packed_float, r32_typeless_rgba16_float };
struct Frame {
    std::uint64_t token = 0; // Adapter-local monotonic token, not a Present id.
    unsigned viewport = 0, width = 0, height = 0;
    // Explicit guide extent; never infer it from output size or fabricate a
    // full-sized guide when the game supplies a lower-resolution one. Guides
    // retain their explicit rectangular extent (including rounded DLSS ratios).
    unsigned guide_width = 0, guide_height = 0;
    GuideLayout guide_layout = GuideLayout::packed_float;
    ID3D12GraphicsCommandList* commands = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* output = nullptr;
    // The adapter must establish these states and non-aliasing before entry.
    // GetDesc cannot query current resource states or overlapping allocations.
    bool states_established = false, distinct_allocations_established = false;
    ColorDomain color_domain = ColorDomain::unknown;
    float motion_scale_x = 1, motion_scale_y = 1;
    bool depth_inverted = false;
    bool history_reset_requested = false; // Public host request, not internal reset proof.
};
struct Selection {
    ID3D12Resource* resource = nullptr;
    bool nr_recorded = false;
    bool reset_requested = false; // Not proof of internal reset semantics.
    unsigned ngx_result = 0;
};

class FrameRunner final {
    Session& session_;
    NVSDK_NGX_Parameter& parameters_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    ThreadAccess access_;
    DeviceIdentity device_identity_;
    unsigned width_, height_;
    std::uint64_t last_token_ = 0, off_frames_ = 0, on_frames_ = 0;
    bool last_on_ = false, retained_pending_ = false;
    // A reference protects COM lifetime, NOT content. Adapter still owns state,
    // aliasing, producer/consumer ordering, and command-list state restoration.
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,4> retained_;
    void owner() const {
        access_.require();
    }
    static Microsoft::WRL::ComPtr<IUnknown> identity(IUnknown* value) {
        Microsoft::WRL::ComPtr<IUnknown> result;
        if(!value || FAILED(value->QueryInterface(IID_PPV_ARGS(&result))))throw std::logic_error("Missing resource COM identity");
        return result;
    }
    void validate(const Frame& f) const {
        if(!f.commands || f.width!=width_ || f.height!=height_ || f.viewport!=0 ||
           !f.states_established || !f.distinct_allocations_established ||
           f.color_domain!=ColorDomain::prepared_sample_input ||
           !std::isfinite(f.motion_scale_x) || !std::isfinite(f.motion_scale_y))
            throw std::logic_error("NR adapter input contract not established (extent, state, color or guides)");
        if(!guide_extent_supported(width_,height_,f.guide_width,f.guide_height))
            throw std::logic_error("Explicit nonzero guide extent must fit output extent");
        if(f.commands->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)throw std::logic_error("Only the verified DIRECT recording path is supported");
        Microsoft::WRL::ComPtr<ID3D12Device> actual;
        if(FAILED(f.commands->GetDevice(IID_PPV_ARGS(&actual))) || !device_identity_.same(actual.Get(),device_.Get()))
            throw std::logic_error("Command list belongs to another device");
        const std::array<ID3D12Resource*,4> resources{f.color,f.depth,f.motion,f.output};
        if(f.guide_layout!=GuideLayout::packed_float && f.guide_layout!=GuideLayout::r32_typeless_rgba16_float)
            throw std::logic_error("Unknown guide resource layout");
        const bool game_layout=f.guide_layout==GuideLayout::r32_typeless_rgba16_float;
        const std::array<DXGI_FORMAT,4> formats{DXGI_FORMAT_R16G16B16A16_FLOAT,
            game_layout?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_R32_FLOAT,
            game_layout?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
        // Each guide role is checked against the spellings NR can be handed; the
        // layout only says which one is expected by default. One two-valued enum
        // cannot express the combination a game actually tags -- RE Engine pairs
        // TYPELESS depth with TWO-channel motion (Resident Evil Requiem),
        // which is neither layout. Known design debt: the acceptable
        // set belongs in the frame itself, and will have to move there when a
        // fourth spelling appears rather than growing this enum combinatorially.
        const auto accepted=[&](unsigned i,DXGI_FORMAT actual){
            // R32G8X24_TYPELESS is deliberately absent: see nr_live_core.hpp.
            // Handing a two-plane depth/stencil resource through unconverted
            // crashed the display driver.
            if(i==1)return game_layout?(actual==DXGI_FORMAT_R32_TYPELESS)
                                      :(actual==DXGI_FORMAT_R32_FLOAT||actual==DXGI_FORMAT_R16_FLOAT);
            if(i==2)return actual==DXGI_FORMAT_R16G16_FLOAT||actual==DXGI_FORMAT_R16G16B16A16_FLOAT;
            return actual==formats[i];
        };
        std::array<Microsoft::WRL::ComPtr<IUnknown>,4> ids;
        for(unsigned i=0;i<4;++i){
            ids[i]=identity(resources[i]);
            for(unsigned j=0;j<i;++j)if(ids[j]==ids[i])throw std::logic_error("NR input/output aliases are refused");
            actual.Reset();
            if(FAILED(resources[i]->GetDevice(IID_PPV_ARGS(&actual))) || !device_identity_.same(actual.Get(),device_.Get()))
                throw std::logic_error("NR resource belongs to another device");
            const auto d=resources[i]->GetDesc();
            const bool guide=i==1 || i==2;
            if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=(guide?f.guide_width:width_) || d.Height!=(guide?f.guide_height:height_) ||
               d.DepthOrArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1 || d.SampleDesc.Quality!=0 || !accepted(i,d.Format) ||
               (i<3 && (d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) ||
               (i==3 && !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
                throw std::logic_error("Unsupported NR texture layout/format/extent/access");
        }
    }
public:
    FrameRunner(Session& session,NVSDK_NGX_Parameter& parameters,ID3D12Device* device,unsigned width,unsigned height,DeviceIdentity identity_policy={})
        :session_(session),parameters_(parameters),device_(device),access_(session.access_gate()),device_identity_(identity_policy),width_(width),height_(height) {
        if(!device || !width || !height || width>8192 || height>8192)throw std::invalid_argument("Invalid fixed NR frame extent/device");
    }
    ~FrameRunner() {
        // Do not release GPU-referenced resources on an unknown completion.
        // The host must terminate/isolate this context; no retry is safe here.
        if(retained_pending_)for(auto& resource:retained_)(void)resource.Detach();
    }
    FrameRunner(const FrameRunner&)=delete;
    FrameRunner& operator=(const FrameRunner&)=delete;
    // Pure preflight for a composed pipeline, before any snapshot/conversion.
    void validate_on_frame(const Frame& f) const {
        owner();
        if(!f.token || f.token<=last_token_)throw std::logic_error("Non-monotonic NR frame token");
        if(retained_pending_ || session_.pending() || session_.abandoned() || session_.operation_failed() || !session_.has_feature())
            throw std::logic_error("NR is unprepared, failed or still using the previous frame");
        validate(f);
    }
    Selection record(Mode mode,const Frame& f) {
        owner();
        if(mode!=Mode::off && mode!=Mode::on)throw std::logic_error("Unknown NR mode");
        if(!f.token || f.token<=last_token_)throw std::logic_error("Non-monotonic frame token");
        if(mode==Mode::off){
            // Strict bypass: no NGX, resource queries, transitions, copies,
            // conversions, allocations or parameter writes for this frame.
            last_token_=f.token;last_on_=false;++off_frames_;return {f.color,false,false,0};
        }
        validate_on_frame(f); // Refuse before mutating parameters or recording any work.
        const std::array<ID3D12Resource*,4> resources{f.color,f.depth,f.motion,f.output};
        constexpr const char* keys[]{"DLSSNR.Color","DLSSNR.Depth","DLSSNR.MVec","DLSSNR.Output"};
        for(unsigned i=0;i<4;++i){
            retained_[i]=resources[i];parameters_.Set(keys[i],resources[i]);
            const std::string prefix(keys[i]);
            parameters_.Set((prefix+"SubrectBaseX").c_str(),0u);parameters_.Set((prefix+"SubrectBaseY").c_str(),0u);
            const bool guide=i==1 || i==2;
            parameters_.Set((prefix+"SubrectWidth").c_str(),guide?f.guide_width:width_);parameters_.Set((prefix+"SubrectHeight").c_str(),guide?f.guide_height:height_);
        }
        parameters_.Set("DLSSNR.MVecScaleX",f.motion_scale_x);parameters_.Set("DLSSNR.MVecScaleY",f.motion_scale_y);
        parameters_.Set("DLSSNR.DepthInverted",f.depth_inverted?1:0);
        parameters_.Set("DLSSNR.Enabled",1);
        const bool reset=!last_on_ || f.history_reset_requested;
        parameters_.Set("DLSSNR.Reset",reset?1:0);
        last_token_=f.token;retained_pending_=true;
        const auto result=session_.evaluate(f.commands,&parameters_);
        if(result!=NVSDK_NGX_Result_Success)return {nullptr,true,reset,static_cast<unsigned>(result)};
        last_on_=true;++on_frames_;return {f.output,true,reset,static_cast<unsigned>(result)};
    }
    void acknowledge_outer_queue_completion() {
        owner();
        if(!retained_pending_)throw std::logic_error("No recorded NR frame pending");
        session_.acknowledge_outer_queue_completion();retained_pending_=false;
        for(auto& resource:retained_)resource.Reset();
    }
    // The list holding this Evaluate was Reset by its owner before any
    // submission (proven by the native observer): the GPU never saw it. Not an
    // acknowledgement. The Feature's history did not advance, so the next
    // Evaluate requests DLSSNR.Reset.
    void discard_recorded() {
        owner();
        if(!retained_pending_)throw std::logic_error("No recorded NR frame to discard");
        session_.discard_pending_recording();retained_pending_=false;last_on_=false;
        for(auto& resource:retained_)resource.Reset();
    }
    std::uint64_t off_frames() const {owner();return off_frames_;}
    std::uint64_t on_frames() const {owner();return on_frames_;}
};
}
