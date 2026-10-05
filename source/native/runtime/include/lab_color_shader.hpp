// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// Lab-authored HLSL. No captured bytecode is embedded/distributed.
// This is the legacy ADDON-COMPATIBLE codec, not the pre/post ratio colour path
// (lab_prepost_ratio_codec.hpp); do not relabel this math as official parity.
namespace lab::nr {
inline constexpr char color_shader[] = R"hlsl(
cbuffer CodecConstants : register(b0) {
    uint2 Size, SourceSize, SourceBase, ProxySize;
    float PaperWhiteScale, TransferStrength, ColorStrength;
    uint HdrMode;
    float4 Padding;
};
Texture2D<float4> Original : register(t0);
Texture2D<float4> Proxy : register(t1);
Texture2D<float4> Neural : register(t2);
Texture2D<float4> OutputOriginal : register(t3);
RWTexture2D<float4> Output : register(u0);
float encode(float x) {
    x = max(x, 0.0);
    if (!HdrMode) return x;
    x /= PaperWhiteScale;
    if (x > 0.75) x = 0.75 + 0.25 * (1.0 - exp2((x - 0.75) * -8.325476));
    x = saturate(x);
    return x <= 0.0031308 ? 12.92*x : 1.055*pow(x, 1.0/2.4)-0.055;
}
float decode(float x) {
    if (!HdrMode) return x;
    x = saturate(x);
    return x <= 0.04045 ? x/12.92 : pow((x+0.055)/1.055, 2.4);
}
float luminance(float3 x) { return dot(x, float3(0.212639,0.715169,0.072192)); }
float3 perceptual(float3 x) {
    float3 lms = float3(dot(x,float3(0.412221462,0.536332548,0.0514459945)),
        dot(x,float3(0.211903498,0.680699527,0.10739696)),dot(x,float3(0.0883024633,0.28171885,0.629978716)));
    lms = sign(lms)*pow(abs(lms),1.0/3.0);
    return float3(dot(lms,float3(0.210454255,0.793617785,-0.00407204684)),
        dot(lms,float3(1.9779985,-2.42859221,0.45059371)),dot(lms,float3(0.0259040371,0.782771766,-0.808675766)));
}
float3 linear_color(float3 x) {
    float3 lms = float3(dot(x,float3(1,0.396337777,0.215803757)),
        dot(x,float3(1,-0.105561346,-0.0638541728)),dot(x,float3(1,-0.0894841775,-1.29148555)));
    lms = lms*lms*lms;
    float3 rgb = float3(dot(lms,float3(4.0767417,-3.3077116,0.230969936)),
        dot(lms,float3(-1.26843798,2.60975742,-0.341319382)),dot(lms,float3(-0.00419608643,-0.703418612,1.70761466)));
    float3 wide = max(0.0,float3(dot(rgb,float3(0.613097,0.339523,0.047379)),
        dot(rgb,float3(0.070194,0.916354,0.013452)),dot(rgb,float3(0.020616,0.109570,0.869815))));
    return float3(dot(wide,float3(1.705051,-0.621792,-0.083259)),
        dot(wide,float3(-0.130256,1.140805,-0.010548)),dot(wide,float3(-0.024003,-0.128969,1.152972)));
}
[numthreads(16,16,1)] void Prepare(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size)) return;
    uint2 p = SourceBase + min(max(SourceSize,1)-1, uint2((float2(id.xy)+0.5)*max(SourceSize,1)/Size));
    float4 x = Original.Load(int3(p,0));
    Output[id.xy] = float4(encode(x.r),encode(x.g),encode(x.b),x.a);
}
[numthreads(16,16,1)] void Composite(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size)) return;
    float4 original = OutputOriginal.Load(int3(id.xy,0));
    float3 base = HdrMode ? max(original.rgb,0.0)/PaperWhiteScale : original.rgb;
    uint2 p = min(max(ProxySize,1)-1, uint2((float2(id.xy)+0.5)*max(ProxySize,1)/Size));
    float3 proxy = Proxy.Load(int3(p,0)).rgb;
    float3 neural = Neural.Load(int3(id.xy,0)).rgb;
    proxy = float3(decode(proxy.r),decode(proxy.g),decode(proxy.b));
    neural = float3(decode(neural.r),decode(neural.g),decode(neural.b));
    float sourceY = luminance(base), proxyY = luminance(proxy), neuralY = luminance(neural);
    float scale = sourceY < proxyY ? sourceY/proxyY : (neuralY>0 ? (neuralY+max(sourceY-proxyY,0))/neuralY : 0);
    float3 scaled = perceptual(neural*scale), raw = perceptual(neural);
    float chroma = length(raw.yz);
    scaled.yz = raw.yz * (chroma == 0 ? 1 : length(scaled.yz)/chroma);
    float3 transferred = HdrMode ? linear_color(scaled) : neural;
    float3 result = lerp(base,transferred,TransferStrength);
    float3 lumaOnly = base * (sourceY == 0 ? 1 : luminance(result)/sourceY);
    result = lerp(lumaOnly,result,ColorStrength);
    Output[id.xy] = float4(HdrMode ? result*PaperWhiteScale : result, original.a);
}
)hlsl";
}
