// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The late panel's present queue, executed on a real GPU.
//
// A late-loaded controller adopts a swapchain it never saw created. On RE9
// that chain answers GetDevice(ID3D12CommandQueue) with
// E_NOINTERFACE -- and so does the plain swapchain below -- so the queue comes
// from evidence: the queue that executes the game's transition of the current
// back buffer into PRESENT, frame after frame, with no other DIRECT queue
// waiting in between.
//
// The shapes that must decide run on a real swapchain and really present. The
// shapes that must NOT decide put a back buffer on a second DIRECT queue, which
// on a real swapchain removed the device (DXGI_ERROR_ACCESS_DENIED, observed
// while writing this file), so they run on ordinary textures standing in for
// the back buffers: the witness is handed their identities exactly as it is
// handed a swapchain's, and every barrier, submission and wait is real.
// Our own window, swapchain and queues; no game.
#include "lab_present_queue_witness.hpp"
#include "lab_overlay.hpp"
#include <d3d12sdklayers.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <future>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Witness=lab::PresentQueueWitness;
namespace {
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
void hr(HRESULT r,const char* what){if(FAILED(r))throw std::runtime_error(std::string(what)+" HRESULT="+std::to_string(static_cast<unsigned long>(r)));}

struct Gpu {
    HWND window=nullptr;
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> present,other,compute;
    ComPtr<IDXGISwapChain3> chain;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    std::array<ComPtr<ID3D12Resource>,3> stand_ins;unsigned stand_in_frame=0;
    ComPtr<ID3D12DescriptorHeap> rtv;
    ComPtr<ID3D12Fence> fence;UINT64 value=0;HANDLE event=nullptr;bool enhanced=false;
    Gpu(){
        WNDCLASSEXW c{sizeof(c)};c.lpfnWndProc=DefWindowProcW;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"OverglazeWitnessFixture";
        RegisterClassExW(&c);
        window=CreateWindowExW(0,c.lpszClassName,L"witness fixture",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,c.hInstance,nullptr);
        need(window!=nullptr,"fixture window");
        ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> adapter;
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;
            if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate))==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 d{};hr(candidate->GetDesc1(&d),"GetDesc1");
            if(d.VendorId==0x10de&&!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter=candidate;break;}}
        need(adapter!=nullptr,"NVIDIA adapter");
        hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"CreateDevice");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&present)),"present queue");
        hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&other)),"second direct queue");
        q.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&compute)),"compute queue");
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=64;desc.Height=64;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=3;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> one;hr(factory->CreateSwapChainForHwnd(present.Get(),window,&desc,nullptr,nullptr,&one),"CreateSwapChainForHwnd");
        hr(one.As(&chain),"IDXGISwapChain3");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
        hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");hr(list->Close(),"Close");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"fixture RTV");
        // Ordinary render targets in COMMON (== PRESENT): legal on any queue.
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=64;rd.Height=64;rd.DepthOrArraySize=1;rd.MipLevels=1;
        rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.SampleDesc.Count=1;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for(auto& r:stand_ins)hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)),"stand-in back buffer");
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);need(event!=nullptr,"event");
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 o{};
        enhanced=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,&o,sizeof(o)))&&o.EnhancedBarriersSupported;
    }
    ~Gpu(){if(event)CloseHandle(event);if(window)DestroyWindow(window);}
    void drain(ID3D12CommandQueue* q){hr(q->Signal(fence.Get(),++value),"Signal");
        if(fence->GetCompletedValue()<value){hr(fence->SetEventOnCompletion(value,event),"SetEventOnCompletion");need(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"GPU drain");}}
    void idle(){drain(present.Get());drain(other.Get());drain(compute.Get());}
    Witness::BackBuffers stand_in_names()const{Witness::BackBuffers b;
        for(unsigned i=0;i<stand_ins.size();++i)b.identity[i]=stand_ins[i].Get();b.count=3;b.current=stand_in_frame%3;b.named=true;return b;}
};

enum class Transition{none,legacy,enhanced};
enum class Sync{cpu,foreign_direct_wait,compute_wait,unrelated_direct_wait};

// Record buffer -> RENDER_TARGET -> PRESENT on `on` and submit it there.
void transition(Gpu& g,ID3D12Resource* buffer,ID3D12CommandQueue* on,Transition kind,bool clear=false,ID3D12Resource* implicit_readback=nullptr){
    hr(g.allocator->Reset(),"allocator Reset");hr(g.list->Reset(g.allocator.Get(),nullptr),"list Reset");
    if(kind==Transition::legacy){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=buffer;
        b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;g.list->ResourceBarrier(1,&b);
        if(clear){auto view=g.rtv->GetCPUDescriptorHandleForHeapStart();g.device->CreateRenderTargetView(buffer,nullptr,view);
            const float color[]{.2f,.3f,.4f,1};g.list->ClearRenderTargetView(view,color,0,nullptr);}
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);g.list->ResourceBarrier(1,&b);
        if(implicit_readback){
            // A read after the last explicit COMMON/PRESENT transition relies
            // on decay at the ORIGINAL Execute boundary. Preserve that boundary.
            D3D12_TEXTURE_COPY_LOCATION from{buffer,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            D3D12_TEXTURE_COPY_LOCATION to{implicit_readback,D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            to.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,1,1,1,256};const D3D12_BOX box{64,80,0,65,81,1};
            g.list->CopyTextureRegion(&to,0,0,0,&from,&box);
        }
    }else if(kind==Transition::enhanced){
        ComPtr<ID3D12GraphicsCommandList7> seven;hr(g.list.As(&seven),"ID3D12GraphicsCommandList7");
        D3D12_TEXTURE_BARRIER t{};t.pResource=buffer;t.Subresources.IndexOrFirstMipLevel=0xffffffff;
        t.SyncBefore=D3D12_BARRIER_SYNC_NONE;t.SyncAfter=D3D12_BARRIER_SYNC_RENDER_TARGET;
        t.AccessBefore=D3D12_BARRIER_ACCESS_NO_ACCESS;t.AccessAfter=D3D12_BARRIER_ACCESS_RENDER_TARGET;
        t.LayoutBefore=D3D12_BARRIER_LAYOUT_PRESENT;t.LayoutAfter=D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        D3D12_BARRIER_GROUP group{};group.Type=D3D12_BARRIER_TYPE_TEXTURE;group.NumBarriers=1;group.pTextureBarriers=&t;seven->Barrier(1,&group);
        std::swap(t.SyncBefore,t.SyncAfter);std::swap(t.AccessBefore,t.AccessAfter);std::swap(t.LayoutBefore,t.LayoutAfter);seven->Barrier(1,&group);
    }
    hr(g.list->Close(),"Close");
    ID3D12CommandList* lists[]{g.list.Get()};on->ExecuteCommandLists(1,lists);
}
void sync_after(Gpu& g,ID3D12CommandQueue* on,Sync sync){
    if(sync==Sync::foreign_direct_wait){
        // Transitioned on `on`, and a different DIRECT queue made to wait for it
        // on the GPU before the present: "transitioned on Q, presented on P".
        hr(on->Signal(g.fence.Get(),++g.value),"Signal");hr(g.present->Wait(g.fence.Get(),g.value),"Wait");
    }else if(sync==Sync::unrelated_direct_wait){
        hr(on->Signal(g.fence.Get(),++g.value),"Signal");hr(g.other->Wait(g.fence.Get(),g.value),"unrelated DIRECT Wait");
    }else if(sync==Sync::compute_wait){
        // An async-compute queue waiting on graphics is not a present queue.
        hr(on->Signal(g.fence.Get(),++g.value),"Signal");hr(g.compute->Wait(g.fence.Get(),g.value),"compute Wait");
    }else g.drain(on);
}
// One real frame on the swapchain; `w` is fed where the controller host feeds it.
ComPtr<ID3D12CommandQueue> frame(Gpu& g,Witness* w,Transition kind,Sync sync=Sync::cpu){
    ComPtr<ID3D12Resource> buffer;hr(g.chain->GetBuffer(g.chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)),"GetBuffer");
    transition(g,buffer.Get(),g.present.Get(),kind);sync_after(g,g.present.Get(),sync);
    const auto permit=w?w->present(g.chain.Get()):ComPtr<ID3D12CommandQueue>{};
    const HRESULT presented=g.chain->Present(0,0);
    if(FAILED(presented))throw std::runtime_error("Present HRESULT="+std::to_string(static_cast<unsigned long>(presented))+
        " removed reason="+std::to_string(static_cast<unsigned long>(g.device->GetDeviceRemovedReason())));
    g.idle();return permit;
}
// One stand-in frame: the current stand-in is transitioned on each queue in
// `on` (in order), then the witness judges it; nothing is presented.
ComPtr<ID3D12CommandQueue> stand_in(Gpu& g,Witness& w,std::initializer_list<ID3D12CommandQueue*> on,Sync sync=Sync::cpu){
    auto* buffer=g.stand_ins[g.stand_in_frame%3].Get();
    for(auto* q:on){transition(g,buffer,q,Transition::legacy);sync_after(g,q,sync);if(sync!=Sync::cpu)g.idle();}
    const auto permit=w.present(g.stand_in_names());
    g.idle();++g.stand_in_frame;return permit;
}
// A separate private fence, never used by drain()/idle(). Model a client
// waiting for an already completed stage before the final back-buffer write.
enum class OldWaitCase {plain,cpu_before,cpu_after_wait,cpu_v1_after_wait,rewind,multiple_producers,current_transition,future_signal};
ComPtr<ID3D12CommandQueue> old_wait_frame(Gpu& g,Witness& w,ID3D12Fence* fence,UINT64& value,OldWaitCase kind=OldWaitCase::plain){
    value+=4;hr(g.present->Signal(fence,value),"old stage Signal");
    if(kind==OldWaitCase::rewind){hr(g.present->Signal(fence,value-1),"rewind Signal");hr(g.present->Signal(fence,value+1),"newer Signal");++value;}
    if(kind==OldWaitCase::multiple_producers){g.drain(g.present.Get());hr(g.other->Signal(fence,++value),"other producer Signal");g.drain(g.other.Get());hr(g.present->Signal(fence,++value),"first producer again");}
    g.drain(g.present.Get());
    if(kind==OldWaitCase::cpu_before)hr(fence->Signal(value),"CPU Signal before transition");
    ComPtr<ID3D12Resource> buffer;hr(g.chain->GetBuffer(g.chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)),"GetBuffer");
    transition(g,buffer.Get(),g.present.Get(),Transition::legacy);
    if(kind==OldWaitCase::current_transition){hr(g.present->Signal(fence,++value),"current transition Signal");g.drain(g.present.Get());}
    if(kind==OldWaitCase::future_signal)++value;
    hr(g.other->Wait(fence,value),"foreign DIRECT Wait");
    if(kind==OldWaitCase::future_signal){hr(g.present->Signal(fence,value),"future Signal after Wait");g.drain(g.present.Get());}
    if(kind==OldWaitCase::cpu_v1_after_wait){ComPtr<ID3D12Fence1> alias;hr(fence->QueryInterface(IID_PPV_ARGS(&alias)),"Fence1 alias");hr(alias->Signal(value),"CPU Signal through Fence1");}
    if(kind==OldWaitCase::cpu_after_wait)hr(fence->Signal(value),"CPU Signal after Wait");
    const auto permit=w.present(g.chain.Get());
    hr(g.chain->Present(0,0),"Present with old-completion wait");g.idle();return permit;
}
std::array<unsigned char,4> pixel(Gpu& g,ID3D12Resource* buffer,unsigned x,unsigned y){
    Witness::OwnWork own;g.drain(g.present.Get());
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC rd{};
    rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=256;rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> out;hr(g.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out)),"pixel readback");
    hr(g.allocator->Reset(),"pixel allocator Reset");hr(g.list->Reset(g.allocator.Get(),nullptr),"pixel list Reset");
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={buffer,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE};
    g.list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION from{buffer,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};D3D12_TEXTURE_COPY_LOCATION to{out.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,1,1,1,256};const D3D12_BOX box{x,y,0,x+1,y+1,1};g.list->CopyTextureRegion(&to,0,0,0,&from,&box);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);g.list->ResourceBarrier(1,&barrier);hr(g.list->Close(),"pixel Close");
    ID3D12CommandList* commands[]{g.list.Get()};g.present->ExecuteCommandLists(1,commands);g.drain(g.present.Get());
    void* data=nullptr;D3D12_RANGE read{0,4};hr(out->Map(0,&read,&data),"pixel Map");std::array<unsigned char,4> result{};std::memcpy(result.data(),data,4);
    D3D12_RANGE none{};out->Unmap(0,&none);return result;
}
std::uint64_t miss(const lab::json& s,const char* why){return s["misses"].value(why,0ULL);}
}

int wmain(){
    try{
        ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"debug layer");debug->EnableDebugLayer();
        Gpu g;ComPtr<ID3D12InfoQueue> info;hr(g.device.As(&info),"debug messages");
        // The fact the witness exists for, recorded rather than assumed.
        ComPtr<ID3D12CommandQueue> reported;const HRESULT asked=g.chain->GetDevice(IID_PPV_ARGS(&reported));
        std::cout<<"GetDevice(ID3D12CommandQueue) on a real D3D12 swapchain: HRESULT=0x"<<std::hex<<static_cast<unsigned long>(asked)<<std::dec
                 <<(reported?(reported.Get()==g.present.Get()?" (the creation queue)":" (another object)"):" (no queue)")<<'\n';
        constexpr unsigned streak=5;
        // 0. The frames are valid with no hook installed at all.
        for(unsigned i=0;i<3;++i){frame(g,nullptr,Transition::none);frame(g,nullptr,Transition::legacy);}
        if(g.enhanced)frame(g,nullptr,Transition::enhanced);
        need(g.device->GetDeviceRemovedReason()==S_OK,"Baseline frames leave the device intact");

        // 1. A real swapchain transitioned on its own queue every frame: decided,
        //    and it is that queue.
        {auto* w=Witness::install(g.device.Get(),{streak});
         frame(g,w,Transition::legacy);
         need(!w->decided(),"The first present only names the back buffers");
         need(miss(w->status(),"first-present-names-the-back-buffers")==1,"First present recorded as such: "+w->status().dump());
         for(unsigned i=1;i<streak;++i){frame(g,w,Transition::legacy);need(!w->decided(),"No decision before the streak");}
         frame(g,w,Transition::legacy);
         need(w->decided().Get()==g.present.Get(),"Decided the queue that executed the transitions: "+w->status().dump());
         need(w->status()["marks"].value("legacy",0ULL)>=streak,"Legacy barriers were the evidence");
         need(w->contradictions()==0,"No contradiction on a consistent chain");
         for(unsigned i=0;i<8;++i){
             need(!frame(g,w,Transition::legacy,Sync::unrelated_direct_wait),"A foreign DIRECT wait denies this frame's draw permit");
             need(!w->contradicted()&&w->decided().Get()==g.present.Get(),"An ambiguous wait is not proof of a queue change");
             need(frame(g,w,Transition::legacy).Get()==g.present.Get(),"A clean current frame resumes without a restart");
         }
         need(w->status().value("redecisions",99ULL)==0,"Ambiguous waits do not consume the re-decision budget");
         need(!frame(g,w,Transition::none),"A prior decision does not permit drawing without a current transition");
         for(unsigned i=0;i<6;++i){Witness::OwnWork own_work;
             need(!frame(g,w,Transition::legacy),"The panel's own transitions must not become game evidence");}
         need(frame(g,w,Transition::legacy).Get()==g.present.Get(),"Game evidence resumes after own-work suppression");
         w->uninstall_for_test();}

        // 2. Async compute waiting on graphics between the transition and Present
        //    does not block the decision: a compute queue cannot present.
        {auto* w=Witness::install(g.device.Get(),{streak});
         for(unsigned i=0;i<=streak;++i)frame(g,w,Transition::legacy,Sync::compute_wait);
         need(w->decided().Get()==g.present.Get(),"Compute-queue waits are not present evidence: "+w->status().dump());
         w->uninstall_for_test();}

        // 3. No transition at all (a buffer presented untouched): nothing to go on.
        {auto* w=Witness::install(g.device.Get(),{streak});
         for(unsigned i=0;i<4*streak;++i)frame(g,w,Transition::none);
         const auto s=w->status();
         need(!w->decided(),"No transition, no decision: "+s.dump());
         need(miss(s,"no-present-transition-for-current-buffer")>=3*streak,"Refused for the named reason: "+s.dump());
         w->uninstall_for_test();}

        // 4. Enhanced barriers (ID3D12GraphicsCommandList7::Barrier) are the same evidence.
        std::string enhanced="not supported by this device";
        if(g.enhanced){auto* w=Witness::install(g.device.Get(),{streak});
         need(w->status().value("enhanced_barriers",std::string())=="hooked","Enhanced barriers hooked: "+w->status().dump());
         for(unsigned i=0;i<=streak;++i)frame(g,w,Transition::enhanced);
         const auto s=w->status();
         need(w->decided().Get()==g.present.Get(),"Enhanced-barrier transitions decide the same queue: "+s.dump());
         need(s["marks"].value("enhanced",0ULL)>=streak&&s["marks"].value("legacy",0ULL)==0,"Evidence came from Barrier(), not ResourceBarrier(): "+s.dump());
         w->uninstall_for_test();enhanced="decided";}

        // ---- stand-in back buffers: the shapes that put a buffer on a second queue.
        // 5. The stand-ins decide exactly like the chain did (the control case).
        {auto* w=Witness::install(g.device.Get(),{streak});
         for(unsigned i=0;i<=streak;++i)stand_in(g,*w,{g.present.Get()});
         need(w->decided().Get()==g.present.Get(),"Stand-ins transitioned on one queue decide it: "+w->status().dump());
         // 5b. After the decision, a frame whose transition ran on another queue
         //     is a contradiction, counted, and the decision is not replaced.
         stand_in(g,*w,{g.other.Get()});
         need(w->contradictions()==1,"A transition on another queue contradicts the decision: "+w->status().dump());
         need(w->decided().Get()==g.present.Get(),"The decision itself is not silently replaced");
         need(!stand_in(g,*w,{g.present.Get()}),"A contradicted decision cannot grant permits until explicitly restarted");
         w->uninstall_for_test();}

        // 6. Transitioned on one DIRECT queue, another DIRECT queue made to wait for
        //    it on the GPU before the present: never decided.
        {auto* w=Witness::install(g.device.Get(),{streak});
         for(unsigned i=0;i<4*streak;++i)stand_in(g,*w,{g.other.Get()},Sync::foreign_direct_wait);
         const auto s=w->status();
         need(!w->decided(),"A cross-queue present shape is refused: "+s.dump());
         need(miss(s,"other-direct-queue-waited-before-present")>=3*streak,"Refused for the named reason: "+s.dump());
         w->uninstall_for_test();}

        // 7. Two queues taking turns: no run ever reaches the streak.
        {auto* w=Witness::install(g.device.Get(),{streak});
         for(unsigned i=0;i<4*streak;++i)stand_in(g,*w,{i%2?g.other.Get():g.present.Get()});
         const auto s=w->status();
         need(!w->decided(),"Alternating queues are never decided: "+s.dump());
         need(s.value("queue_changes",0ULL)>=3*streak,"Each change of queue is counted: "+s.dump());
         w->uninstall_for_test();}

        // 8. Both queues transition the same buffer before one present.
        {auto* w=Witness::install(g.device.Get(),{streak});
         for(unsigned i=0;i<4*streak;++i)stand_in(g,*w,{g.present.Get(),g.other.Get()});
         const auto s=w->status();
         need(!w->decided(),"A buffer transitioned by two queues is never decided: "+s.dump());
         need(miss(s,"two-queues-transitioned-the-buffer")>=3*streak,"Refused for the named reason: "+s.dump());
         w->uninstall_for_test();}

        // 9. Re-decision (RE9: frame generation switched on in game
        //    moved presents off the decided queue and the panel was withdrawn for
        //    good). Decided Q1; the transitions move to Q2; the decision is
        //    contradicted, not replaced; the caller restarts; only a fresh FULL
        //    run under the same rules decides Q2.
        {Witness::Policy policy{streak};policy.restarts=2;policy.restart_window_ms=1000;
         auto* w=Witness::install(g.device.Get(),policy);
         for(unsigned i=0;i<=streak;++i)stand_in(g,*w,{g.present.Get()});
         need(w->decided().Get()==g.present.Get()&&!w->contradicted(),"Decided Q1: "+w->status().dump());
         stand_in(g,*w,{g.other.Get()});
         need(w->contradicted()&&w->decided().Get()==g.present.Get(),"Q2's transition contradicts Q1 and does not replace it: "+w->status().dump());
         need(w->restart(1000),"The first re-decision is allowed");
         need(!w->decided()&&!w->contradicted()&&w->status().value("streak",99u)==0,"A restart drops the decision and the run: "+w->status().dump());
         for(unsigned i=1;i<streak;++i){stand_in(g,*w,{g.other.Get()});need(!w->decided(),"No re-decision before a full fresh run: "+w->status().dump());}
         stand_in(g,*w,{g.other.Get()});
         need(w->decided().Get()==g.other.Get()&&!w->contradicted(),"A fresh run under the same rules decides Q2: "+w->status().dump());
         // The same strict rules after a restart: a cross-queue shape still refuses.
         stand_in(g,*w,{g.present.Get()});need(w->contradicted(),"Q1 now contradicts the Q2 decision");
         need(w->restart(1500),"The second re-decision within the window is allowed");
         for(unsigned i=0;i<2*streak;++i)stand_in(g,*w,{g.other.Get()},Sync::foreign_direct_wait);
         need(!w->decided()&&miss(w->status(),"other-direct-queue-waited-before-present")>=streak,"After a restart the cross-queue shape is still refused: "+w->status().dump());
         // 9b. The bound: `restarts` within the window, then refused; the window slides.
         need(!w->restart(1600),"A third restart within the window is refused: "+w->status().dump());
         need(w->status().value("refused_redecisions",0ULL)==1&&w->status().value("redecisions",0ULL)==2,"Refusal and re-decisions counted: "+w->status().dump());
         need(w->restart(2100),"Once the window has slid past the first restart, another is allowed");
         // 10. A different followed chain: rebind() drops everything, the next
         //     present names the buffers again, and only a full run decides.
         w->rebind();
         stand_in(g,*w,{g.present.Get()});
         need(!w->decided()&&miss(w->status(),"first-present-names-the-back-buffers")==2,"A rebind names the back buffers again: "+w->status().dump());
         for(unsigned i=1;i<streak;++i){stand_in(g,*w,{g.present.Get()});need(!w->decided(),"No decision before the streak after a rebind");}
         stand_in(g,*w,{g.present.Get()});
         const auto s=w->status();
         need(w->decided().Get()==g.present.Get()&&s.value("rebinds",0ULL)==1&&s.value("decisions",0ULL)==3,"Decided again after the rebind: "+s.dump());
         w->uninstall_for_test();}

        // 11. A private monotonic single-producer point completed BEFORE the
        // final transition cannot carry that transition onto the waiting queue.
        {auto* w=Witness::install(g.device.Get(),{streak});ComPtr<ID3D12Fence> f;
         hr(g.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)),"old-completion fence");UINT64 value=0;
         for(unsigned i=0;i<=streak;++i)old_wait_frame(g,*w,f.Get(),value);
         need(w->decided().Get()==g.present.Get(),"Old completed points no longer prevent a queue decision: "+w->status().dump());
         need(old_wait_frame(g,*w,f.Get(),value).Get()==g.present.Get(),"An old completed wait permits this actual present");
         need(w->status()["wait_provenance"].value("ignored_old_completed_waits",0ULL)>=streak,"Excluded waits have bounded provenance diagnostics");
         need(!w->contradicted(),"Excluding a proven old point is not a queue contradiction");w->uninstall_for_test();}
        // 12. Never exempt a current-transition signal, a CPU-written fence
        // (including one changed AFTER the Wait snapshot), a rewind or a fence
        // with multiple producers. Even already-completed waits stay refused.
        for(auto kind:{OldWaitCase::current_transition,OldWaitCase::future_signal,OldWaitCase::cpu_before,OldWaitCase::cpu_after_wait,OldWaitCase::cpu_v1_after_wait,OldWaitCase::rewind,OldWaitCase::multiple_producers}){
         auto* w=Witness::install(g.device.Get(),{streak});ComPtr<ID3D12Fence> f;
         hr(g.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)),"ambiguous fence");UINT64 value=0;
         for(unsigned i=0;i<3*streak;++i)need(!old_wait_frame(g,*w,f.Get(),value,kind),"Unsafe/unknown provenance grants no frame permit");
         need(!w->decided()&&miss(w->status(),"other-direct-queue-waited-before-present")>=streak,"Unsafe/unknown provenance still prevents a decision: "+w->status().dump());
         need(w->status()["wait_provenance"].value("ignored_old_completed_waits",99ULL)==0,"No unknown fence point is exempted");w->uninstall_for_test();}
        // CPU enqueue order without GPU completion is never enough. Hold Q on
        // a separate gate so its old signal is still incomplete when P waits.
        {auto* w=Witness::install(g.device.Get(),{streak});ComPtr<ID3D12Fence> f;
         hr(g.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)),"pending old-point fence");UINT64 value=0;
         for(unsigned i=0;i<3*streak;++i){ComPtr<ID3D12Fence> gate;
             hr(g.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"GPU completion gate");
             hr(g.present->Wait(gate.Get(),1),"hold Q before old Signal");hr(g.present->Signal(f.Get(),++value),"pending old Signal");
             ComPtr<ID3D12Resource> buffer;hr(g.chain->GetBuffer(g.chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)),"GetBuffer");
             transition(g,buffer.Get(),g.present.Get(),Transition::legacy);hr(g.other->Wait(f.Get(),value),"P waits for pending old point");
             const auto permit=w->present(g.chain.Get());
             hr(gate->Signal(1),"release GPU gate");hr(g.chain->Present(0,0),"Present after pending-point check");g.idle();
             need(!permit,"An earlier enqueue without observed GPU completion cannot exempt a foreign wait");}
         need(!w->decided(),"CPU ordering alone never decides the queue");w->uninstall_for_test();}
        // Shared fences may have writers outside these hooks; no exclusion.
        {auto* w=Witness::install(g.device.Get(),{streak});ComPtr<ID3D12Fence> f;
         hr(g.device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&f)),"shared fence");UINT64 value=0;
         for(unsigned i=0;i<3*streak;++i)need(!old_wait_frame(g,*w,f.Get(),value),"Shared fence cannot grant an old-point exemption");
         need(!w->decided(),"Shared-fence writers are not assumed observable");w->uninstall_for_test();}
        // Reaching the fixed metadata cap never turns missing history into proof.
        {auto* w=Witness::install(g.device.Get(),{streak});w->present(g.chain.Get());
         std::vector<ComPtr<ID3D12Fence>> fences;
         for(unsigned i=0;i<65;++i){ComPtr<ID3D12Fence> f;hr(g.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)),"capacity fence");
             hr(g.present->Signal(f.Get(),1),"capacity Signal");fences.push_back(f);}
         UINT64 value=1;for(unsigned i=0;i<3*streak;++i)need(!old_wait_frame(g,*w,fences.back().Get(),value),"Fence metadata overflow defers drawing");
         const auto p=w->status()["wait_provenance"];
         need(p.value("tracked_fences",0u)==p.value("fence_capacity",999u)&&p.value("capacity_misses",0ULL)>0&&!w->decided(),"Fixed capacity remains conservative: "+p.dump());
         w->uninstall_for_test();}

        // 13. Same-batch panel rendering: a foreign DIRECT wait follows every
        // original game submission. The legacy Present proof still refuses it,
        // but that signal/wait now orders the panel already in the same batch.
        hr(g.chain->ResizeBuffers(3,1280,800,DXGI_FORMAT_R8G8B8A8_UNORM,0),"batch fixture Resize");
        {wchar_t self[32768]{};need(GetModuleFileNameW(nullptr,self,32768)>0,"fixture executable path");
         const auto router_path=std::filesystem::path(self).parent_path()/L"lab_witness_router_fixture.dll";
         const auto router=LoadLibraryExW(router_path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
         need(router!=nullptr,"Load the test-only second hook DLL");
         const auto start=reinterpret_cast<BOOL(*)(ID3D12CommandQueue*)>(GetProcAddress(router,"WitnessRouterStart"));
         const auto observed=reinterpret_cast<BOOL(*)()>(GetProcAddress(router,"WitnessRouterObserved"));
         const auto stop=reinterpret_cast<void(*)()>(GetProcAddress(router,"WitnessRouterStop"));
         need(start&&observed&&stop&&start(g.present.Get()),"Real bridge-style binding/Reset/Execute hooks install first in a separate DLL");
         auto* w=Witness::install(g.device.Get(),{streak});w->present(g.chain.Get());
         lab::Controller controller;lab::GameOverlay panel(controller);panel.attach(g.chain.Get(),g.present.Get());panel.use_batch_submission(true);
         ComPtr<ID3D12Fence> same_queue_point,other_queue_point;hr(g.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&same_queue_point)),"same-queue marker");
         hr(g.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&other_queue_point)),"other-queue marker");
         std::promise<void> same_entered,same_finished,other_finished;
         auto entered=same_entered.get_future(),same_done=same_finished.get_future(),other_done=other_finished.get_future();
         std::jthread same_thread,other_thread;bool checked_scope=false,same_blocked=false,other_progress=false;HRESULT same_hr=E_FAIL,other_hr=E_FAIL;
         w->set_batch_draw([&](ID3D12CommandQueue* q,ID3D12Resource* r,const Witness::BatchSubmit& submit){
             if(panel.visible()&&!checked_scope){checked_scope=true;
                 same_thread=std::jthread([&]{same_entered.set_value();same_hr=g.present->Signal(same_queue_point.Get(),1);same_finished.set_value();});
                 entered.wait();same_blocked=same_done.wait_for(std::chrono::milliseconds(8))==std::future_status::timeout;
                 other_thread=std::jthread([&]{other_hr=g.other->Signal(other_queue_point.Get(),1);other_finished.set_value();});
                 other_progress=other_done.wait_for(std::chrono::milliseconds(100))==std::future_status::ready;
             }
             panel.submit_batch(q,r,submit);
         });
         D3D12_HEAP_PROPERTIES read_heap{};read_heap.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC read_desc{};
         read_desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;read_desc.Width=256;read_desc.Height=1;read_desc.DepthOrArraySize=1;read_desc.MipLevels=1;read_desc.SampleDesc.Count=1;read_desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
         ComPtr<ID3D12Resource> game_read;hr(g.device->CreateCommittedResource(&read_heap,D3D12_HEAP_FLAG_NONE,&read_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&game_read)),"original-game implicit readback");
         ComPtr<ID3D12Resource> buffer;hr(g.chain->GetBuffer(g.chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)),"baseline buffer");
         transition(g,buffer.Get(),g.present.Get(),Transition::legacy,true);const auto baseline=pixel(g,buffer.Get(),30,50);
         (void)w->present(g.chain.Get());buffer.Reset();hr(g.chain->Present(0,0),"baseline Present");g.idle();panel.show();
         for(unsigned i=0;i<22;++i){
             hr(g.chain->GetBuffer(g.chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)),"batch buffer");
             const auto before=panel.snapshot().value("draws",0ULL);
             transition(g,buffer.Get(),g.present.Get(),Transition::legacy,true,game_read.Get());
             if(i==0){same_thread.join();other_thread.join();
                 need(checked_scope&&same_blocked&&SUCCEEDED(same_hr),"A competing same-queue Signal cannot interleave with game/panel submission");
                 need(other_progress&&SUCCEEDED(other_hr),"Another queue remains independently usable during panel preparation");}
             need(panel.snapshot().value("draws",0ULL)==before+1,"The panel is drawn inside the original game batch");
             sync_after(g,g.present.Get(),Sync::unrelated_direct_wait);
             const auto candidate=w->batch_queue(Witness::name(g.chain.Get()));
             need(candidate.Get()==g.present.Get(),"Fresh batch binding names the exact submitting queue, independently of Present");
             need(!w->present(g.chain.Get()),"Foreign wait still refuses the old Present-time draw proof");
             const auto at_present=panel.snapshot().value("draws",0ULL);panel.present(g.chain.Get(),false,nullptr);
             need(panel.snapshot().value("draws",0ULL)==at_present,"Batch mode never duplicates the draw in Present");
             const auto bounds=panel.snapshot()["controls"]["panel_bounds"];
             const auto px=unsigned(bounds[0].get<float>()+6*std::clamp(GetDpiForWindow(g.window)/96.f,1.f,2.5f));
             const auto py=unsigned(bounds[1].get<float>()+bounds[3].get<float>()*.5f);
             need(px<1280&&py<800,"DPI-aware panel sample lies inside the fixture target");
             // Dear ImGui's new auto-sized window needs its initial layout
             // frames before it contributes draw data. Verify twenty real frames
             // after that warm-up, rather than calling a submission visibility.
             const auto actual=pixel(g,buffer.Get(),px,py);
             need(i<2||actual!=baseline,"Actual batch panel pixels before Present at frame "+std::to_string(i)+" bounds="+bounds.dump()+" actual="+lab::json(actual).dump()+" baseline="+lab::json(baseline).dump());
             need(pixel(g,buffer.Get(),1279,799)==baseline,"Pixels outside the panel retain the game clear colour");
             void* original=nullptr;D3D12_RANGE read{0,4};hr(game_read->Map(0,&read,&original),"original-game Map");std::array<unsigned char,4> original_pixel{};std::memcpy(original_pixel.data(),original,4);D3D12_RANGE none{};game_read->Unmap(0,&none);
             need(original_pixel==baseline,"The original game's implicit read finishes before the panel and decays at its original Execute boundary");
             buffer.Reset();hr(g.chain->Present(0,0),"batch original Present");g.idle();
         }
         const auto state=w->status();need(state["batch_panel"].value("appended",0u)==22,"One appended panel list per game batch");
         need(panel.snapshot()["submission_evidence"].value("gpu_completed",0u)==22,"All twenty-two batch panel submissions retire on the GPU");
         need(state["wait_provenance"].value("ignored_old_completed_waits",99u)==0,"Same-batch rendering needs no old-wait exemption");
         // Reset a recorded list before it is submitted: stale PRESENT metadata
         // must neither append a panel nor qualify the subsequent empty batch.
         const auto before=panel.snapshot().value("draws",0ULL);hr(g.chain->GetBuffer(g.chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)),"reset buffer");
         hr(g.allocator->Reset(),"discard allocator");hr(g.list->Reset(g.allocator.Get(),nullptr),"record discarded list");
         D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={buffer.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET};
         g.list->ResourceBarrier(1,&b);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);g.list->ResourceBarrier(1,&b);hr(g.list->Close(),"discard Close");
         hr(g.list->Reset(g.allocator.Get(),nullptr),"discard by Reset");hr(g.list->Close(),"empty Close");ID3D12CommandList* lists[]{g.list.Get()};g.present->ExecuteCommandLists(1,lists);g.idle();
         need(panel.snapshot().value("draws",0ULL)==before,"A Reset command list cannot append a stale buffer draw");
         // Hidden panel does no work even though the valid transition is seen.
         SendMessageW(g.window,WM_KEYDOWN,VK_ESCAPE,0);transition(g,buffer.Get(),g.present.Get(),Transition::legacy,true);g.idle();
         need(panel.snapshot().value("draws",0ULL)==before,"Closing the panel stops batch rendering");
         buffer.Reset();need(observed()!=FALSE,"The independent binding observer sees recordings and remains healthy with the panel hooks");
         w->set_batch_draw({});panel.stop();w->uninstall_for_test();stop();FreeLibrary(router);}
        // 14. Ineligible final state or more than one named buffer cannot cause
        // an append. Ordinary stand-ins keep these negative cases off DXGI.
        {auto* w=Witness::install(g.device.Get(),{1});w->present(g.stand_in_names());unsigned callbacks=0;
         w->set_batch_draw([&](ID3D12CommandQueue*,ID3D12Resource*,const Witness::BatchSubmit&){++callbacks;});
         hr(g.allocator->Reset(),"away allocator");hr(g.list->Reset(g.allocator.Get(),nullptr),"away list");
         D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={g.stand_ins[g.stand_in_frame%3].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET};
         g.list->ResourceBarrier(1,&b);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);g.list->ResourceBarrier(1,&b);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);g.list->ResourceBarrier(1,&b);
         hr(g.list->Close(),"away Close");ID3D12CommandList* lists[]{g.list.Get()};g.present->ExecuteCommandLists(1,lists);g.idle();
         need(callbacks==0&&w->status()["batch_panel"].value("last_refusal","")=="batch-not-finally-present","A later transition away from PRESENT refuses append");
         need(!w->batch_queue(g.stand_in_names()),"A non-PRESENT final state cannot bind the batch renderer");
         {Witness::OwnWork own;hr(g.allocator->Reset(),"restore allocator");hr(g.list->Reset(g.allocator.Get(),nullptr),"restore list");std::swap(b.Transition.StateBefore,b.Transition.StateAfter);g.list->ResourceBarrier(1,&b);hr(g.list->Close(),"restore Close");g.present->ExecuteCommandLists(1,lists);g.idle();}
         hr(g.allocator->Reset(),"multi allocator");hr(g.list->Reset(g.allocator.Get(),nullptr),"multi list");
         for(unsigned i=0;i<2;++i){b.Transition={g.stand_ins[i].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET};g.list->ResourceBarrier(1,&b);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);g.list->ResourceBarrier(1,&b);}
         hr(g.list->Close(),"multi Close");g.present->ExecuteCommandLists(1,lists);g.idle();
         need(callbacks==0&&w->status()["batch_panel"].value("last_refusal","")=="multiple-back-buffers-in-batch","An ambiguous multi-buffer batch never calls the renderer");
         w->set_batch_draw({});w->uninstall_for_test();}
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<std::byte> data(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());hr(info->GetMessage(i,message,&bytes),"debug message");
            need(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR&&message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,std::string("D3D12 debug error: ")+message->pDescription);}
        // 15. The GPU survived everything above.

        need(g.device->GetDeviceRemovedReason()==S_OK,"Device not removed");
        std::cout<<"PASS "<<checks<<" present-queue witness checks; decided a real swapchain's present queue from its own "
                   "back-buffer transitions; refused a GPU-ordered cross-queue shape, alternating queues, a doubly transitioned "
                   "buffer and missing transitions; counted a contradiction; re-decided after one only on a fresh full run, "
                   "bounded; re-named the buffers after a rebind. enhanced barriers: "<<enhanced<<'\n';
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
