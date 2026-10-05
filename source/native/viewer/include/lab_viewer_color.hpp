// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cmath>
#include <algorithm>
#include "lab_aces_view.hpp"
#include "lab_preconvert_reference.hpp"
#include <optional>
namespace lab::viewer {
struct Display {
    unsigned stage=0,hdr=0,curve=0,nr_linear=0;
    float exposure=0,white_nits=203,difference_gain=8,peak_nits=1000;
    // 0: viewing transform, 1: domain display, 2: legacy, 3: ACES-fitted SDR.
    unsigned mapping=0;float contrast=1,ui_white_nits=80;unsigned nr_reconstruct=0;
};
static_assert(sizeof(Display)==48);
// Keep the user's viewing recipe intact while bypassing it. The returned
// request is shared by both panes and export; it never reaches the NR runner.
inline Display viewing_request(Display saved,bool enabled){
    if(!enabled){saved.mapping=1;saved.exposure=0;saved.contrast=1;saved.nr_reconstruct=0;saved.nr_linear=0;}
    return saved;
}
inline void viewing_preset(Display& saved,unsigned preset){
    if(preset>1)throw std::runtime_error("Unknown viewing preset");
    saved.mapping=preset==0?0u:3u;saved.exposure=0;saved.contrast=preset==0?1.15f:1.f;saved.nr_linear=0;
    // This is an artistic viewing default, not a fitted game LUT. Do not
    // implicitly enable RR-assisted reconstruction or infer source exposure.
}
inline double srgb_decode(double x){return x<=.04045?x/12.92:std::pow((x+.055)/1.055,2.4);}
inline double srgb_encode(double x){x=std::max(0.,x);return x<=.0031308?12.92*x:1.055*std::pow(x,1./2.4)-.055;}
inline double filmic(double x){constexpr double a=.15,b=.5,c=.1,d=.2,e=.02,f=.3;return ((x*(a*x+c*b)+d*e)/(x*(a*x+b)+d*f))-e/f;}
inline double shoulder_nits(double y,double white,double peak){const auto start=std::min(white,peak*.75);if(y<=start)return y;return start+(peak-start)*(1-std::exp(-(y-start)/(peak-start)));}
// Display-only toe / linear middle / exponential shoulder. Peak is relative
// to the assumed image white, not a measurement of the source's light units.
inline double viewing_curve(double x,double peak){
    constexpr double middle=.22,toe=1.33,length=.4;
    const double join=middle+(peak-middle)*length;
    if(x>=join)return peak-(peak-join)*std::exp(-(x-join)/(peak-join));
    const double t=std::clamp(x/middle,0.,1.),w=t*t*(3-2*t);
    return (1-w)*middle*std::pow(x/middle,toe)+w*x;
}
// Explicit display-only branch for the recorded operator3 / identity / E=1
// adapter. RR is a required reference, never silently replaced by a screenshot.
// Preserve the original wrapper's FP16 scratch and whole-pixel invalid guard.
inline std::array<float,4> reconstruct_for_view(std::array<float,4> raw,std::array<float,4> rr){
    for(unsigned c=0;c<3;++c)if(!std::isfinite(raw[c])||raw[c]<0||raw[c]>65504||!std::isfinite(rr[c])||rr[c]<0||rr[c]>65504)return rr;
    const float lo=std::min({rr[0],rr[1],rr[2]}),hi=std::max({rr[0],rr[1],rr[2]});
    const float a=lo/(1+lo),b=hi/(1+hi),f=(b-a)*(hi==lo?1.f:1.f/(hi-lo));
    std::array<float,4> result{0,0,0,rr[3]};
    for(unsigned c=0;c<3;++c){const float s=preconvert::unhalf(preconvert::half(a+f*(rr[c]-lo)));
        const float n=float(srgb_decode(raw[c]));const float v=rr[c]*std::clamp((n+1e-6f)/(s+1e-6f),.01f,10.f);
        if(!std::isfinite(v)||v<0||v>65504)return rr;result[c]=preconvert::unhalf(preconvert::half(v));}
    return result;
}
// Display-only interpretation; never changes raw FP16 and never establishes
// the engine's actual chromaticities, exposure or nits-per-unit.
inline std::array<float,4> display_pixel(std::array<float,4> raw,const Display& d,std::optional<std::array<float,4>> rr=std::nullopt){
    // Stage is selected by the verified viewing contract, not a texture index.
    // Final SDR is already display encoded; no second gamma / ACES / exposure.
    if(d.stage==4)return {raw[0],raw[1],raw[2],1};
    if((d.stage==1||d.stage==2)&&d.nr_reconstruct){
        if(!rr)throw std::runtime_error("NR viewing reconstruction requires the recorded RR reference");
        auto working=d;working.stage=3;working.nr_reconstruct=0;
        return display_pixel(reconstruct_for_view(raw,*rr),working);
    }
    std::array<float,4> result{0,0,0,1};for(unsigned i=0;i<3;++i)if(!std::isfinite(raw[i]))return {d.hdr?d.white_nits/80.f:1.f,0,d.hdr?d.white_nits/80.f:1.f,1};
    const bool nr=d.stage==1||d.stage==2;double values[3];for(unsigned c=0;c<3;++c)values[c]=(nr&&!d.nr_linear?srgb_decode(std::max(0.f,raw[c])):std::max(0.f,raw[c]))*std::exp2(d.exposure);
    if(!nr&&d.mapping==3){const auto mapped=aces_fitted({values[0],values[1],values[2]});for(unsigned c=0;c<3;++c)values[c]=mapped[c];}
    if(!nr&&d.mapping==0){const double peak=d.hdr?std::max(1.,double(d.peak_nits)/d.white_nits):1.;
        const double top=std::max({values[0],values[1],values[2]});
        // One scale for RGB preserves ratios and bounds all channels, unlike
        // clipping individual saturated highlights. No adaptive A/B exposure.
        if(top>0){const double adjusted=.18*std::pow(top/.18,d.contrast);
            const double scale=viewing_curve(adjusted,peak)/top;for(auto& v:values)v*=scale;}}
    if(!nr&&d.mapping==2&&!d.hdr){if(d.curve==0){for(auto& v:values)v=filmic(v*2)/filmic(11.2);}
        else if(d.curve==1){const double luminance=.2126*values[0]+.7152*values[1]+.0722*values[2];const auto scale=(1+luminance/16)/(1+luminance);for(auto& v:values)v*=scale;}}
    if(d.hdr&&!nr&&d.mapping==2){const auto y=(.2126*values[0]+.7152*values[1]+.0722*values[2])*d.white_nits;
        const auto scale=y>0?shoulder_nits(y,d.white_nits,std::max(80.f,d.peak_nits))/y:1;for(auto& v:values)v*=scale;}
    for(unsigned c=0;c<3;++c)result[c]=float(d.hdr?std::min(65504.,values[c]*d.white_nits/80):std::clamp(srgb_encode(values[c]),0.,1.));return result;
}
}
