// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Edit extrapolation in the pre/post colour composite (Live ABI25), on WARP: no
// hardware GPU, no model. A texture the test writes plays NR's output N; C is the
// prepared NR input the codec itself wrote, read back.
//
//   1. never set, and a factor of 1, are the plain composite byte for byte, also
//      on a frame after an extrapolated one;
//   2. factor k composites N' = saturate(C + k(N - C)): equal to the plain
//      composite of N' precomputed on the CPU (RGBA32F), with N' clamped at
//      both ends somewhere;
//   3. the result is still the bounded per-channel ratio transfer onto the
//      saved working image, H x clamp((decode(N') + e) / (S + e), .01, 10) / E;
//   4. with it on, the invalid-RGB guard and the compare split behave as before;
//   5. the factor contract: finite 1..4, changed only between frames.
#include "lab_prepost_ratio_codec.hpp"
#include "lab_preconvert_reference.hpp"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Bytes=std::vector<unsigned char>;
namespace {
constexpr unsigned W=37,H=23,N=W*H;
constexpr float exposure=1.75f;
int checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
void hr(HRESULT r,const char* what){if(FAILED(r))throw std::runtime_error(std::string(what)+" failed");}
template<class F> void refused(F f,const char* what){bool threw=false;try{f();}catch(const std::logic_error&){threw=true;}need(threw,what);}
float get(const Bytes& b,std::size_t i,bool half){if(half){std::uint16_t h;std::memcpy(&h,b.data()+i*2,2);return lab::preconvert::unhalf(h);}float v;std::memcpy(&v,b.data()+i*4,4);return v;}
void put(Bytes& b,std::size_t i,float v,bool half){if(half){const auto h=lab::preconvert::half(v);std::memcpy(b.data()+i*2,&h,2);}else std::memcpy(b.data()+i*4,&v,4);}
// The shader's sRGB decode, in the same order of operations.
float decode(float n){return n<=.04045f?n/12.92f:std::pow(std::abs((n+.055f)/1.055f),2.4f);}
struct Gpu {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;ComPtr<ID3D12InfoQueue> info;
    UINT64 value=0;HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    std::vector<ComPtr<ID3D12Resource>> uploads;
    Gpu(){
        // The debug layer is optional: with it, a wrong barrier or a descriptor
        // the shader cannot use is an error message; without it the values are
        // still checked.
        ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"factory");
        ComPtr<IDXGIAdapter> warp;hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"warp");
        hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
        if(debug)(void)device.As(&info);
        D3D12_COMMAND_QUEUE_DESC qd{};hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
        hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    }
    ~Gpu(){CloseHandle(event);}
    void run(){
        hr(list->Close(),"close");ID3D12CommandList* l[]{list.Get()};queue->ExecuteCommandLists(1,l);
        hr(queue->Signal(fence.Get(),++value),"signal");hr(fence->SetEventOnCompletion(value,event),"event");
        need(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU wait");
        hr(allocator->Reset(),"allocator reset");hr(list->Reset(allocator.Get(),nullptr),"list reset");uploads.clear();
    }
    void barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        if(a==b)return;D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,0,a,b};list->ResourceBarrier(1,&x);
    }
    ComPtr<ID3D12Resource> texture(DXGI_FORMAT f,bool uav){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=W;d.Height=H;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=f;d.SampleDesc.Count=1;
        d.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,
            uav?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"texture");return r;
    }
    ComPtr<ID3D12Resource> buffer(UINT64 size,D3D12_HEAP_TYPE type){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=1;
        d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,
            type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"buffer");return r;
    }
    void upload(ID3D12Resource* t,const Bytes& bytes,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        const auto d=t->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows=0;UINT64 row=0,total=0;
        device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&row,&total);
        auto u=buffer(total,D3D12_HEAP_TYPE_UPLOAD);void* mapped=nullptr;hr(u->Map(0,nullptr,&mapped),"map");
        for(unsigned y=0;y<H;++y)std::memcpy(static_cast<BYTE*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,bytes.data()+y*row,static_cast<std::size_t>(row));
        u->Unmap(0,nullptr);barrier(t,before,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=t;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=u.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(t,D3D12_RESOURCE_STATE_COPY_DEST,after);uploads.push_back(u);
    }
    Bytes read(ID3D12Resource* t,D3D12_RESOURCE_STATES state){
        const auto d=t->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows=0;UINT64 row=0,total=0;
        device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&row,&total);auto b=buffer(total,D3D12_HEAP_TYPE_READBACK);
        barrier(t,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=t;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=b.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(t,D3D12_RESOURCE_STATE_COPY_SOURCE,state);run();
        void* mapped=nullptr;hr(b->Map(0,nullptr,&mapped),"readback map");Bytes out(static_cast<std::size_t>(row)*H);
        for(unsigned y=0;y<H;++y)std::memcpy(out.data()+y*row,static_cast<BYTE*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,static_cast<std::size_t>(row));
        D3D12_RANGE none{};b->Unmap(0,&none);return out;
    }
    std::vector<std::string> errors(){
        std::vector<std::string> out;if(!info)return out;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;hr(info->GetMessage(i,nullptr,&size),"message size");
            std::vector<std::uint8_t> b(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data());hr(info->GetMessage(i,m,&size),"message");
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)out.emplace_back(m->pDescription,m->DescriptionByteLength);}
        return out;
    }
};
struct Output {Bytes prepared,composite,hdr,sdr;};
// One codec, one frame per entry of `factors` with the same inputs; the
// readbacks of the last frame. A factor of 0 means set_extrapolation is never
// called for that frame.
Output composite(Gpu& g,bool half,bool guard,bool split,std::initializer_list<float> factors,const Bytes& source,const Bytes& neural){
    const auto f=half?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto original=g.texture(f,false),prepared=g.texture(f,true),nr=g.texture(f,true),out=g.texture(f,true),hdr=g.texture(f,true),sdr=g.texture(f,true);
    g.upload(original.Get(),source,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.upload(nr.Get(),neural,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);g.run();
    lab::nr::PrePostColorInputs inputs{original.Get(),prepared.Get(),nr.Get(),out.Get(),hdr.Get(),sdr.Get(),true,!guard,guard};
    lab::nr::PrePostColorCodec codec(g.device.Get(),inputs);codec.set_compare_split(split);
    lab::nr::PrePostColorConstants c{};c.width=W;c.height=H;c.pre_post=1;c.tonemap_operator=3;c.exposure=exposure;c.sigma=10;c.gamma=.454f;
    c.pre_matrix={{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};c.post_matrix=c.pre_matrix;
    for(const float factor:factors){
        if(factor>0)codec.set_extrapolation(factor);
        codec.prepare(g.list.Get(),c);codec.composite(g.list.Get());codec.bind_completion(g.fence.Get(),g.value+1);g.run();
        need(codec.retire_if_complete(),"Frame retired");
    }
    constexpr auto nps=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return {g.read(prepared.Get(),nps),g.read(out.Get(),nps),g.read(hdr.Get(),nps),g.read(sdr.Get(),nps)};
}
bool same(float a,float b){return (std::isnan(a)&&std::isnan(b))||std::memcmp(&a,&b,4)==0;}
}

int main(){try{
    Gpu g;
    // Working RGB from deep shadow to highlights; NR output anywhere in [0, 1.05].
    const auto inputs=[](bool half,bool poison){
        Bytes source(N*4*(half?2:4)),neural(source.size());std::uint32_t seed=0x5eed;
        const auto random=[&]{seed=seed*1664525u+1013904223u;return static_cast<float>(seed>>8)/static_cast<float>(1u<<24);};
        for(unsigned i=0;i<N;++i){
            for(unsigned ch=0;ch<3;++ch){put(source,i*4+ch,.0005f*std::pow(1.9f,static_cast<float>((i*3+ch)%19)),half);put(neural,i*4+ch,random()*1.05f,half);}
            put(source,i*4+3,static_cast<float>(i%5)/4,half);put(neural,i*4+3,0,half);
            if(poison)switch(i%7){
                case 0:put(source,i*4,-.1f,half);break;
                case 1:put(source,i*4+1,std::numeric_limits<float>::quiet_NaN(),half);break;
                case 2:put(source,i*4+2,std::numeric_limits<float>::infinity(),half);break;
                case 3:put(neural,i*4,std::numeric_limits<float>::quiet_NaN(),half);break;
                case 4:put(neural,i*4+1,-.25f,half);break;
                case 5:put(neural,i*4+2,std::numeric_limits<float>::infinity(),half);break;
                default:break;}
        }
        return std::pair{source,neural};
    };

    // ---- 1. off and a factor of 1 are the plain composite, byte for byte
    for(bool half:{false,true})for(bool guard:{false,true})for(bool split:{false,true}){
        const auto [source,neural]=inputs(half,false);
        const auto plain=composite(g,half,guard,split,{0.f},source,neural);
        const auto one=composite(g,half,guard,split,{1.f},source,neural);
        const auto back=composite(g,half,guard,split,{3.f,1.f},source,neural);
        need(one.prepared==plain.prepared&&one.composite==plain.composite,"A factor of 1 is the plain composite byte for byte");
        need(back.prepared==plain.prepared&&back.composite==plain.composite,"Back to 1 after an extrapolated frame is the plain composite again");
        const auto two=composite(g,half,guard,split,{2.f},source,neural);
        need(two.prepared==plain.prepared,"Extrapolation never changes what NR is given");
        need(two.composite!=plain.composite,"A factor of 2 changes the composite");
    }

    // ---- 2 and 3. factor k: the plain composite of saturate(C + k(N - C)), and
    // still the bounded ratio transfer. RGBA32F, so N' can be handed over exactly.
    double worst_structural=0,worst_reference=0;unsigned clamped_low=0,clamped_high=0,ratio_low=0,ratio_high=0;
    for(bool guard:{false,true})for(const float k:{2.f,3.7f,4.f}){
        const auto [source,neural]=inputs(false,false);
        const auto ext=composite(g,false,guard,false,{k},source,neural);
        Bytes amplified(neural.size());
        for(unsigned i=0;i<N;++i){
            for(unsigned ch=0;ch<3;++ch){const float c=get(ext.prepared,i*4+ch,false),n=get(neural,i*4+ch,false);
                const float e=std::clamp(c+k*(n-c),0.f,1.f);put(amplified,i*4+ch,e,false);
                clamped_low+=e==0.f&&c+k*(n-c)<0.f;clamped_high+=e==1.f&&c+k*(n-c)>1.f;}
            put(amplified,i*4+3,get(neural,i*4+3,false),false);
        }
        const auto ref=composite(g,false,guard,false,{0.f},source,amplified);
        need(ref.prepared==ext.prepared&&ref.hdr==ext.hdr&&ref.sdr==ext.sdr,"Same prepare either way");
        for(unsigned i=0;i<N;++i)for(unsigned ch=0;ch<4;++ch){
            const float a=get(ext.composite,i*4+ch,false),b=get(ref.composite,i*4+ch,false);
            const double d=std::abs(double(a)-double(b))/std::max(1.0,std::abs(double(b)));worst_structural=std::max(worst_structural,d);
            need(std::isfinite(a)&&d<=1e-5,"Factor k composites exactly saturate(C + k(N - C))");
            if(ch==3){need(same(a,get(source,i*4+3,false)),"Alpha kept");continue;}
            const float h=get(ext.hdr,i*4+ch,false),s=get(ext.sdr,i*4+ch,false),e=get(amplified,i*4+ch,false);
            const float raw_ratio=(decode(e)+1e-6f)/(s+1e-6f),r=std::clamp(raw_ratio,.01f,10.f);
            ratio_low+=raw_ratio<.01f;ratio_high+=raw_ratio>10.f;
            const double want=double(h)*r/exposure,rel=std::abs(double(a)-want)/std::max(1e-6,std::abs(want));
            worst_reference=std::max(worst_reference,rel);
            need(rel<=2e-3,"Still the bounded per-channel ratio transfer onto the saved working image");
        }
    }
    need(clamped_low>0&&clamped_high>0,"N' was clamped at 0 and at 1 somewhere");
    need(ratio_low>0&&ratio_high>0,"The ratio bounds .01 and 10 were both reached");

    // ---- 4. guard and split with extrapolation on
    for(bool half:{false,true}){
        const auto [source,neural]=inputs(half,true);
        const auto ext=composite(g,half,true,false,{4.f},source,neural);
        const auto split=composite(g,half,true,true,{4.f},source,neural);
        for(unsigned i=0;i<N;++i){
            const unsigned x=i%W,kind=i%7;
            for(unsigned ch=0;ch<3;++ch){const float v=get(ext.prepared,i*4+ch,half);
                need(std::isfinite(v)&&v>=0&&v<=1,"No invalid value reaches the model input");
                if(kind<3)need(v==0,"An invalid source gives a neutral model input");}
            for(unsigned ch=0;ch<4;++ch){const float a=get(ext.composite,i*4+ch,half),o=get(source,i*4+ch,half);
                if(kind<6)need(same(a,o),"An invalid source or model value passes the original pixel through, extrapolated or not");
                else need(std::isfinite(a),"A valid neighbour stays finite");
                const float b=get(split.composite,i*4+ch,half);
                if(x<W/2)need(same(b,o),"Split: the left half is the untouched original");
                else need(same(b,a),"Split: the right half is the extrapolated composite");}
        }
    }

    // ---- 5. the factor contract
    {   const auto [source,neural]=inputs(false,false);
        auto original=g.texture(DXGI_FORMAT_R32G32B32A32_FLOAT,false);
        std::vector<ComPtr<ID3D12Resource>> t;for(unsigned i=0;i<5;++i)t.push_back(g.texture(DXGI_FORMAT_R32G32B32A32_FLOAT,true));
        g.upload(original.Get(),source,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);g.run();
        lab::nr::PrePostColorCodec codec(g.device.Get(),{original.Get(),t[0].Get(),t[1].Get(),t[2].Get(),t[3].Get(),t[4].Get(),true,false,true});
        need(codec.extrapolation()==1.f,"A new codec composites plainly");
        for(const float bad:{0.f,.999f,-2.f,4.001f,8.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
            refused([&]{codec.set_extrapolation(bad);},"Factor outside finite 1..4 refused");
        need(codec.extrapolation()==1.f,"A refused factor changes nothing");
        codec.set_extrapolation(4.f);need(codec.extrapolation()==4.f,"Factor 4 accepted");
        lab::nr::PrePostColorConstants c{};c.width=W;c.height=H;c.pre_post=1;c.tonemap_operator=3;c.exposure=1;c.sigma=10;c.gamma=.454f;
        c.pre_matrix={{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};c.post_matrix=c.pre_matrix;
        codec.prepare(g.list.Get(),c);refused([&]{codec.set_extrapolation(2.f);},"No change inside a frame");
        codec.composite(g.list.Get());codec.bind_completion(g.fence.Get(),g.value+1);g.run();need(codec.retire_if_complete(),"Contract frame retired");
        codec.set_extrapolation(1.f);need(codec.extrapolation()==1.f,"Changed again between frames");
    }
    const auto errors=g.errors();
    for(const auto& e:errors)std::cerr<<"D3D12: "<<e<<'\n';
    need(errors.empty(),"No debug-layer error");
    std::cout<<"PASS "<<checks<<" edit extrapolation checks on WARP (structural max rel "<<worst_structural<<", ratio reference max rel "<<worst_reference
             <<"; N' clamped "<<clamped_low<<" low / "<<clamped_high<<" high); debug layer "<<(g.info?"on":"unavailable")<<"; no NR, no game\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
