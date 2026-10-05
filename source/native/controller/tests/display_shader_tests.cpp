// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_platform.hpp"
#include "lab_viewer_color.hpp"
#include "lab_viewer_shader.hpp"
#include "lab_overlay_shader.hpp"
#include "lab_preconvert_reference.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <iostream>
#include <fstream>
#include <map>
using Microsoft::WRL::ComPtr;
namespace {
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("Display GPU HRESULT "+std::to_string(unsigned(r)));}
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
constexpr char vertex_source[]=R"HLSL(struct Output{float4 pos:SV_POSITION;float4 color:COLOR0;float2 uv:TEXCOORD0;};Output VS(uint id:SV_VertexID){Output o;o.pos=float4(id==2?3:-1,id==1?3:-1,0,1);o.uv=float2((o.pos.x+1)/2,(1-o.pos.y)/2);o.color=1;return o;})HLSL";
struct Gpu {
    std::map<std::string,ComPtr<ID3DBlob>> compiled;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11Texture2D> output,readback;
    ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11Buffer> constants;ComPtr<ID3D11SamplerState> sampler;
    Gpu(){D3D_FEATURE_LEVEL level;hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context));
        D3D11_TEXTURE2D_DESC d{};d.Width=8;d.Height=1;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        hr(device->CreateTexture2D(&d,nullptr,&output));hr(device->CreateRenderTargetView(output.Get(),nullptr,&target));d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;hr(device->CreateTexture2D(&d,nullptr,&readback));
        D3D11_BUFFER_DESC b{};b.ByteWidth=48;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;hr(device->CreateBuffer(&b,nullptr,&constants));
        D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;hr(device->CreateSamplerState(&s,&sampler));
        D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;hr(device->CreateRasterizerState(&r,&raster));context->RSSetState(raster.Get());
    }
    ComPtr<ID3DBlob> compile(const char* source,const char* entry,const char* profile){const auto key=std::string(source)+entry+profile;if(compiled.contains(key))return compiled.at(key);
        ComPtr<ID3DBlob> code,errors;const auto r=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&errors);
        if(FAILED(r))throw std::runtime_error(errors?std::string(static_cast<char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"Display shader compile failed");compiled.emplace(key,code);return code;}
    ComPtr<ID3D11ShaderResourceView> texture(const std::array<std::array<float,4>,8>& pixels){std::array<std::array<std::uint16_t,4>,8> half{};for(unsigned x=0;x<8;++x)for(unsigned c=0;c<4;++c)half[x][c]=lab::preconvert::half(pixels[x][c]);
        D3D11_TEXTURE2D_DESC d{};d.Width=8;d.Height=1;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.Usage=D3D11_USAGE_IMMUTABLE;
        D3D11_SUBRESOURCE_DATA p{half.data(),64,0};ComPtr<ID3D11Texture2D> t;hr(device->CreateTexture2D(&d,&p,&t));ComPtr<ID3D11ShaderResourceView> view;hr(device->CreateShaderResourceView(t.Get(),nullptr,&view));return view;}
    template<class T> std::array<std::array<float,4>,8> draw(const char* vs_source,const char* ps_source,const char* entry,const T* data,ID3D11ShaderResourceView* a,ID3D11ShaderResourceView* b,ID3D11ShaderResourceView* reference=nullptr){
        auto v=compile(vs_source,"VS","vs_5_0"),p=compile(ps_source,entry,"ps_5_0");ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
        hr(device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs));hr(device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps));context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        static_assert(sizeof(T)<=48);std::array<std::byte,48> padded{};memcpy(padded.data(),data,sizeof(T));context->UpdateSubresource(constants.Get(),0,nullptr,padded.data(),0,0);context->PSSetConstantBuffers(0,1,constants.GetAddressOf());ID3D11ShaderResourceView* inputs[]{a,b,reference};context->PSSetShaderResources(0,3,inputs);context->PSSetSamplers(0,1,sampler.GetAddressOf());
        context->OMSetRenderTargets(1,target.GetAddressOf(),nullptr);D3D11_VIEWPORT viewport{0,0,8,1,0,1};context->RSSetViewports(1,&viewport);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->Draw(3,0);
        context->CopyResource(readback.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};hr(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));std::array<std::array<float,4>,8> result;
        for(unsigned x=0;x<8;++x)for(unsigned c=0;c<4;++c)result[x][c]=lab::preconvert::unhalf(static_cast<const std::uint16_t*>(mapped.pData)[x*4+c]);context->Unmap(readback.Get(),0);return result;
    }
};
double quant(double x){return lab::preconvert::unhalf(lab::preconvert::half(float(x)));}
double pq(double v,bool encode){const double m1=2610./16384,m2=2523./32,c1=3424./4096,c2=2413./128,c3=2392./128;
    if(encode){auto p=std::pow(std::clamp(v/10000.,0.,1.),m1);return std::pow((c1+c2*p)/(1+c3*p),m2);}auto p=std::pow(std::clamp(v,0.,1.),1/m2);return 10000*std::pow(std::max(p-c1,0.)/std::max(c2-c3*p,1e-7),1/m1);}
}
int wmain(int argc,wchar_t** argv){try{
    Gpu gpu;unsigned cases=0;double max_error=0;const float levels[]{0,.018f,.18f,1,5,128,-.5f,INFINITY};std::array<std::array<float,4>,8> pixels{};
    for(unsigned x=0;x<8;++x)pixels[x]={float(quant(levels[x])),float(quant(levels[x]/2)),float(quant(levels[x]/4)),1};const auto image=gpu.texture(pixels);
    for(unsigned mapping=0;mapping<4;++mapping)for(float contrast:{1.f,1.5f})for(unsigned hdr=0;hdr<2;++hdr)for(unsigned stage:{0u,1u,2u,3u})for(unsigned curve=0;curve<3;++curve){lab::viewer::Display d;d.mapping=mapping;d.contrast=contrast;d.hdr=hdr;d.stage=stage;d.curve=curve;d.exposure=curve==1?-1.f:0;d.white_nits=203;
        const auto output=gpu.draw(vertex_source,lab::viewer::shader,"PSImage",&d,image.Get(),nullptr);
        for(unsigned x=0;x<8;++x){const auto expected=lab::viewer::display_pixel(pixels[x],d);for(unsigned c=0;c<4;++c){const double e=std::abs(output[x][c]-quant(expected[c]));max_error=std::max(max_error,e);if(!(e<=.001+.001*std::abs(expected[c])))throw std::runtime_error("Viewer CPU/GPU mismatch hdr="+std::to_string(hdr)+" stage="+std::to_string(stage)+" curve="+std::to_string(curve)+" x="+std::to_string(x)+" channel="+std::to_string(c)+" expected="+std::to_string(quant(expected[c]))+" actual="+std::to_string(output[x][c]));}}
        ++cases;}
    unsigned aces_checks=0;double last=-1;
    for(unsigned i=0;i<10000;++i){double x=i*.01;auto rgb=lab::viewer::aces_fitted({x,x,x});
        // Published rounded blue output row sums to .99999, not 1. Preserve
        // that reference rather than modifying the matrix to force neutrality.
        const double scalar=(x*(x+.0245786)-.000090537)/(x*(.983729*x+.4329510)+.238081);
        need(rgb[0]>=last&&rgb[0]>=0&&rgb[0]<=1&&std::abs(rgb[0]-rgb[1])<1e-12&&std::abs(std::clamp(scalar*.99999,0.,1.)-rgb[2])<1e-12,"ACES neutral/range/monotonic");last=rgb[0];++aces_checks;}
    need(lab::viewer::aces_fitted({0,0,0})==std::array<double,3>{0,0,0},"ACES black");
    // Fixed neutral anchor from the published scalar formula; no extra gamma.
    need(std::abs(lab::viewer::aces_fitted({.18,.18,.18})[0]-.10559124722893054)<1e-12,"ACES neutral anchor");
    const std::array<std::array<float,4>,8> colors{{{1,0,0,1},{0,1,0,1},{0,0,1,1},{10,1,.01f,1},{.18f,.18f,.18f,1},{65504,0,0,1},{1,10,100,1},{0,0,0,1}}};
    auto colorful=gpu.texture(colors);lab::viewer::Display aces;aces.mapping=3;
    unsigned final_identity_checks=0;
    for(unsigned mapping=0;mapping<4;++mapping)for(float ev:{-6.f,6.f}){lab::viewer::Display d;d.stage=4;d.mapping=mapping;d.exposure=ev;d.contrast=1.5;d.nr_linear=1;
        const auto final=gpu.draw(vertex_source,lab::viewer::shader,"PSImage",&d,colorful.Get(),nullptr);
        for(unsigned x=0;x<8;++x)for(unsigned c=0;c<3;++c){need(final[x][c]==quant(colors[x][c]),"Final code shader must ignore image controls");++final_identity_checks;}++cases;}
    const auto rendered=gpu.draw(vertex_source,lab::viewer::shader,"PSImage",&aces,colorful.Get(),nullptr);
    for(unsigned x=0;x<8;++x){auto pixel=colors[x];for(auto& v:pixel)v=float(quant(v));const auto expected=lab::viewer::display_pixel(pixel,aces);
        for(unsigned c=0;c<3;++c)need(std::abs(rendered[x][c]-quant(expected[c]))<=.001+.001*std::abs(expected[c]),"ACES colored CPU/GPU");++aces_checks;}
    for(unsigned stage:{1u,2u}){aces.stage=stage;auto direct=aces;direct.mapping=1;
        need(lab::viewer::display_pixel({.2f,.5f,.8f,1},aces)==lab::viewer::display_pixel({.2f,.5f,.8f,1},direct),"NR compressed proxy must bypass scene tone mapping");++aces_checks;}
    unsigned curve_checks=0;for(double peak:{1.,2.,5.,50.}){double previous=-1;
        for(unsigned i=0;i<10000;++i){const double x=i*.02,y=lab::viewer::viewing_curve(x,peak);need(std::isfinite(y)&&y>=previous&&y<=peak,"Viewing curve must be monotone, finite, bounded");previous=y;++curve_checks;}
        need(lab::viewer::viewing_curve(0,peak)==0,"Black is not lifted");need(lab::viewer::viewing_curve(.018,peak)<.018,"Toe must retain dark contrast");
        need(std::abs(lab::viewer::viewing_curve(.22,peak)-.22)<1e-12,"Linear middle anchor");}
    unsigned reconstruction_checks=0;
    std::array<std::array<float,4>,8> neural{{{.2f,.5f,.8f,1},{0,0,0,1},{1,1,1,1},{.8f,.9f,.99f,1},{-.01f,.5f,.5f,1},{NAN,.5f,.5f,1},{INFINITY,0,0,1},{2,.7f,.1f,1}}};
    for(auto& p:neural)for(auto& v:p)v=float(quant(v));auto neural_texture=gpu.texture(neural);
    for(unsigned stage:{1u,2u})for(unsigned hdr:{0u,1u})for(unsigned mapping:{0u,1u,3u}){lab::viewer::Display d;d.nr_reconstruct=1;d.stage=stage;d.hdr=hdr;d.mapping=mapping;
        const auto actual=gpu.draw(vertex_source,lab::viewer::shader,"PSImage",&d,neural_texture.Get(),nullptr,colorful.Get());
        for(unsigned x=0;x<8;++x){auto rr=colors[x];for(auto& v:rr)v=float(quant(v));const auto expected=lab::viewer::display_pixel(neural[x],d,rr);
            for(unsigned c=0;c<3;++c)if(!(std::abs(actual[x][c]-quant(expected[c]))<=.001+.001*std::abs(expected[c]))){
                const auto probe_source=std::string(lab::viewer::shader)+"float4 PSProbe(Input i):SV_Target {return float4(reconstructed_texel(int2(floor(i.uv*int2(8,1))),int2(8,1)),1);}";
                const auto probe=gpu.draw(vertex_source,probe_source.c_str(),"PSProbe",&d,neural_texture.Get(),nullptr,colorful.Get());
                const auto raw_source=std::string(lab::viewer::shader)+"float4 PSProbe(Input i):SV_Target {float3 v=Image.Load(int3(int2(floor(i.uv*int2(8,1))),0)).rgb;return float4(v,any(isnan(v))?1:0);}";
                const auto raw_probe=gpu.draw(vertex_source,raw_source.c_str(),"PSProbe",&d,neural_texture.Get(),nullptr,colorful.Get());
                throw std::runtime_error("Reconstruction CPU/GPU mismatch "+lab::json{{"stage",stage},{"hdr",hdr},{"mapping",mapping},{"x",x},{"channel",c},{"actual",actual[x]},{"expected",expected},{"raw",neural[x]},{"rr",rr},{"gpu_reconstruction",probe[x]},{"raw_probe",raw_probe[x]}}.dump());}++reconstruction_checks;}
        d.nr_reconstruct=0;const auto disabled=gpu.draw(vertex_source,lab::viewer::shader,"PSImage",&d,neural_texture.Get(),nullptr,colorful.Get());
        const auto no_reference=gpu.draw(vertex_source,lab::viewer::shader,"PSImage",&d,neural_texture.Get(),nullptr);
        need(disabled==no_reference,"Disabled viewing must not consume RR reference");++cases;
    }
    {lab::viewer::Display d;d.stage=2;d.nr_reconstruct=3;d.mapping=3;
        const std::string shifted="struct Output{float4 pos:SV_POSITION;float4 color:COLOR0;float2 uv:TEXCOORD0;};Output VS(uint id:SV_VertexID){Output o;o.pos=float4(id==2?3:-1,id==1?3:-1,0,1);o.uv=float2((o.pos.x+1)/2+.03125,(1-o.pos.y)/2);o.color=1;return o;}";
        const auto actual=gpu.draw(shifted.c_str(),lab::viewer::shader,"PSImage",&d,neural_texture.Get(),nullptr,colorful.Get());
        for(unsigned x=0;x<8;++x){auto rr0=colors[x],rr1=colors[std::min(x+1,7u)];for(auto& v:rr0)v=float(quant(v));for(auto& v:rr1)v=float(quant(v));
            const auto a=lab::viewer::reconstruct_for_view(neural[x],rr0),b=lab::viewer::reconstruct_for_view(neural[std::min(x+1,7u)],rr1);
            std::array<float,4> mixed{};for(unsigned c=0;c<4;++c)mixed[c]=a[c]*.75f+b[c]*.25f;auto working=d;working.stage=3;working.nr_reconstruct=0;
            const auto expected=lab::viewer::display_pixel(mixed,working);for(unsigned c=0;c<3;++c)need(std::abs(actual[x][c]-quant(expected[c]))<=.001+.001*std::abs(expected[c]),"Reconstruct each source texel before resampling");++reconstruction_checks;
        }++cases;
        d.nr_reconstruct=0;const auto raw_difference=gpu.draw(vertex_source,lab::viewer::shader,"PSDifference",&d,neural_texture.Get(),colorful.Get());
        d.nr_reconstruct=1;const auto requested_difference=gpu.draw(vertex_source,lab::viewer::shader,"PSDifference",&d,neural_texture.Get(),colorful.Get(),colorful.Get());
        need(raw_difference==requested_difference,"Raw difference must ignore reconstruction request");++cases;
    }
    std::array<std::array<float,4>,8> scene{},ui{};for(unsigned x=0;x<8;++x){const float alpha=x%3==0?0:x%3==1?.5f:1.f;scene[x]={.5f,.25f,.125f,.75f};ui[x]={.8f*alpha,.4f*alpha,.2f*alpha,alpha};}
    const auto background=gpu.texture(scene),ink=gpu.texture(ui);
    unsigned ui_checks=0;for(unsigned hdr:{0u,1u})for(float white:{80.f,400.f})for(float exposure:{-6.f,6.f}){lab::viewer::Display d;d.hdr=hdr;d.white_nits=white;d.ui_white_nits=160;d.exposure=exposure;d.peak_nits=4000;d.contrast=1.5;
        const auto output=gpu.draw(vertex_source,lab::viewer::shader,"PSUI",&d,background.Get(),nullptr);
        for(unsigned x=0;x<8;++x)for(unsigned c=0;c<3;++c){const double expected=hdr?lab::viewer::srgb_decode(scene[x][c])*2:scene[x][c];need(std::abs(output[x][c]-quant(expected))<.001,"UI brightness must ignore all image controls");++ui_checks;}++cases;}
    for(unsigned transfer=0;transfer<3;++transfer){struct alignas(16) Constants{unsigned transfer;float white;float pad[6];}constants{transfer,203,{}};
        const auto output=gpu.draw(lab::overlay_shader,lab::overlay_shader,"PS",&constants,background.Get(),ink.Get());
        for(unsigned x=0;x<8;++x){const auto alpha=ui[x][3];double decoded[3]{};if(alpha)for(unsigned c=0;c<3;++c)decoded[c]=lab::viewer::srgb_decode(quant(ui[x][c])/alpha);
            const double matrix[3][3]{{.627403896,.329283038,.043313066},{.069097289,.919540395,.011362316},{.016391439,.088013308,.895595253}};
            for(unsigned c=0;c<3;++c){double expected=scene[x][c];if(alpha){if(transfer==0)expected=lab::viewer::srgb_encode(lab::viewer::srgb_decode(expected)*(1-alpha)+decoded[c]*alpha);
                    else if(transfer==1)expected=expected*(1-alpha)+decoded[c]*203/80*alpha;
                    else {double nits=0;for(unsigned j=0;j<3;++j)nits+=matrix[c][j]*decoded[j]*203;expected=pq(pq(expected,false)*(1-alpha)+nits*alpha,true);}}
                const double e=std::abs(output[x][c]-quant(expected));max_error=std::max(max_error,e);need(e<=.001+.001*std::abs(expected),"Overlay CPU/GPU blend reference mismatch");}
            need(output[x][3]==scene[x][3],"Overlay changed scene alpha");}
        ++cases;
    }
    lab::json result={{"purpose","functional-verification"},{"origin","synthetic-display-shaders"},{"passed",true},{"cases",cases},{"max_absolute_fp16_error",max_error},{"game_started",false},{"nr_executed",false},{"raw_files",0},{"monitor_calibration_proven",false},{"modules",lab::current_modules()}};
    result["curve_monotonic_checks"]=curve_checks;result["ui_invariance_channel_checks"]=ui_checks;
    result["aces_checks"]=aces_checks;
    result["final_identity_channel_checks"]=final_identity_checks;
    result["reconstruction_pixel_checks"]=reconstruction_checks;
    if(argc==2){const std::filesystem::path path=argv[1];need(path.is_absolute()&&!std::filesystem::exists(path),"New report path required");std::ofstream out(path,std::ios::binary);out<<result.dump(2);}std::cout<<"PASS "<<cases<<" independent CPU/GPU display color cases; maximum error="<<max_error<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
