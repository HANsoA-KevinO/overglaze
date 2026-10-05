// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The late panel's swapchain binding, executed on a real GPU and real windows.
//
// RE9: with the late controller injected and NR ON, switching DLSS
// frame generation on in game ended in an access violation inside Streamline's
// frame generation, on a null swapchain pointer while it iterated back buffers.
// The host held the adopted chain for the process lifetime and the panel held
// every back buffer. The working hypothesis (unproven) is that the game
// recreated its chain on the same window and the new creation failed because
// ours kept the old one alive: DXGI admits one flip-model chain per HWND.
//
// What this executes, with the same units the host's Present detour drives
// (latebind::LatePanel over GameOverlay):
//   0. ChainTracker alone: both chains presenting keeps the first; a silent one
//      is taken over only after the stale window; refusals and the cap.
//   1. The DXGI rule itself, as a control: a chain kept alive by any reference
//      makes a second creation on its window fail. And, recorded as a fact, what
//      a back-buffer reference alone does.
//   2. The panel attached and drawing through the late units, then every
//      reference of the test released: a new chain on the same window MUST be
//      created. This fails if the units hold the chain or a back buffer.
//   3. The panel follows the new chain (take-over, or the same identity if the
//      allocator reused the address) and draws on it.
//   4. A take-over to a chain on another window: the old window's procedure is
//      handed back and the new one is subclassed.
//   5. A contradiction detaches before the frame draws, a re-decision attaches
//      again, and past the bound the panel stays withdrawn: unbound, the hotkey
//      passes to the game and nothing is drawn.
// Our own windows, swapchains and queue; no game, no NR.
#include "lab_late_panel.hpp"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Tracker=lab::latebind::ChainTracker;
using Late=lab::latebind::LatePanel;
namespace {
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
void hr(HRESULT r,const char* what){if(FAILED(r))throw std::runtime_error(std::string(what)+" HRESULT=0x"+[&]{char b[16];std::snprintf(b,sizeof b,"%08lx",static_cast<unsigned long>(r));return std::string(b);}());}
std::string hex(HRESULT r){char b[16];std::snprintf(b,sizeof b,"0x%08lx",static_cast<unsigned long>(r));return b;}
unsigned keys=0;
LRESULT CALLBACK window_proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_KEYDOWN&&w=='K')++keys;return DefWindowProcW(h,m,w,l);}
void toggle(HWND w){SendMessageW(w,WM_KEYDOWN,VK_INSERT,0);SendMessageW(w,WM_KEYUP,VK_INSERT,1LL<<31);}

void tracker(){
    int a=0,b=0,c=0,d=0,e=0;
    Tracker t({4,2,3});t.adopt(&a);
    need(t.present(&a)==Tracker::Verdict::followed,"The adopted chain is followed");
    for(int i=0;i<20;++i){
        need(t.present(&b)==Tracker::Verdict::other,"While the followed chain presents, another is never taken over");
        need(t.present(&a)==Tracker::Verdict::followed,"The first chain stays followed");}
    need(t.status().value("held_while_followed_recently_presented",0ULL)==20,"Both presenting is counted: "+t.status().dump());
    for(unsigned i=0;i<4;++i)need(t.present(&b)==Tracker::Verdict::other,"Inside the stale window the followed chain keeps its place");
    need(t.present(&b)==Tracker::Verdict::take_over,"Past the stale window a steadily presenting chain takes over: "+t.status().dump());
    t.adopt(&b);need(t.followed()==&b&&t.present(&b)==Tracker::Verdict::followed,"The new chain is followed");
    for(unsigned i=0;i<4;++i)(void)t.present(&c);
    need(t.present(&c)==Tracker::Verdict::take_over,"A second candidate");
    t.refuse(&c,"different-device");
    for(unsigned i=0;i<10;++i)need(t.present(&c)==Tracker::Verdict::other,"A refused chain is never offered again");
    for(unsigned i=0;i<4;++i)(void)t.present(&d);
    need(t.present(&d)==Tracker::Verdict::take_over,"A third candidate");t.adopt(&d);
    for(unsigned i=0;i<10;++i)need(t.present(&e)==Tracker::Verdict::other,"Past the adoption cap nothing takes over");
    const auto s=t.status();
    need(s.value("adoptions",0ULL)==3&&s.value("take_overs",0ULL)==2&&s["refused"].value("different-device",0ULL)==1&&
         s.value("take_overs_over_cap",0ULL)>0&&s["chains"].size()<=4&&s.value("evicted_chains",0ULL)>0&&s.value("chain_switches",0ULL)>0,
         "Tracker counters: "+s.dump());
}

struct Gpu {
    HWND w1=nullptr,w2=nullptr;
    ComPtr<IDXGIFactory2> factory;ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12DescriptorHeap> rtv;
    ComPtr<ID3D12Fence> fence;UINT64 value=0;HANDLE event=nullptr;
    Gpu(){
        WNDCLASSEXW c{sizeof(c)};c.lpfnWndProc=window_proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"OverglazeLatePanelFixture";RegisterClassExW(&c);
        for(auto* w:{&w1,&w2}){*w=CreateWindowExW(0,c.lpszClassName,L"late panel fixture",WS_OVERLAPPEDWINDOW,0,0,800,500,nullptr,nullptr,c.hInstance,nullptr);need(*w!=nullptr,"fixture window");}
        hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"CreateDXGIFactory2");
        hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"CreateDevice");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
        hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"allocator");
        hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");hr(list->Close(),"Close");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"rtv heap");
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");event=CreateEventW(nullptr,FALSE,FALSE,nullptr);need(event!=nullptr,"event");
    }
    ~Gpu(){if(event)CloseHandle(event);for(auto w:{w1,w2})if(w)DestroyWindow(w);}
    void idle(){hr(queue->Signal(fence.Get(),++value),"Signal");
        if(fence->GetCompletedValue()<value){hr(fence->SetEventOnCompletion(value,event),"SetEventOnCompletion");need(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"GPU idle");}}
    // A flip-model chain on `window`; the HRESULT is returned, never thrown.
    HRESULT make(HWND window,ComPtr<IDXGISwapChain3>& out){out.Reset();
        DXGI_SWAP_CHAIN_DESC1 d{};d.Width=640;d.Height=400;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;
        d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=3;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> one;const HRESULT r=factory->CreateSwapChainForHwnd(queue.Get(),window,&d,nullptr,nullptr,&one);
        if(SUCCEEDED(r))hr(one.As(&out),"IDXGISwapChain3");return r;}
};
struct Probe {ComPtr<ID3D12CommandQueue> queue;bool contradiction=false;unsigned restarts=0,allowed=2,rebinds=0,resolves=0;};

// One game frame on `chain`, in the order the host's Present detour runs:
// follow, bind (followed chain only), the overlay's draw, the original Present.
void frame(Gpu& g,IDXGISwapChain3* chain,Late& late,lab::GameOverlay& overlay,std::uint64_t now){
    ComPtr<ID3D12Resource> buffer;hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)),"GetBuffer");
    hr(g.allocator->Reset(),"allocator Reset");hr(g.list->Reset(g.allocator.Get(),nullptr),"list Reset");
    const auto h=g.rtv->GetCPUDescriptorHandleForHeapStart();g.device->CreateRenderTargetView(buffer.Get(),nullptr,h);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={buffer.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET};
    g.list->ResourceBarrier(1,&b);const float color[]{.1f,.2f,.3f,1};g.list->ClearRenderTargetView(h,color,0,nullptr);
    std::swap(b.Transition.StateBefore,b.Transition.StateAfter);g.list->ResourceBarrier(1,&b);hr(g.list->Close(),"Close");
    ID3D12CommandList* lists[]{g.list.Get()};g.queue->ExecuteCommandLists(1,lists);buffer.Reset();
    IDXGISwapChain* key=chain;
    if(late.follow(key))late.bind(key,true,now);
    overlay.present(key);
    hr(chain->Present(0,0),"Present");g.idle();
}
std::uint64_t draws(const lab::GameOverlay& o){return o.snapshot().value("draws",0ULL);}
}

int wmain(){
    try{
        tracker();
        ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        Gpu g;lab::json report={{"test","late-panel-binding"}};
        lab::Controller controller;controller.enable_nr_preparation(true);controller.enable_embedded_control();
        lab::GameOverlay overlay(controller);
        Probe probe;probe.queue=g.queue;
        Late::Evidence e;
        e.accept=[&](IDXGISwapChain3*,std::string&){return true;};
        e.resolve=[&](IDXGISwapChain3*){++probe.resolves;return Late::Answer{probe.queue,"fixture-creation-queue",{}};};
        e.contradicted=[&](const std::string&){return probe.contradiction;};
        e.restart=[&](std::uint64_t){probe.contradiction=false;return ++probe.restarts<=probe.allowed;};
        e.rebind=[&]{++probe.rebinds;};
        constexpr unsigned stale=6;
        Late late(overlay,e,{stale,3,8});
        std::uint64_t now=1000;

        // 1. Attached and drawing through the late units on chain A (window 1).
        ComPtr<IDXGISwapChain3> a;hr(g.make(g.w1,a),"chain A");
        const void* a_identity=a.Get();
        late.adopt(a.Get());
        frame(g,a.Get(),late,overlay,now);
        need(late.attached()&&overlay.bound(),"The panel attached on the first followed present: "+late.status().dump());
        toggle(g.w1);for(unsigned i=0;i<3;++i)frame(g,a.Get(),late,overlay,++now);
        need(draws(overlay)==3,"The panel draws on A: "+overlay.snapshot().dump());

        // 2. A second chain (window 2) presenting alongside: A stays followed.
        ComPtr<IDXGISwapChain3> c;hr(g.make(g.w2,c),"chain C");
        for(unsigned i=0;i<3*stale;++i){frame(g,c.Get(),late,overlay,++now);frame(g,a.Get(),late,overlay,++now);}
        {const auto s=late.status()["chains"];
         need(s.value("adoptions",0ULL)==1&&s.value("held_while_followed_recently_presented",0ULL)>=3*stale&&draws(overlay)==3+3*stale,
              "Both presenting: the first stays followed and keeps drawing: "+late.status().dump());}

        // 3. The DXGI rule, as a control: any reference keeping A alive makes a
        //    second flip chain on window 1 fail.
        g.idle();
        ComPtr<IDXGISwapChain3> kept=a;a.Reset();
        ComPtr<IDXGISwapChain3> b;const HRESULT while_chain_held=g.make(g.w1,b);
        need(FAILED(while_chain_held),"Premise: a second flip chain on a window whose chain is still referenced fails (got "+hex(while_chain_held)+")");
        // A back-buffer reference alone, recorded as a fact.
        ComPtr<ID3D12Resource> held;hr(kept->GetBuffer(0,IID_PPV_ARGS(&held)),"GetBuffer");kept.Reset();g.idle();
        const HRESULT while_buffer_held=g.make(g.w1,b);
        report["create_while_chain_referenced"]=hex(while_chain_held);
        report["create_while_only_a_back_buffer_referenced"]=hex(while_buffer_held);
        report["back_buffer_reference_keeps_chain_alive"]=FAILED(while_buffer_held);
        // 4. The crash shape: every reference of the test gone, the late units and
        //    the panel were attached and drawing on A. The new chain must be created.
        if(FAILED(while_buffer_held)){held.Reset();g.idle();
            const HRESULT created=g.make(g.w1,b);
            need(SUCCEEDED(created),"With the test's references released, a new chain on the same window is created (got "+hex(created)+
                 "): the late units and the panel hold no reference to the old chain or its back buffers");}
        else held.Reset();
        report["new_chain_same_window"]="created";
        const bool same_identity=b.Get()==a_identity;report["new_chain_reused_address"]=same_identity;
        // 5. The panel follows B and draws on it.
        const auto before_b=draws(overlay);
        for(unsigned i=0;i<2*stale;++i)frame(g,b.Get(),late,overlay,++now);
        if(same_identity){
            // The allocator reused A's address: the same identity, now a new chain.
            // The overlay acquires B's buffers per present and checks the shape.
            need(late.attached()&&draws(overlay)==before_b+2*stale,"Same address: the panel keeps drawing, now into B's buffers: "+overlay.snapshot().dump());
        }else{
            const auto s=late.status();
            need(s["chains"].value("adoptions",0ULL)==2&&probe.rebinds==1&&s.value("detaches",0ULL)==1&&s.value("attaches",0ULL)==2&&late.attached(),
                 "The silent chain is taken over and the panel rebinds on B: "+s.dump());
            need(!overlay.snapshot().value("visible",true),"A rebind hides the panel");
            toggle(g.w1);const auto shown=draws(overlay);for(unsigned i=0;i<3;++i)frame(g,b.Get(),late,overlay,++now);
            need(draws(overlay)==shown+3,"The panel draws on B: "+overlay.snapshot().dump());
        }
        // 6. A take-over to a chain on ANOTHER window (C, window 2, still alive).
        const auto adoptions=late.status()["chains"].value("adoptions",0ULL);
        for(unsigned i=0;i<2*stale;++i)frame(g,c.Get(),late,overlay,++now);
        need(late.status()["chains"].value("adoptions",0ULL)==adoptions+1&&late.attached(),"C is followed: "+late.status().dump());
        need(GetWindowLongPtrW(g.w1,GWLP_WNDPROC)==reinterpret_cast<LONG_PTR>(&window_proc),"Window 1 has its own procedure back");
        need(GetWindowLongPtrW(g.w2,GWLP_WNDPROC)!=reinterpret_cast<LONG_PTR>(&window_proc),"Window 2 is subclassed by the panel");
        toggle(g.w2);{const auto shown=draws(overlay);for(unsigned i=0;i<3;++i)frame(g,c.Get(),late,overlay,++now);
         need(draws(overlay)==shown+3,"The panel draws on C: "+overlay.snapshot().dump());}

        // 7. A contradiction: detached before this frame draws; re-decided; attached again.
        {const auto shown=draws(overlay);probe.contradiction=true;frame(g,c.Get(),late,overlay,++now);
         need(draws(overlay)==shown&&!late.attached()&&!overlay.bound(),"A contradicted queue is not drawn on: "+late.status().dump());
         frame(g,c.Get(),late,overlay,++now);
         need(late.attached()&&late.status().value("redecisions",0ULL)==1,"Re-decided and attached again: "+late.status().dump());
         toggle(g.w2);frame(g,c.Get(),late,overlay,++now);need(draws(overlay)==shown+1,"Drawing again after the re-decision");}
        // 8. Past the bound: withdrawn, unbound, the hotkey goes to the game.
        probe.contradiction=true;frame(g,c.Get(),late,overlay,++now);frame(g,c.Get(),late,overlay,++now); // second re-decision
        need(late.attached()&&late.status().value("redecisions",0ULL)==2,"Second re-decision: "+late.status().dump());
        probe.contradiction=true;frame(g,c.Get(),late,overlay,++now);
        need(!late.attached()&&late.state().rfind("withdrawn",0)==0&&late.status().value("refused_redecisions",0ULL)==1,"Past the bound the panel stays withdrawn: "+late.status().dump());
        {const auto shown=draws(overlay);const auto k=keys;toggle(g.w2);SendMessageW(g.w2,WM_KEYDOWN,'K',0);
         for(unsigned i=0;i<3;++i)frame(g,c.Get(),late,overlay,++now);
         need(draws(overlay)==shown&&!overlay.snapshot().value("visible",true)&&keys==k+1,"Withdrawn: the hotkey opens nothing and the game gets its input: "+overlay.snapshot().dump());}

        overlay.stop();g.idle();
        need(g.device->GetDeviceRemovedReason()==S_OK,"Device not removed");
        ComPtr<ID3D12InfoQueue> info;lab::json errors=lab::json::array();
        if(SUCCEEDED(g.device.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);
            std::vector<std::uint8_t> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,m,&size);
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)errors.push_back(std::string(m->pDescription,m->DescriptionByteLength));}
        need(errors.empty(),"D3D12 debug errors: "+errors.dump());
        report["debug_layer"]=debug!=nullptr;report["late_binding"]=late.status();report["overlay"]={{"draws",draws(overlay)},
            {"attaches",overlay.snapshot().value("attaches",0ULL)},{"detaches",overlay.snapshot().value("detaches",0ULL)}};
        std::cout<<report.dump()<<'\n';
        std::cout<<"PASS "<<checks<<" late-panel checks; a new flip chain on the window of one the panel had been drawing on was created; "
                   "the panel followed it ("<<(same_identity?"same address":"take-over")<<"), moved to another window, re-decided after a "
                   "contradiction and stayed withdrawn past the bound\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
