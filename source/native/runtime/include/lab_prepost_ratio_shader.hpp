// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
namespace lab::nr {
// Independent source, not embedded/repackaged proprietary shader bytecode.
inline constexpr char prepost_ratio_shader[]=R"HLSL(
cbuffer Params : register(b0) {
    float4 Pre0, Pre1, Pre2, Post0, Post1, Post2;
    float Exposure, Sigma, Gamma; uint Operator;
    uint Width, Height, PrePost, Reserved;
    float Extrapolate; uint3 ExtrapolatePad;
};
Texture2D<float4> Original : register(t0);
Texture2D<float4> Neural : register(t1);
Texture2D<float4> Hdr : register(t2);
Texture2D<float4> Sdr : register(t3);
Texture2D<float4> Prepared : register(t4);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float4> SavedHdr : register(u1);
RWTexture2D<float4> SavedSdr : register(u2);
float3 encode(float3 x) {
    x=saturate(x);
    return float3(x.r<=.0031308 ? x.r*12.92 : 1.055*pow(x.r,1.0/2.4)-.055,
                  x.g<=.0031308 ? x.g*12.92 : 1.055*pow(x.g,1.0/2.4)-.055,
                  x.b<=.0031308 ? x.b*12.92 : 1.055*pow(x.b,1.0/2.4)-.055);
}
float3 decode(float3 n) {
    return float3(n.r<=.04045 ? n.r/12.92 : pow(abs((n.r+.055)/1.055),2.4),
                  n.g<=.04045 ? n.g/12.92 : pow(abs((n.g+.055)/1.055),2.4),
                  n.b<=.04045 ? n.b/12.92 : pow(abs((n.b+.055)/1.055),2.4));
}
float3 tonemap(float3 h) {
    if(Operator==1) return h/(1+h);
    if(Operator==2) return h/(1+max(h.r,max(h.g,h.b)));
    if(Operator==3) {
        float lo=min(h.r,min(h.g,h.b)), hi=max(h.r,max(h.g,h.b));
        float a=lo/(1+lo),b=hi/(1+hi);
        float f=(b-a)*(hi==lo ? 1 : 1/(hi-lo));
        return a+f*(h-lo);
    }
    if(Operator==4) {
        float y=dot(h,float3(.2126,.7152,.0722));
        return h*((1-exp(-pow(y/Sigma,Gamma)))/(y+1e-6));
    }
    return h;
}
// Opt-in lab guard. Reserved is set internally by the codec, not a model knob.
// Guard-off remains the statically reconstructed official finite-domain math.
bool safe_rgb(float3 v) {
    return all(isfinite(v)) && all(v>=0) && (!(Reserved&2) || all(v<=65504));
}
[numthreads(8,8,1)]
void Prepare(uint3 id : SV_DispatchThreadID) {
    if(id.x>=Width || id.y>=Height) return;
    float4 x=Original.Load(int3(id.xy,0));
    float3 e=x.rgb*Exposure;
    float3 h=PrePost ? float3(dot(Pre0.xyz,e),dot(Pre1.xyz,e),dot(Pre2.xyz,e)) : x.rgb;
    if((Reserved&1) && (!safe_rgb(x.rgb) || !safe_rgb(h))) {
        SavedHdr[id.xy]=0; SavedSdr[id.xy]=0; Output[id.xy]=float4(0,0,0,x.a);return;
    }
    float3 s=PrePost ? tonemap(h) : h;
    if((Reserved&1) && !safe_rgb(s)) {
        SavedHdr[id.xy]=0; SavedSdr[id.xy]=0; Output[id.xy]=float4(0,0,0,x.a);return;
    }
    SavedHdr[id.xy]=float4(h,x.a);
    SavedSdr[id.xy]=float4(s,x.a);
    Output[id.xy]=float4(encode(s),x.a);
}
[numthreads(8,8,1)]
void Composite(uint3 id : SV_DispatchThreadID) {
    if(id.x>=Width || id.y>=Height) return;
    float4 x=Original.Load(int3(id.xy,0));float3 raw=Neural.Load(int3(id.xy,0)).rgb;
    if((Reserved&4) && id.x<Width/2) {Output[id.xy]=x;return;} // diagnostic split: left = untouched working RGB
    if(Reserved&1) {
        float3 e=x.rgb*Exposure;
        float3 h=PrePost ? float3(dot(Pre0.xyz,e),dot(Pre1.xyz,e),dot(Pre2.xyz,e)) : x.rgb;
        if(!safe_rgb(x.rgb) || !safe_rgb(h) || !safe_rgb(PrePost?tonemap(h):h) || !safe_rgb(raw)) {Output[id.xy]=x;return;}
    }
    // Lab extension, edit extrapolation (Reserved&8, set only for a factor other
    // than 1): amplify this pass's edit in the NR API domain before the ratio
    // transfer, N' = saturate(C + k(N - C)) with C the encoded image NR was given.
    // Only what is composited back changes; NR keeps its own output as history.
    if(Reserved&8) {float3 c=Prepared.Load(int3(id.xy,0)).rgb; raw=saturate(c+Extrapolate*(raw-c));}
    float3 n=decode(raw);
    if(PrePost) {
        float3 r=clamp((n+1e-6)/(Sdr.Load(int3(id.xy,0)).rgb+1e-6),.01,10);
        float3 h=Hdr.Load(int3(id.xy,0)).rgb*r;
        n=float3(dot(Post0.xyz,h),dot(Post1.xyz,h),dot(Post2.xyz,h))/Exposure;
    }
    if((Reserved&1) && !safe_rgb(n)) {Output[id.xy]=x;return;}
    Output[id.xy]=float4(n,x.a);
}
)HLSL";
}
