// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
namespace lab::viewer {
inline constexpr char shader[]=R"HLSL(
Texture2D<float4> Image:register(t0);Texture2D<float4> Other:register(t1);SamplerState Sample:register(s0);
Texture2D<float4> RrReference:register(t2);
cbuffer Display:register(b0){uint Stage;uint HDR;uint Curve;uint NrLinear;float Exposure;float WhiteNits;float DifferenceGain;float PeakNits;uint Mapping;float Contrast;float UiWhiteNits;uint NrReconstruct;};
struct Input{float4 pos:SV_POSITION;float4 color:COLOR0;float2 uv:TEXCOORD0;};
float decode1(float x){return x<=.04045?x/12.92:pow((x+.055)/1.055,2.4);}
float encode1(float x){x=max(x,0);return x<=.0031308?12.92*x:1.055*pow(x,1/2.4)-.055;}
float3 decode(float3 x){return float3(decode1(x.r),decode1(x.g),decode1(x.b));}
float3 encode(float3 x){return float3(encode1(x.r),encode1(x.g),encode1(x.b));}
float3 filmic(float3 x){return ((x*(.15*x+.1*.5)+.2*.02)/(x*(.15*x+.5)+.2*.3))-.02/.3;}
// Stephen Hill / MJP BakingLab fit; MIT, third_party/BakingLab-LICENSE.txt.
// Display-only approximation, never applied directly to compressed NR images.
float3 aces_fitted(float3 rgb){
    float3 v=mul(float3x3(.59719,.35458,.04823,.07600,.90834,.01566,.02840,.13383,.83777),rgb);
    v=(v*(v+.0245786)-.000090537)/(v*(.983729*v+.4329510)+.238081);
    return saturate(mul(float3x3(1.60475,-.53108,-.07367,-.10208,1.10813,-.00605,-.00327,-.07276,1.07602),v));
}
float viewing_curve(float x,float peak){float middle=.22,join=middle+(peak-middle)*.4;
    if(x>=join)return peak-(peak-join)*exp(-(x-join)/(peak-join));
    float t=saturate(x/middle),w=t*t*(3-2*t);return (1-w)*middle*pow(x/middle,1.33)+w*x;}
bool invalid_view_rgb(float3 v){return any((asuint(v)&0x7fffffffu)>=0x7f800000u)||any(v<0)||any(v>65504);}
float half_round_even(float x){
    // Explicit binary16 nearest-even, matching stored FP16 intermediates and
    // the CPU reference; do not depend on a conversion instruction's rounding.
    uint bits=asuint(x),m=bits&0x7fffff;int e=int((bits>>23)&255)-127;
    if(e < -25)return 0;
    uint shift=e < -14?uint(-e-1):13u,significand=e < -14?m|0x800000:m;
    uint q=significand>>shift,r=significand&((1u<<shift)-1),mid=1u<<(shift-1);
    if(r>mid||(r==mid&&(q&1)))++q;
    return f16tof32((e < -14?0u:uint(e+15)<<10)+q);
}
float3 round_half3(float3 v){return float3(half_round_even(v.r),half_round_even(v.g),half_round_even(v.b));}
float3 reconstruct_for_view(float3 raw,float3 rr){
    if(invalid_view_rgb(raw)||invalid_view_rgb(rr))return rr;
    float lo=min(rr.r,min(rr.g,rr.b)),hi=max(rr.r,max(rr.g,rr.b));
    float a=lo/(1+lo),b=hi/(1+hi),f=(b-a)*(hi==lo?1:1/(hi-lo));
    float3 s=round_half3(a+f*(rr-lo));
    float3 v=rr*clamp((decode(raw)+1e-6)/(s+1e-6),.01,10);
    if(invalid_view_rgb(v))return rr;
    return round_half3(v);
}
float3 reconstructed_texel(int2 p,int2 size){
    p=clamp(p,0,size-1);
    // Load preserves nonfinite texels for the whole-pixel guard. Hardware
    // filtering of a NaN must not turn it into a valid zero before validation.
    return reconstruct_for_view(Image.Load(int3(p,0)).rgb,RrReference.Load(int3(p,0)).rgb);
}
float3 sample_reconstructed(float2 uv){
    uint w,h;Image.GetDimensions(w,h);int2 size=int2(w,h);
    if(!(NrReconstruct&2))return reconstructed_texel(int2(floor(uv*size)),size);
    float2 p=uv*size-.5,t=frac(p);int2 origin=int2(floor(p));
    return lerp(lerp(reconstructed_texel(origin,size),reconstructed_texel(origin+int2(1,0),size),t.x),
                lerp(reconstructed_texel(origin+int2(0,1),size),reconstructed_texel(origin+int2(1,1),size),t.x),t.y);
}
float3 display(float3 raw,bool reconstructed){
    if(Stage==4)return raw; // Validated SDR code view; no second transform.
    if(any(isnan(raw))||any(isinf(raw)))return float3(1,0,1)*(HDR?WhiteNits/80:1);
    bool nr=(Stage==1||Stage==2)&&!reconstructed;float3 radiance=(nr&&!NrLinear?decode(max(raw,0)):max(raw,0))*exp2(Exposure);
    if(!nr&&Mapping==3)radiance=aces_fitted(radiance);
    if(!nr&&Mapping==0){float peak=HDR?max(1,PeakNits/WhiteNits):1;float top=max(radiance.r,max(radiance.g,radiance.b));
        if(top>0)radiance*=viewing_curve(.18*pow(top/.18,Contrast),peak)/top;}
    if(!nr&&Mapping==2&&!HDR){if(Curve==0)radiance=filmic(radiance*2)/filmic(11.2);else if(Curve==1){float y=dot(radiance,float3(.2126,.7152,.0722));radiance*=(1+y/16)/(1+y);}}
    if(HDR&&!nr&&Mapping==2){float y=dot(radiance,float3(.2126,.7152,.0722))*WhiteNits;float peak=max(80,PeakNits),start=min(WhiteNits,peak*.75);
        if(y>start){float mapped=start+(peak-start)*(1-exp(-(y-start)/(peak-start)));radiance*=mapped/y;}}
    return HDR?min(radiance*(WhiteNits/80),65504):saturate(encode(radiance));
}
float4 PSImage(Input i):SV_Target{
    float3 raw=Image.Sample(Sample,i.uv).rgb;bool reconstructed=NrReconstruct&&(Stage==1||Stage==2);
    if(reconstructed)raw=sample_reconstructed(i.uv);
    return float4(display(raw,reconstructed),1);
}
float4 PSDifference(Input i):SV_Target{
    float3 a=Image.Sample(Sample,i.uv).rgb,b=Other.Sample(Sample,i.uv).rgb;
    if(any(isnan(a))||any(isinf(a))||any(isnan(b))||any(isinf(b)))return float4(float3(1,0,1)*(HDR?WhiteNits/80:1),1);
    float3 delta=saturate(abs(a-b)*DifferenceGain);return float4(HDR?decode(delta)*(WhiteNits/80):delta,1);
}
float4 PSUI(Input i):SV_Target{float4 v=Image.Sample(Sample,i.uv)*i.color;return float4(HDR?decode(saturate(v.rgb))*(UiWhiteNits/80):v.rgb,v.a);}
)HLSL";
}
