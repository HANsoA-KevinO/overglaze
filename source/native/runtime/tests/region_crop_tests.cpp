// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Frame regions (Live ABI23, Hellblade 2), per pixel on WARP.
//
// Hellblade 2's 2.39:1 frame: output 5120x2142 inside a 5120x2144 texture,
// render 2970x1243 inside 2972x1256 depth, motion at display resolution
// 5120x2142. The runtime crops colour and depth to their regions and resamples
// motion onto the depth grid. Checked here, with small stand-ins:
//   1. motion resampled from a larger region picks the source pixel under each
//      guide pixel's centre, values unchanged;
//   2. a region equal to the guide extent is an exact crop;
//   3. the depth level-0 copy reads only the top-left region, and refuses a
//      larger source unless asked to crop;
//   4. the colour handoff copies only the region in and writes only the region
//      back -- the padding rows of the game's texture are never touched.
#include "lab_color_handoff.hpp"
#include "lab_depth_codec.hpp"
#include "lab_motion_codec.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
int checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
void hr(HRESULT r,const char* what){if(FAILED(r))throw std::runtime_error(std::string(what)+" failed");}
template<class F> void refused(F f,const char* what){bool threw=false;try{f();}catch(const std::exception&){threw=true;}need(threw,what);}
// Exact for the small integers used here.
std::uint16_t half(float v){
    if(v==0)return 0;std::uint32_t b;std::memcpy(&b,&v,4);
    const std::uint32_t sign=(b>>16)&0x8000u;const int exponent=int((b>>23)&0xff)-127+15;const std::uint32_t mantissa=(b>>13)&0x3ffu;
    return static_cast<std::uint16_t>(sign|(std::uint32_t(exponent)<<10)|mantissa);
}
float single(std::uint16_t h){
    if(!(h&0x7fff))return 0;const std::uint32_t sign=(h&0x8000u)<<16,exponent=((h>>10)&0x1f)-15+127,mantissa=(h&0x3ffu)<<13;
    const std::uint32_t b=sign|(exponent<<23)|mantissa;float v;std::memcpy(&v,&b,4);return v;
}
struct Gpu {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;UINT64 value=0;HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    Gpu(){
        ComPtr<IDXGIFactory4> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"factory");
        ComPtr<IDXGIAdapter> warp;hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"warp");
        hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
        D3D12_COMMAND_QUEUE_DESC qd{};hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
        hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    }
    ~Gpu(){CloseHandle(event);}
    // Close, submit, wait, reopen. `before_submit` gets the fence value this
    // submission will signal (the handoff wants it bound before submission).
    void run(const std::function<void(UINT64)>& before_submit={}){
        const UINT64 next=value+1;if(before_submit)before_submit(next);
        hr(list->Close(),"close");ID3D12CommandList* l[]{list.Get()};queue->ExecuteCommandLists(1,l);
        hr(queue->Signal(fence.Get(),++value),"signal");hr(fence->SetEventOnCompletion(value,event),"event");
        need(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU wait");
        hr(allocator->Reset(),"allocator reset");hr(list->Reset(allocator.Get(),nullptr),"list reset");
    }
    ComPtr<ID3D12Resource> texture(unsigned w,unsigned h,DXGI_FORMAT f,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=w;d.Height=h;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=f;d.SampleDesc.Count=1;d.Flags=flags;
        ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"texture");return r;
    }
    void barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        if(a==b)return;D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,0,a,b};list->ResourceBarrier(1,&x);
    }
    // Rows of bytes in, through an upload buffer; the texture ends in `after`.
    void upload(ID3D12Resource* t,const std::vector<std::uint8_t>& bytes,unsigned pixel_bytes,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        const auto d=t->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 total=0;device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&total);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=total;b.Height=1;
        b.DepthOrArraySize=1;b.MipLevels=1;b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&buffer)),"upload");
        std::uint8_t* p=nullptr;hr(buffer->Map(0,nullptr,reinterpret_cast<void**>(&p)),"map");
        for(UINT y=0;y<d.Height;++y)std::memcpy(p+fp.Offset+y*fp.Footprint.RowPitch,bytes.data()+size_t(y)*d.Width*pixel_bytes,size_t(d.Width)*pixel_bytes);
        buffer->Unmap(0,nullptr);
        barrier(t,before,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to{t,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}},from{buffer.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}};
        to.SubresourceIndex=0;from.PlacedFootprint=fp;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        barrier(t,D3D12_RESOURCE_STATE_COPY_DEST,after);run();
    }
    std::vector<std::uint8_t> download(ID3D12Resource* t,unsigned pixel_bytes,D3D12_RESOURCE_STATES state){
        const auto d=t->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 total=0;device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&total);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=total;b.Height=1;
        b.DepthOrArraySize=1;b.MipLevels=1;b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buffer)),"readback");
        barrier(t,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{buffer.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}},from{t,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
        to.PlacedFootprint=fp;from.SubresourceIndex=0;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        barrier(t,D3D12_RESOURCE_STATE_COPY_SOURCE,state);run();
        std::vector<std::uint8_t> out(size_t(d.Width)*d.Height*pixel_bytes);std::uint8_t* p=nullptr;hr(buffer->Map(0,nullptr,reinterpret_cast<void**>(&p)),"map");
        for(UINT y=0;y<d.Height;++y)std::memcpy(out.data()+size_t(y)*d.Width*pixel_bytes,p+fp.Offset+y*fp.Footprint.RowPitch,size_t(d.Width)*pixel_bytes);
        buffer->Unmap(0,nullptr);return out;
    }
};
// RG16F motion whose value at (x,y) is (x, 100+y): each pixel names itself.
std::vector<std::uint8_t> named_motion(unsigned w,unsigned h){
    std::vector<std::uint8_t> b(size_t(w)*h*4);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const std::uint16_t v[2]{half(float(x)),half(float(100+y))};std::memcpy(&b[(size_t(y)*w+x)*4],v,4);}
    return b;
}
void check_motion(const std::vector<std::uint8_t>& got,unsigned gw,unsigned gh,unsigned rw,unsigned rh,const char* what){
    for(unsigned y=0;y<gh;++y)for(unsigned x=0;x<gw;++x){
        std::uint16_t v[2];std::memcpy(v,&got[(size_t(y)*gw+x)*4],4);
        const unsigned sx=std::min(rw-1,(x*2+1)*rw/(2*gw)),sy=std::min(rh-1,(y*2+1)*rh/(2*gh));
        if(single(v[0])!=float(sx)||single(v[1])!=float(100+sy))
            throw std::runtime_error(std::string(what)+": guide ("+std::to_string(x)+","+std::to_string(y)+") got ("+std::to_string(single(v[0]))+","+
                std::to_string(single(v[1]))+") want source ("+std::to_string(sx)+","+std::to_string(sy)+")");
    }
    ++checks;
}
}

int main() try {
    Gpu g;constexpr auto npsr=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // 1. Display-resolution motion (a 20x9 region inside a 22x10 resource) onto a
    //    9x4 guide grid: the source pixel under each guide centre, unchanged.
    {auto motion=g.texture(22,10,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_COMMON);
     g.upload(motion.Get(),named_motion(22,10),4,D3D12_RESOURCE_STATE_COMMON,npsr);
     lab::nr::MotionCodec codec(g.device.Get(),9,4);
     codec.record(g.list.Get(),motion.Get(),20,9);g.run();codec.acknowledge_completion();
     check_motion(g.download(codec.output(),4,npsr),9,4,20,9,"display-resolution resample");
     // Refusals: a region larger than the resource; a pending slot.
     refused([&]{codec.validate(g.list.Get(),motion.Get(),23,9);},"a region larger than the motion resource is refused");
     codec.record(g.list.Get(),motion.Get(),20,9);
     refused([&]{codec.validate(g.list.Get(),motion.Get(),20,9);},"a pending slot is refused");
     g.run();codec.acknowledge_completion();}

    // 2. A region equal to the guide extent is an exact crop (a render subrect
    //    on the depth grid: 10x7 of a 12x9 resource).
    {auto motion=g.texture(12,9,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_COMMON);
     g.upload(motion.Get(),named_motion(12,9),4,D3D12_RESOURCE_STATE_COMMON,npsr);
     lab::nr::MotionCodec codec(g.device.Get(),10,7);
     codec.record(g.list.Get(),motion.Get(),10,7);g.run();codec.acknowledge_completion();
     const auto got=g.download(codec.output(),4,npsr);check_motion(got,10,7,10,7,"crop");
     for(unsigned y=0;y<7;++y)for(unsigned x=0;x<10;++x){std::uint16_t v[2];std::memcpy(v,&got[(size_t(y)*10+x)*4],4);
         need(single(v[0])==float(x)&&single(v[1])==float(100+y),"crop is the identity on the region");}}

    // 3. Depth: the level-0 copy of a 12x9 R32F depth into a 10x7 guide reads the
    //    top-left region only; without region_crop the larger source is refused.
    {auto depth=g.texture(12,9,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COMMON);
     std::vector<std::uint8_t> bytes(12*9*4);for(unsigned i=0;i<12*9;++i){const float v=float(i)/1000.f;std::memcpy(&bytes[i*4],&v,4);}
     g.upload(depth.Get(),bytes,4,D3D12_RESOURCE_STATE_COMMON,npsr);
     lab::nr::DepthCodec strict(g.device.Get(),10,7,nullptr,lab::nr::DepthCodecMode::hardware_mip0_copy);
     refused([&]{strict.validate(g.list.Get(),depth.Get(),{});},"a larger depth is refused without region_crop");
     lab::nr::DepthCodec codec(g.device.Get(),10,7,nullptr,lab::nr::DepthCodecMode::hardware_mip0_copy,true);
     codec.record(g.list.Get(),depth.Get(),{});g.run();codec.acknowledge_completion();
     const auto got=g.download(codec.output(),4,npsr);
     for(unsigned y=0;y<7;++y)for(unsigned x=0;x<10;++x){float v;std::memcpy(&v,&got[(size_t(y)*10+x)*4],4);
         need(v==float(y*12+x)/1000.f,"depth crop keeps the top-left region, pixel for pixel");}
     auto tiny=g.texture(9,7,DXGI_FORMAT_R32_FLOAT,npsr);
     refused([&]{codec.validate(g.list.Get(),tiny.Get(),{});},"a depth smaller than the region is refused even when cropping");}

    // 4. Colour: an 8x6 target whose image is its top 8x4 region. The handoff
    //    copies the region in and writes only the region back; rows 4 and 5 keep
    //    their own values.
    {constexpr unsigned tw=8,th=6,rw=8,rh=4;
     auto target=g.texture(tw,th,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
     auto snapshot=g.texture(rw,rh,DXGI_FORMAT_R16G16B16A16_FLOAT,npsr);
     auto composite=g.texture(rw,rh,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COMMON);
     auto pixels=[](unsigned w,unsigned h,float base){std::vector<std::uint8_t> b(size_t(w)*h*8);
         for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const std::uint16_t v[4]{half(base+x),half(base+y),half(base),half(1)};std::memcpy(&b[(size_t(y)*w+x)*8],v,8);}return b;};
     g.upload(target.Get(),pixels(tw,th,10),8,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
     g.upload(composite.Get(),pixels(rw,rh,500),8,D3D12_RESOURCE_STATE_COMMON,npsr);
     lab::nr::ColorHandoff strict(g.device.Get(),{snapshot.Get(),composite.Get(),true,false});
     refused([&]{strict.validate_snapshot(g.list.Get(),target.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,1,true);},"a larger target is refused without region_crop");
     lab::nr::ColorHandoff handoff(g.device.Get(),{snapshot.Get(),composite.Get(),true,true});
     handoff.snapshot(g.list.Get(),target.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,1,true);
     handoff.return_output(g.list.Get());
     g.run([&](UINT64 next){handoff.bind_completion(g.fence.Get(),next);});
     need(handoff.retire_if_complete(),"handoff retires after its fence");
     const auto in=g.download(snapshot.Get(),8,npsr),out=g.download(target.Get(),8,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
     const auto original=pixels(tw,th,10),written=pixels(rw,rh,500);
     for(unsigned y=0;y<rh;++y)need(std::memcmp(&in[size_t(y)*rw*8],&original[size_t(y)*tw*8],rw*8)==0,"snapshot is the target's region");
     for(unsigned y=0;y<rh;++y)need(std::memcmp(&out[size_t(y)*tw*8],&written[size_t(y)*rw*8],rw*8)==0,"the region receives the composite");
     for(unsigned y=rh;y<th;++y)need(std::memcmp(&out[size_t(y)*tw*8],&original[size_t(y)*tw*8],tw*8)==0,"padding rows outside the region are never written");
     auto tiny=g.texture(8,3,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
     refused([&]{handoff.validate_snapshot(g.list.Get(),tiny.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,2,true);},"a target smaller than the region is refused");}

    std::cout<<"PASS "<<checks<<" frame-region checks: display-resolution motion resampled onto the guide grid, "
               "exact crops of motion and depth, and a colour region written back with the padding untouched\n";
    return 0;
} catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
