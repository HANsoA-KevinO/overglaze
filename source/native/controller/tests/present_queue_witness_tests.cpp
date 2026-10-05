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
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <string>

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
enum class Sync{cpu,foreign_direct_wait,compute_wait};

// Record buffer -> RENDER_TARGET -> PRESENT on `on` and submit it there.
void transition(Gpu& g,ID3D12Resource* buffer,ID3D12CommandQueue* on,Transition kind){
    hr(g.allocator->Reset(),"allocator Reset");hr(g.list->Reset(g.allocator.Get(),nullptr),"list Reset");
    if(kind==Transition::legacy){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=buffer;
        b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;g.list->ResourceBarrier(1,&b);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);g.list->ResourceBarrier(1,&b);
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
    }else if(sync==Sync::compute_wait){
        // An async-compute queue waiting on graphics is not a present queue.
        hr(on->Signal(g.fence.Get(),++g.value),"Signal");hr(g.compute->Wait(g.fence.Get(),g.value),"compute Wait");
    }else g.drain(on);
}
// One real frame on the swapchain; `w` is fed where the controller host feeds it.
void frame(Gpu& g,Witness* w,Transition kind,Sync sync=Sync::cpu){
    ComPtr<ID3D12Resource> buffer;hr(g.chain->GetBuffer(g.chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)),"GetBuffer");
    transition(g,buffer.Get(),g.present.Get(),kind);sync_after(g,g.present.Get(),sync);
    if(w)w->present(g.chain.Get());
    const HRESULT presented=g.chain->Present(0,0);
    if(FAILED(presented))throw std::runtime_error("Present HRESULT="+std::to_string(static_cast<unsigned long>(presented))+
        " removed reason="+std::to_string(static_cast<unsigned long>(g.device->GetDeviceRemovedReason())));
    g.idle();
}
// One stand-in frame: the current stand-in is transitioned on each queue in
// `on` (in order), then the witness judges it; nothing is presented.
void stand_in(Gpu& g,Witness& w,std::initializer_list<ID3D12CommandQueue*> on,Sync sync=Sync::cpu){
    auto* buffer=g.stand_ins[g.stand_in_frame%3].Get();
    for(auto* q:on){transition(g,buffer,q,Transition::legacy);sync_after(g,q,sync);if(sync!=Sync::cpu)g.idle();}
    w.present(g.stand_in_names());
    g.idle();++g.stand_in_frame;
}
std::uint64_t miss(const lab::json& s,const char* why){return s["misses"].value(why,0ULL);}
}

int wmain(){
    try{
        Gpu g;
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

        // 11. The GPU survived everything above.
        need(g.device->GetDeviceRemovedReason()==S_OK,"Device not removed");
        std::cout<<"PASS "<<checks<<" present-queue witness checks; decided a real swapchain's present queue from its own "
                   "back-buffer transitions; refused a GPU-ordered cross-queue shape, alternating queues, a doubly transitioned "
                   "buffer and missing transitions; counted a contradiction; re-decided after one only on a fresh full run, "
                   "bounded; re-named the buffers after a rebind. enhanced barriers: "<<enhanced<<'\n';
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
