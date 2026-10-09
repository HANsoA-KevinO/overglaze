// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The game's own exposure (Live ABI24), on WARP: no hardware GPU, no model.
//
//   1. every note has its own name;
//   2. the per-frame screen of an exposure texture: format, shape, declared
//      state, aliasing, pre-exposure and scale -- each a named note, never a
//      refusal of the frame;
//   3. the GPU reader returns texel (0,0)'s R channel for R32F, R16F and RGBA16F,
//      from NON_PIXEL_SHADER_RESOURCE, COMMON and UNORDERED_ACCESS, and leaves the
//      game's texture in its declared state (checked with the debug layer when
//      Graphics Tools are installed);
//   4. the reader's one-slot contract: refuses an unscreened texture, a second
//      recording, a retirement without one; a discarded recording is no reading.
#include "lab_exposure_meter.hpp"
#include "lab_game_exposure.hpp"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using lab::live::ExposureNote;
namespace {
int checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
void hr(HRESULT r,const char* what){if(FAILED(r))throw std::runtime_error(std::string(what)+" failed");}
template<class F> void refused(F f,const char* what){bool threw=false;try{f();}catch(const std::logic_error&){threw=true;}need(threw,what);}
// Exact for the values used here.
std::uint16_t half(float v){
    if(v==0)return 0;std::uint32_t b;std::memcpy(&b,&v,4);
    const std::uint32_t sign=(b>>16)&0x8000u;const int exponent=int((b>>23)&0xff)-127+15;const std::uint32_t mantissa=(b>>13)&0x3ffu;
    return static_cast<std::uint16_t>(sign|(std::uint32_t(exponent)<<10)|mantissa);
}
struct Gpu {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;ComPtr<ID3D12InfoQueue> info;
    UINT64 value=0;HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    std::vector<ComPtr<ID3D12Resource>> uploads;
    Gpu(){
        // The debug layer is optional: with it, a wrong barrier on the game's
        // texture is an error message; without it the values are still checked.
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
    ComPtr<ID3D12Resource> texture(unsigned w,unsigned h,DXGI_FORMAT f,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE,UINT16 array=1){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=w;d.Height=h;d.DepthOrArraySize=array;d.MipLevels=1;d.Format=f;d.SampleDesc.Count=1;d.Flags=flags;
        ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"texture");return r;
    }
    void barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        if(a==b)return;D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,0,a,b};list->ResourceBarrier(1,&x);
    }
    // One texel in; the texture goes from `before` to COPY_DEST and on to `after`.
    void set_texel(ID3D12Resource* t,const void* bytes,unsigned size,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        const auto d=t->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 total=0;device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&total);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=total;b.Height=1;
        b.DepthOrArraySize=1;b.MipLevels=1;b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> u;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&u)),"upload");
        void* mapped=nullptr;hr(u->Map(0,nullptr,&mapped),"map");std::memset(mapped,0,static_cast<std::size_t>(total));
        std::memcpy(static_cast<std::uint8_t*>(mapped)+fp.Offset,bytes,size);u->Unmap(0,nullptr);
        barrier(t,before,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=t;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=u.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(t,D3D12_RESOURCE_STATE_COPY_DEST,after);uploads.push_back(u);
    }
    std::vector<std::string> errors(){
        std::vector<std::string> out;if(!info)return out;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;hr(info->GetMessage(i,nullptr,&size),"message size");
            std::vector<std::uint8_t> b(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data());hr(info->GetMessage(i,m,&size),"message");
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)out.emplace_back(m->pDescription,m->DescriptionByteLength);}
        return out;
    }
};
}

int main(){try{
    // ---- 1. names
    std::set<std::string> names;
    for(unsigned i=0;i<lab::live::exposure_note_count;++i){const std::string n=lab::live::exposure_note_name(i);
        need(n!="unknown"&&names.insert(n).second,"Every exposure note has its own name: "+n);}
    need(std::string(lab::live::exposure_note_name(lab::live::exposure_note_count))=="unknown","Out-of-range notes are unknown");
    need(std::string(lab::live::exposure_source_name(0))=="manual"&&std::string(lab::live::exposure_source_name(1))=="meter"&&
         std::string(lab::live::exposure_source_name(2))=="game","Source names");

    Gpu g;
    constexpr auto nps=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // ---- 2. the per-frame screen
    using lab::live::inspect_exposure_texture;
    auto r32=g.texture(1,1,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COMMON);
    need(inspect_exposure_texture(nullptr,nps)==ExposureNote::no_texture,"No texture");
    for(unsigned state:{unsigned(nps),0u,unsigned(nps|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),unsigned(D3D12_RESOURCE_STATE_COPY_SOURCE)})
        need(inspect_exposure_texture(r32.Get(),state)==ExposureNote::none,"1x1 R32F in a readable or COMMON state is accepted ("+std::to_string(state)+")");
    for(unsigned state:{unsigned(D3D12_RESOURCE_STATE_RENDER_TARGET),unsigned(D3D12_RESOURCE_STATE_COPY_DEST),unsigned(D3D12_RESOURCE_STATE_DEPTH_WRITE),
                        unsigned(D3D12_RESOURCE_STATE_UNORDERED_ACCESS|nps)})
        need(inspect_exposure_texture(r32.Get(),state)==ExposureNote::unsupported_state,"A write or mixed state is a named note ("+std::to_string(state)+")");
    need(inspect_exposure_texture(r32.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS)==ExposureNote::unsupported_state,"UAV declared on a texture that cannot be one");
    auto uav16=g.texture(1,1,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    need(inspect_exposure_texture(uav16.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS)==ExposureNote::none,"UAV declared on a UAV-capable texture");
    auto r16=g.texture(1,1,DXGI_FORMAT_R16_FLOAT,D3D12_RESOURCE_STATE_COMMON);
    need(inspect_exposure_texture(r16.Get(),nps)==ExposureNote::none,"R16F accepted");
    for(auto format:{DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_UINT,DXGI_FORMAT_R11G11B10_FLOAT}){
        auto t=g.texture(1,1,format,D3D12_RESOURCE_STATE_COMMON);
        need(inspect_exposure_texture(t.Get(),nps)==ExposureNote::unsupported_format,"Format refused by name: "+std::to_string(unsigned(format)));}
    {auto t=g.texture(2,2,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COMMON);need(inspect_exposure_texture(t.Get(),nps)==ExposureNote::unsupported_shape,"2x2 is not the 1x1 contract");}
    {auto t=g.texture(1,1,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE,2);need(inspect_exposure_texture(t.Get(),nps)==ExposureNote::unsupported_shape,"An array is refused");}
    {   // The frame-level screen: a note, never a refusal; the frame keeps its roles.
        auto color=g.texture(8,8,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        lab::live::Frame f;f.color=color.Get();f.exposure=r32.Get();f.exposure_state=nps;f.exposure_note=0;f.pre_exposure=.5f;f.exposure_scale=2.f;
        auto a=f;lab::live::screen_exposure(a);need(a.exposure==r32.Get()&&a.exposure_note==0&&a.color==color.Get(),"A usable exposure is kept");
        a=f;a.exposure=color.Get();lab::live::screen_exposure(a);need(!a.exposure&&a.exposure_note==unsigned(ExposureNote::aliased)&&a.color==color.Get(),"Aliasing colour is a note");
        for(float bad:{0.f,-1.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
            a=f;a.pre_exposure=bad;lab::live::screen_exposure(a);need(!a.exposure&&a.exposure_note==unsigned(ExposureNote::invalid_scale),"Pre-exposure not finite and positive");
            a=f;a.exposure_scale=bad;lab::live::screen_exposure(a);need(!a.exposure&&a.exposure_note==unsigned(ExposureNote::invalid_scale),"Exposure scale not finite and positive");}
        a=f;a.exposure=nullptr;a.exposure_note=unsigned(ExposureNote::tag_other_thread);lab::live::screen_exposure(a);
        need(!a.exposure&&a.exposure_note==unsigned(ExposureNote::tag_other_thread),"A frame source's note is kept");
        a=f;a.exposure=nullptr;a.exposure_note=0;lab::live::screen_exposure(a);need(a.exposure_note==unsigned(ExposureNote::no_texture),"No texture without a note is named");
        a=f;a.exposure_note=999;lab::live::screen_exposure(a);need(!a.exposure&&a.exposure_note==unsigned(ExposureNote::tag_invalid),"An unknown note is not trusted");
        a=f;a.exposure_state=D3D12_RESOURCE_STATE_RENDER_TARGET;lab::live::screen_exposure(a);need(!a.exposure&&a.exposure_note==unsigned(ExposureNote::unsupported_state),"State screened");
        lab::live::Frame defaults;need(!defaults.exposure&&defaults.exposure_note==unsigned(ExposureNote::no_texture)&&defaults.pre_exposure==1.f&&defaults.exposure_scale==1.f,
            "A frame without exposure says so by default");
    }

    // ---- 3. the reader, per format and declared state
    lab::nr::GameExposureReader reader(g.device.Get());
    need(!reader.pending(),"Reader starts idle");
    refused([&]{reader.acknowledge_completion();},"No retirement without a recording");
    refused([&]{reader.discard_recording();},"No discard without a recording");
    {   const float v=.0038f; // Halo's measured DLSS.Pre.Exposure order of magnitude
        g.set_texel(r32.Get(),&v,4,D3D12_RESOURCE_STATE_COMMON,nps);
        reader.record(g.list.Get(),r32.Get(),nps);need(reader.pending(),"Recording marks the slot");
        refused([&]{reader.record(g.list.Get(),r32.Get(),nps);},"One slot only");
        g.run();const auto got=reader.acknowledge_completion();
        need(got.read&&got.value==v&&!reader.pending(),"R32F read back exactly from NON_PIXEL_SHADER_RESOURCE");}
    {   const auto h=half(.5f);
        g.set_texel(r16.Get(),&h,2,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON);
        reader.record(g.list.Get(),r16.Get(),D3D12_RESOURCE_STATE_COMMON);
        // The game's next use starts from COMMON again: a wrong restore would be
        // a debug-layer error on this barrier.
        g.barrier(r16.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);g.barrier(r16.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        g.run();const auto got=reader.acknowledge_completion();need(got.read&&got.value==.5f,"R16F read from COMMON, transitioned there and back");}
    {   const std::uint16_t texel[4]{half(2.f),half(9.f),half(9.f),half(9.f)};
        g.set_texel(uav16.Get(),texel,8,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        reader.record(g.list.Get(),uav16.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.barrier(uav16.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        g.run();const auto got=reader.acknowledge_completion();need(got.read&&got.value==2.f,"RGBA16F: the R channel, from UNORDERED_ACCESS");}
    {   const float v=-3.f; // read as given; finiteness and sign are the selector's verdict
        g.set_texel(r32.Get(),&v,4,nps,nps);g.run();
        reader.record(g.list.Get(),r32.Get(),nps);g.run();const auto got=reader.acknowledge_completion();need(got.read&&got.value==-3.f,"A value is reported as read");}
    // ---- 4. the slot contract
    {auto big=g.texture(2,2,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COMMON);
     refused([&]{reader.record(g.list.Get(),big.Get(),nps);},"An unscreened texture is refused before recording");
     refused([&]{reader.record(g.list.Get(),r32.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);},"An unscreened state is refused before recording");
     need(!reader.pending(),"A refusal records nothing");}
    reader.record(g.list.Get(),r32.Get(),nps);reader.discard_recording();
    need(!reader.pending(),"A discarded recording leaves the slot idle");
    refused([&]{reader.acknowledge_completion();},"A discarded recording has no reading");
    g.run(); // the discarded list still executes here harmlessly; nothing is read from it
    const auto errors=g.errors();
    for(const auto& e:errors)std::cerr<<"D3D12: "<<e<<'\n';
    need(errors.empty(),"No debug-layer error");
    std::cout<<"PASS "<<checks<<" game exposure checks on WARP; debug layer "<<(g.info?"on":"unavailable")<<"; no NR, no game\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
