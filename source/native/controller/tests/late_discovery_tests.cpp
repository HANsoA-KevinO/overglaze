// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Late discovery, executed: in a process where a D3D12 swapchain ALREADY exists,
// find that implementation's Present-family method addresses without ever having
// seen the swapchain created.
//
// This is the fixture for the one thing late loading needs that nothing else in
// the tree does. Every other vtable read in this codebase starts from an object the
// game handed over; here we start from nothing.
//
// The order is the whole point. The harness creates its swapchain FIRST and only
// then calls discovery, which is exactly the shape of a controller injected into
// a running game. If discovery could only work when it ran first, this test
// would pass while the real case failed.
//
// It is not a game and claims nothing about one: the window is our own, the
// swapchain is our own, and no hook is installed anywhere.
#include "lab_late_discovery.hpp"
#include "lab_hook_bank.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <atomic>

using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
void hr(HRESULT r,const char* what){if(FAILED(r))throw std::runtime_error(std::string(what)+" HRESULT="+std::to_string(static_cast<unsigned long>(r)));}

// A pre-existing swapchain, standing in for the game's. Created before discovery
// runs and still alive while it runs.
struct Existing {
    HWND window=nullptr;
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGIFactory2> factory;ComPtr<IDXGISwapChain3> chain;
    Existing(){
        WNDCLASSEXW c{sizeof(c)};c.lpfnWndProc=DefWindowProcW;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"OverglazeLateFixture";
        RegisterClassExW(&c);
        window=CreateWindowExW(0,c.lpszClassName,L"late fixture",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,c.hInstance,nullptr);
        need(window!=nullptr,"fixture window");
        hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"CreateDXGIFactory2");
        ComPtr<IDXGIFactory6> six;hr(factory.As(&six),"IDXGIFactory6");
        ComPtr<IDXGIAdapter1> adapter;
        for(UINT i=0;;++i){
            ComPtr<IDXGIAdapter1> candidate;
            if(six->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate))==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 d{};hr(candidate->GetDesc1(&d),"GetDesc1");
            if(d.VendorId==0x10de&&!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter=candidate;break;}
        }
        need(adapter!=nullptr,"NVIDIA adapter");
        hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"CreateDevice");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"CreateCommandQueue");
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=64;desc.Height=64;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> one;
        hr(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&one),"CreateSwapChainForHwnd");
        hr(one.As(&chain),"IDXGISwapChain3");
    }
    ~Existing(){if(window)DestroyWindow(window);}
};
}

namespace {
// The adoption half, using the real hook bank the controller host uses. A late
// controller hooks the addresses discovery found and then waits: the first real
// Present that arrives is the running game handing us its own swapchain.
lab::HookBank<5>* bank=nullptr;
std::atomic<IDXGISwapChain3*> adopted{nullptr};
std::atomic<unsigned> observed{0};

HRESULT STDMETHODCALLTYPE present_detour(IDXGISwapChain3* chain,UINT interval,UINT flags){
    ++observed;
    IDXGISwapChain3* none=nullptr;
    adopted.compare_exchange_strong(none,chain); // first real Present wins
    return bank->original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT)>(0,0)(chain,interval,flags);
}
// The other four slots are hooked so the bank installs the same batch the
// controller installs; they only forward.
HRESULT STDMETHODCALLTYPE present1_detour(IDXGISwapChain3* c,UINT i,UINT f,const DXGI_PRESENT_PARAMETERS* p){
    return bank->original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*)>(0,1)(c,i,f,p);}
HRESULT STDMETHODCALLTYPE resize_detour(IDXGISwapChain3* c,UINT n,UINT w,UINT h,DXGI_FORMAT fm,UINT fl){
    return bank->original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT)>(0,2)(c,n,w,h,fm,fl);}
HRESULT STDMETHODCALLTYPE resize1_detour(IDXGISwapChain3* c,UINT n,UINT w,UINT h,DXGI_FORMAT fm,UINT fl,const UINT* nm,IUnknown* const* q){
    return bank->original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*)>(0,3)(c,n,w,h,fm,fl,nm,q);}
HRESULT STDMETHODCALLTYPE colour_detour(IDXGISwapChain3* c,DXGI_COLOR_SPACE_TYPE s){
    return bank->original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,DXGI_COLOR_SPACE_TYPE)>(0,4)(c,s);}
}

int wmain(){
    try{
        // 1. The game is already running and already presenting.
        Existing game;
        need(GetModuleHandleW(L"d3d12.dll")!=nullptr,"d3d12.dll is loaded once a device exists");

        // 2. We arrive late and look for the implementation.
        const auto found=lab::latebind::discover_swapchain_methods();
        need(found.ready,"discovery completed");
        for(const auto* method:found.methods)need(method!=nullptr,"every method address resolved");

        // 3. The addresses really are the ones the pre-existing swapchain uses.
        need(lab::latebind::matches(found,game.chain.Get()),
             "the discovered addresses are the ones the already-existing swapchain uses");

        // 4. They are ordinary code, not the object: the throwaway swapchain that
        //    produced them is long gone, and every address still points into a
        //    loaded, executable image.
        for(const auto* method:found.methods){
            MEMORY_BASIC_INFORMATION info{};
            need(VirtualQuery(method,&info,sizeof(info))==sizeof(info),"method address is queryable");
            const auto executable=info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY);
            need(info.State==MEM_COMMIT&&executable!=0,"method address is committed executable memory");
            HMODULE owner=nullptr;
            need(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(method),&owner)!=0&&owner!=nullptr,
                 "method address belongs to a loaded module");
        }

        // 5. Discovery is repeatable and stable: a second pass finds the same
        //    implementation, which is what lets a failed attach be retried.
        const auto again=lab::latebind::discover_swapchain_methods();
        need(again.methods==found.methods,"a second discovery finds the same implementation");
        need(again.agility==found.agility,"the Agility SDK observation is stable");

        // 6. matches() is a real comparison, not a constant: a Discovery whose
        //    addresses were tampered with must be rejected.
        auto tampered=found;tampered.methods[0]=reinterpret_cast<void*>(&wmain);
        need(!lab::latebind::matches(tampered,game.chain.Get()),"a wrong address is refused");
        lab::latebind::Discovery empty;
        need(!lab::latebind::matches(empty,game.chain.Get()),"an unfinished discovery is refused");
        need(!lab::latebind::matches(found,nullptr),"a null swapchain is refused");

        // ---- adoption: hook what we found, then let the already-running chain present.
        lab::HookBank<5> hooks;bank=&hooks;
        hooks.install({found.methods[0],found.methods[1],found.methods[2],found.methods[3],found.methods[4]},
                      {lab::HookBank<5>::Entries{reinterpret_cast<void*>(&present_detour),reinterpret_cast<void*>(&present1_detour),
                                                 reinterpret_cast<void*>(&resize_detour),reinterpret_cast<void*>(&resize1_detour),
                                                 reinterpret_cast<void*>(&colour_detour)},
                       lab::HookBank<5>::Entries{},lab::HookBank<5>::Entries{},lab::HookBank<5>::Entries{}});
        need(observed.load()==0,"nothing observed before the game presents");

        // The pre-existing swapchain presents, exactly as a running game would.
        for(unsigned i=0;i<3;++i)hr(game.chain->Present(0,DXGI_PRESENT_TEST),"Present");
        need(observed.load()>=3,"the already-existing swapchain's Present reached the late hook");
        auto* taken=adopted.load();
        need(taken==game.chain.Get(),"adoption captured the game's own swapchain object");
        need(lab::latebind::matches(found,taken),"the adopted chain uses the implementation we discovered");

        // ---- the NR seed, resolved from the adopted chain alone.
        ComPtr<ID3D12Device> device;
        need(SUCCEEDED(taken->GetDevice(IID_PPV_ARGS(&device))),"the adopted swapchain yields its D3D12 device");
        need(device.Get()==game.device.Get(),"and it is the device the game actually renders with");
        D3D12_COMMAND_QUEUE_DESC seed_desc{};seed_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> seed;
        need(SUCCEEDED(device->CreateCommandQueue(&seed_desc,IID_PPV_ARGS(&seed))),"a DIRECT seed queue on the adopted device");
        need(seed->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT,"the seed is DIRECT, which the submission router demands");
        // The seed shares the game queue's implementation, which is what lets the
        // submission router bind the game's own submissions later.
        auto** seed_vtable=*reinterpret_cast<void***>(seed.Get());
        auto** game_vtable=*reinterpret_cast<void***>(game.queue.Get());
        need(seed_vtable==game_vtable,"the seed queue shares the game queue's implementation");

        std::cout<<"PASS "<<checks<<" late discovery and adoption checks; found this process's swapchain "
                   "implementation with the swapchain already created, adopted it on its next Present, "
                   "resolved the game's device and seeded a matching DIRECT queue. agility="<<(found.agility?"yes":"no")<<'\n';
        return 0;
    }catch(const std::exception& e){
        std::cerr<<e.what()<<'\n';
        return 1;
    }
}
