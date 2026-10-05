// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// Bounded GPU helpers for the colour tests: one hardware device with the debug
// layer, padded row copies, upload/readback, a private fence, and FP16/FP32
// packing. Header-only and in an anonymous namespace, so each test executable
// gets its own copy.
//
// The device is the first hardware adapter (no software rasteriser) that
// creates a feature-level 12_0 D3D12 device. Without such an adapter, or
// without the D3D12 debug layer (Graphics Tools), TestGpu throws
// TestGpuUnavailable; test mains report it and exit with kTestGpuSkip, which
// CMake registers as SKIP_RETURN_CODE, so a machine without a GPU skips these
// tests instead of failing them.
#include "lab_platform.hpp"
#include "lab_preconvert_reference.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
using Bytes=std::vector<unsigned char>;
namespace p=lab::preconvert;
namespace {
constexpr int kTestGpuSkip=77;
struct TestGpuUnavailable:std::runtime_error{using std::runtime_error::runtime_error;};
constexpr unsigned width=37,height=23;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void hr(HRESULT v,const std::source_location at=std::source_location::current()){
    if(FAILED(v)){char code[16]{};sprintf_s(code,"0x%08lx",static_cast<unsigned long>(v));
        throw std::runtime_error(std::string("D3D12 operation failed ")+code+" at "+at.file_name()+":"+std::to_string(at.line()));}
}
template<class F> void rejects(F f){bool caught=false;try{f();}catch(const std::logic_error&){caught=true;}require(caught,"Invalid codec contract not rejected");}
void barrier(ID3D12GraphicsCommandList* cmd,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,0,a,b};cmd->ResourceBarrier(1,&v);}
float unpack(const Bytes& v,std::size_t i,bool half){if(half){std::uint16_t n;std::memcpy(&n,v.data()+i*2,2);return p::unhalf(n);}float n;std::memcpy(&n,v.data()+i*4,4);return n;}
void pack(Bytes& v,std::size_t i,float n,bool half){if(half){auto bits=p::half(n);std::memcpy(v.data()+i*2,&bits,2);}else std::memcpy(v.data()+i*4,&n,4);}
struct TestGpu {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12Fence> fence;ComPtr<ID3D12InfoQueue> info;
    std::uint64_t serial=0;DXGI_ADAPTER_DESC1 adapter{};lab::Handle event;
    TestGpu(){
        ComPtr<ID3D12Debug> debug;if(FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))throw TestGpuUnavailable("D3D12 debug layer unavailable (install Graphics Tools)");debug->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> selected;
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;const auto rv=factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate));if(rv==DXGI_ERROR_NOT_FOUND)break;hr(rv);
            DXGI_ADAPTER_DESC1 d{};hr(candidate->GetDesc1(&d));
            if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(candidate.Get(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),nullptr))){selected=candidate;adapter=d;break;}}
        if(!selected)throw TestGpuUnavailable("no hardware adapter with a feature-level 12_0 D3D12 device");
        hr(D3D12CreateDevice(selected.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));hr(device.As(&info));
        D3D12_COMMAND_QUEUE_DESC q{};hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
        hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)));hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event.valid(),"Event creation failed");
    }
    ComPtr<ID3D12Resource> create(const D3D12_RESOURCE_DESC& d,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){D3D12_HEAP_PROPERTIES h{};h.Type=type;ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;}
    ComPtr<ID3D12Resource> texture(DXGI_FORMAT format,bool uav){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
        return create(d,D3D12_HEAP_TYPE_DEFAULT,uav?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST);}
    struct Layout {D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};UINT rows=0;UINT64 row_bytes=0,size=0;};
    Layout layout(ID3D12Resource* r){Layout l;const auto d=r->GetDesc();device->GetCopyableFootprints(&d,0,1,0,&l.placed,&l.rows,&l.row_bytes,&l.size);require(l.size<1024*1024,"Test texture budget exceeded");return l;}
    ComPtr<ID3D12Resource> buffer(UINT64 size,D3D12_HEAP_TYPE type){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return create(d,type,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST);}
    void copy(ID3D12Resource* t,ID3D12Resource* b,const Layout& l,bool upload){D3D12_TEXTURE_COPY_LOCATION tr{};tr.pResource=t;tr.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION br{};br.pResource=b;br.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;br.PlacedFootprint=l.placed;
        if(upload)commands->CopyTextureRegion(&tr,0,0,0,&br,nullptr);else commands->CopyTextureRegion(&br,0,0,0,&tr,nullptr);}
    ComPtr<ID3D12Resource> upload(ID3D12Resource* t,const Bytes& bytes,D3D12_RESOURCE_STATES before){const auto l=layout(t);auto u=buffer(l.size,D3D12_HEAP_TYPE_UPLOAD);void* mapped=nullptr;D3D12_RANGE none{};hr(u->Map(0,&none,&mapped));std::memset(mapped,0xa5,static_cast<std::size_t>(l.size));
        for(unsigned y=0;y<height;++y)std::memcpy(static_cast<BYTE*>(mapped)+l.placed.Offset+y*l.placed.Footprint.RowPitch,bytes.data()+y*l.row_bytes,static_cast<std::size_t>(l.row_bytes));u->Unmap(0,nullptr);
        if(before!=D3D12_RESOURCE_STATE_COPY_DEST)barrier(commands.Get(),t,before,D3D12_RESOURCE_STATE_COPY_DEST);copy(t,u.Get(),l,true);barrier(commands.Get(),t,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);return u;}
    void execute(){hr(commands->Close());ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);++serial;
        if(FAILED(queue->Signal(fence.Get(),serial)) || FAILED(fence->SetEventOnCompletion(serial,event.value)) || WaitForSingleObject(event.value,10000)!=WAIT_OBJECT_0 || fence->GetCompletedValue()!=serial){std::cerr<<"Isolated GPU completion unknown\n";ExitProcess(4);}
        hr(device->GetDeviceRemovedReason());hr(allocator->Reset());hr(commands->Reset(allocator.Get(),nullptr));}
    Bytes read(ID3D12Resource* t,D3D12_RESOURCE_STATES state){const auto l=layout(t);auto b=buffer(l.size,D3D12_HEAP_TYPE_READBACK);barrier(commands.Get(),t,state,D3D12_RESOURCE_STATE_COPY_SOURCE);copy(t,b.Get(),l,false);barrier(commands.Get(),t,D3D12_RESOURCE_STATE_COPY_SOURCE,state);execute();
        void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(l.size)};hr(b->Map(0,&range,&mapped));Bytes result(static_cast<std::size_t>(l.row_bytes)*height);
        for(unsigned y=0;y<height;++y)std::memcpy(result.data()+y*l.row_bytes,static_cast<BYTE*>(mapped)+l.placed.Offset+y*l.placed.Footprint.RowPitch,static_cast<std::size_t>(l.row_bytes));D3D12_RANGE none{};b->Unmap(0,&none);return result;}
    lab::json errors(){lab::json errors=lab::json::array();for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;hr(info->GetMessage(i,nullptr,&size));Bytes b(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data());hr(info->GetMessage(i,m,&size));if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)errors.push_back(std::string(m->pDescription,m->DescriptionByteLength));}return errors;}
};
}
