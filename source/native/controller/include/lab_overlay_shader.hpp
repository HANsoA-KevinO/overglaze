// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
namespace lab {
// UI is first rendered into an encoded, premultiplied RGBA16F surface.
// Only nonzero-alpha pixels are converted/composited in display-linear light.
// Windows scRGB uses 80 nits per 1.0; PQ uses Rec.2020 / ST.2084, 10000 nits.
// Source: Microsoft Advanced Color documentation (scRGB and HDR10 swapchains).
inline constexpr char overlay_shader[]=R"HLSL(
Texture2D<float4> Scene:register(t0);
Texture2D<float4> UI:register(t1);
cbuffer Constants:register(b0){uint Transfer;float PaperWhite;float2 Padding;};
float3 srgb_to_linear(float3 x){return float3(x.r<=.04045?x.r/12.92:pow((x.r+.055)/1.055,2.4),
    x.g<=.04045?x.g/12.92:pow((x.g+.055)/1.055,2.4),x.b<=.04045?x.b/12.92:pow((x.b+.055)/1.055,2.4));}
float3 linear_to_srgb(float3 x){x=max(x,0);return float3(x.r<=.0031308?12.92*x.r:1.055*pow(x.r,1/2.4)-.055,
    x.g<=.0031308?12.92*x.g:1.055*pow(x.g,1/2.4)-.055,x.b<=.0031308?12.92*x.b:1.055*pow(x.b,1/2.4)-.055);}
float3 pq_decode(float3 x){float3 p=pow(saturate(x),1/(2523.0/32));return 10000*pow(max(p-3424.0/4096,0)/max(2413.0/128-(2392.0/128)*p,1e-7),1/(2610.0/16384));}
float3 pq_encode(float3 x){float3 p=pow(saturate(x/10000),2610.0/16384);return pow((3424.0/4096+(2413.0/128)*p)/(1+(2392.0/128)*p),2523.0/32);}
float3 to2020(float3 c){return float3(dot(c,float3(.627403896,.329283038,.043313066)),
    dot(c,float3(.069097289,.919540395,.011362316)),dot(c,float3(.016391439,.088013308,.895595253)));}
float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}
float4 PS(float4 pos:SV_Position):SV_Target{
    int3 p=int3(int2(pos.xy),0);float4 scene=Scene.Load(p),ui=UI.Load(p);
    if(ui.a<=0)return scene;
    float alpha=saturate(ui.a);float3 ink=srgb_to_linear(saturate(ui.rgb/max(alpha,1e-6)));
    float3 mixed;
    if(Transfer==1)mixed=lerp(scene.rgb,ink*(PaperWhite/80),alpha);
    else if(Transfer==2)mixed=pq_encode(lerp(pq_decode(scene.rgb),to2020(ink)*PaperWhite,alpha));
    else mixed=linear_to_srgb(lerp(srgb_to_linear(saturate(scene.rgb)),ink,alpha));
    return float4(mixed,scene.a);
}
)HLSL";
}
