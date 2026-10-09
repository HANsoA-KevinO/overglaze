// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The game's own exposure for the auto-exposure path (Live ABI24).
//
// DLSS takes a game's exposure as a 1x1 texture E (NGX ExposureTexture,
// Streamline kBufferTypeExposure) plus Pre.Exposure and Exposure.Scale; the
// game-exposed colour is colour x E x scale / pre-exposure. With auto exposure on,
// Overglaze prefers that value to its own GPU meter and falls back to the meter
// when the game gives none or a value it cannot trust. Nothing here is ever a
// reason to skip a frame or to stop NR: an unusable exposure is a named note.
//
//   * screen_exposure: the per-frame rules both frame sources and the runtime
//     apply to the texture (format, shape, state, aliasing, pre/scale);
//   * GameExposureSelector: game or meter, from the readings retired with each
//     frame, with a plausibility window and hysteresis.
#include "lab_nr_live_api.hpp"
#include <d3d12.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace lab::live {
inline const char* exposure_note_name(unsigned note) noexcept {
    switch(static_cast<ExposureNote>(note)){
    case ExposureNote::none:return "none";
    case ExposureNote::no_texture:return "no-exposure-texture";
    case ExposureNote::dlss_auto_exposure:return "game-uses-dlss-auto-exposure";
    case ExposureNote::tag_not_fresh:return "exposure-tag-not-fresh";
    case ExposureNote::tag_only_valid_now:return "exposure-tag-only-valid-now";
    case ExposureNote::tag_other_thread:return "exposure-tag-other-thread";
    case ExposureNote::tag_invalid:return "exposure-tag-invalid";
    case ExposureNote::lease_unavailable:return "exposure-lease-unavailable";
    case ExposureNote::unsupported_format:return "unsupported-format";
    case ExposureNote::unsupported_shape:return "unsupported-shape";
    case ExposureNote::unsupported_state:return "unsupported-state";
    case ExposureNote::aliased:return "exposure-aliases-an-input";
    case ExposureNote::other_device:return "exposure-on-another-device";
    case ExposureNote::invalid_scale:return "invalid-pre-exposure-or-scale";
    case ExposureNote::invalid_value:return "invalid-exposure-value";
    case ExposureNote::implausible:return "implausible";
    case ExposureNote::unverified:return "no-meter-reading-to-verify";
    case ExposureNote::waiting:return "no-reading-yet";
    case ExposureNote::reader_unavailable:return "exposure-reader-unavailable";
    case ExposureNote::count:break;}
    return "unknown";
}
inline const char* exposure_source_name(unsigned source) noexcept {
    return source==static_cast<unsigned>(ExposureSource::game)?"game":source==static_cast<unsigned>(ExposureSource::meter)?"meter":"manual";
}
// Typed float formats whose R channel a typed SRV load returns as the value.
// NVIDIA's contract is a 1x1 R32_FLOAT or R16_FLOAT; the wider float formats
// cost nothing to read the same way. Typeless or integer formats are a guess.
inline bool exposure_format_supported(DXGI_FORMAT f) noexcept {
    return f==DXGI_FORMAT_R32_FLOAT||f==DXGI_FORMAT_R16_FLOAT||f==DXGI_FORMAT_R32G32_FLOAT||f==DXGI_FORMAT_R16G16_FLOAT||
           f==DXGI_FORMAT_R32G32B32A32_FLOAT||f==DXGI_FORMAT_R16G16B16A16_FLOAT;
}
// A declared state the reader can move to NON_PIXEL_SHADER_RESOURCE and back
// with one paired transition: COMMON, UNORDERED_ACCESS on its own, or shader /
// copy reads only.
inline bool exposure_state_supported(unsigned state) noexcept {
    constexpr unsigned reads=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_COPY_SOURCE;
    return state==D3D12_RESOURCE_STATE_COMMON||state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS||!(state&~reads);
}
inline ExposureNote inspect_exposure_texture(ID3D12Resource* r,unsigned state) noexcept {
    if(!r)return ExposureNote::no_texture;
    const auto d=r->GetDesc();
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Width!=1||d.Height!=1||d.DepthOrArraySize!=1||!d.MipLevels||
       d.SampleDesc.Count!=1||d.SampleDesc.Quality||(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return ExposureNote::unsupported_shape;
    if(!exposure_format_supported(d.Format))return ExposureNote::unsupported_format;
    if(!exposure_state_supported(state)||(state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS&&!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
        return ExposureNote::unsupported_state;
    return ExposureNote::none;
}
// Applies every per-frame rule to the frame's exposure: an unusable texture is
// dropped (null) with its note; pre/scale that are not finite and positive drop
// it too. Colour, depth and motion are not looked at beyond their identity.
// Only a texture whose note is none is usable; a frame source's note wins.
inline void screen_exposure(Frame& f) noexcept {
    auto note=f.exposure_note<exposure_note_count?static_cast<ExposureNote>(f.exposure_note):ExposureNote::tag_invalid;
    if(note==ExposureNote::none&&!f.exposure)note=ExposureNote::no_texture;
    if(note==ExposureNote::none&&(f.exposure==f.color||f.exposure==f.depth||f.exposure==f.motion))note=ExposureNote::aliased;
    if(note==ExposureNote::none)note=inspect_exposure_texture(f.exposure,f.exposure_state);
    if(note==ExposureNote::none&&!(std::isfinite(f.pre_exposure)&&f.pre_exposure>0.f&&std::isfinite(f.exposure_scale)&&f.exposure_scale>0.f))
        note=ExposureNote::invalid_scale;
    if(note!=ExposureNote::none){f.exposure=nullptr;f.exposure_state=0;}
    f.exposure_note=static_cast<unsigned>(note);
}
}
namespace lab::nr {
// Game or meter. Fed once per retired auto-exposure frame with what the GPU
// read back for it; decides which gain the NEXT frame uses (one frame of
// latency, like the meter).
//
// Plausibility: the game-exposed image's log-average -- the meter's mean log2
// luminance of the working RGB plus log2(E x scale / pre) -- must lie in
// 2^-7.19 .. 2^-0.42, the window the RenoDX DLSS5 add-on trusts a game's
// exposure in. A game value without a meter reading to check it is not trusted
// for a switch to the game, but does not end one either.
//
// Hysteresis: the first reading decides at once. After that the source moves
// only after switch_frames consecutive readings that say so, and a trusted
// game value is kept until it leaves the window by more than leave_margin_stops.
// While a trusted value is held through doubtful readings, the last good E is
// used. Turning auto exposure off or rebuilding resets it (reset()).
struct GameExposureSelector {
    static constexpr float min_exposed_log2=-7.19f,max_exposed_log2=-0.42f;
    static constexpr float leave_margin_stops=1.f;
    static constexpr unsigned switch_frames=8;
    bool decided=false,game=false;
    unsigned streak=0;
    live::ExposureNote note=live::ExposureNote::waiting; // the latest reason against the game's value
    float exposure=0,pre=1,scale=1;                       // last game reading trusted
    bool have_exposure=false;
    float exposed_log2=0;bool exposed_valid=false;        // the latest plausibility input
    std::uint64_t switches=0;
    bool using_game() const noexcept {return decided&&game&&have_exposure;}
    // log2 gain of the trusted game exposure under THIS frame's pre-exposure and
    // scale (the ones its colour carries); falls back to the reading frame's own
    // when the current ones are unusable.
    float stops(float pre_now,float scale_now) const noexcept {
        const bool ok=std::isfinite(pre_now)&&pre_now>0.f&&std::isfinite(scale_now)&&scale_now>0.f;
        return std::log2(exposure)+std::log2(ok?scale_now:scale)-std::log2(ok?pre_now:pre);
    }
    // One retired frame. value_read: the GPU read the frame's exposure texture;
    // why: the frame's note when it did not. e, frame_pre, frame_scale: that
    // frame's values. meter_valid / meter_log2: the meter's reading of the same frame.
    void observe(bool value_read,live::ExposureNote why,float e,float frame_pre,float frame_scale,bool meter_valid,float meter_log2) noexcept {
        using live::ExposureNote;
        auto verdict=ExposureNote::none;exposed_valid=false;
        if(!value_read)verdict=why==ExposureNote::none?ExposureNote::invalid_value:why;
        else if(!(std::isfinite(e)&&e>0.f))verdict=ExposureNote::invalid_value;
        else if(!(std::isfinite(frame_pre)&&frame_pre>0.f&&std::isfinite(frame_scale)&&frame_scale>0.f))verdict=ExposureNote::invalid_scale;
        else if(meter_valid&&std::isfinite(meter_log2)){
            exposed_log2=meter_log2+std::log2(e*frame_scale/frame_pre);exposed_valid=true;
            const float margin=game?leave_margin_stops:0.f;
            if(exposed_log2<min_exposed_log2-margin||exposed_log2>max_exposed_log2+margin)verdict=ExposureNote::implausible;
        }else if(!game)verdict=ExposureNote::unverified;
        if(verdict==ExposureNote::none){exposure=e;pre=frame_pre;scale=frame_scale;have_exposure=true;}
        if(!decided){decided=true;game=verdict==ExposureNote::none;streak=0;note=verdict;return;}
        if(game){
            if(verdict==ExposureNote::none){streak=0;note=verdict;return;}
            note=verdict;if(++streak>=switch_frames){game=false;streak=0;++switches;}
        }else{
            if(verdict!=ExposureNote::none){streak=0;note=verdict;return;}
            if(++streak>=switch_frames){game=true;streak=0;note=ExposureNote::none;++switches;}
        }
    }
    void reset() noexcept {*this=GameExposureSelector{};}
};
}
