// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Finding this process's D3D12 swapchain implementation when we were loaded too
// late to have seen it created.
//
// The root-proxy controller learns the swapchain's method addresses by hooking
// the DXGI factory before any swapchain exists. A late-loaded controller has no
// such moment: the factory call happened long ago and the game's swapchain is
// already presenting. So we build a throwaway device, DIRECT queue and flip
// swapchain of our own on a hidden window, read the method addresses out of its
// vtable, and release everything. The addresses live in the implementation's own
// code pages, not in the objects, so they stay valid after the objects are gone.
//
// Three things make this honest rather than a guess, and each is enforced below:
//
//   - The D3D12 implementation must be the one the GAME is already using. We
//     take d3d12.dll with GetModuleHandleW and never LoadLibrary it, because the
//     Agility SDK picks its implementation when the game first loads d3d12, and
//     a fresh load could hand us the operating system's copy with a different
//     vtable. If d3d12.dll is not loaded yet, this is not a running D3D12 game
//     and we refuse instead of forcing one into existence.
//   - DXGI is resolved from the system copy by path. Under the root strategy
//     this module IS dxgi.dll, so calling CreateDXGIFactory2 through our own
//     import table would re-enter our own proxy.
//   - The addresses we hook are only a candidate until a real swapchain arrives.
//     The caller re-reads the game's actual vtable on the first real Present and
//     refuses if it differs. Nothing here asserts the game uses what we found.
//
// Nothing in this file touches the game's objects, windows or message loop. The
// window is hidden, owned by us, and destroyed before returning.
#include "lab_late_discovery.hpp"
#include "lab_platform.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>

using Microsoft::WRL::ComPtr;

namespace lab::latebind {
namespace {
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}

// A 1x1 hidden window of our own. A flip-model swapchain needs a real HWND, so
// a message-only window will not do. It never becomes visible and never enters
// the game's message loop.
struct HiddenWindow {
    HWND handle=nullptr;ATOM klass=0;HINSTANCE instance=nullptr;
    HiddenWindow(){
        instance=GetModuleHandleW(nullptr);
        WNDCLASSEXW c{sizeof(c)};c.lpfnWndProc=DefWindowProcW;c.hInstance=instance;c.lpszClassName=L"OverglazeLateDiscovery";
        klass=RegisterClassExW(&c); // a duplicate class from an earlier attempt is fine
        handle=CreateWindowExW(0,L"OverglazeLateDiscovery",L"",WS_OVERLAPPED,0,0,1,1,nullptr,nullptr,instance,nullptr);
        need(handle!=nullptr,"Late discovery window");
    }
    ~HiddenWindow(){
        if(handle)DestroyWindow(handle);
        if(klass)UnregisterClassW(L"OverglazeLateDiscovery",instance);
    }
    HiddenWindow(const HiddenWindow&)=delete;HiddenWindow& operator=(const HiddenWindow&)=delete;
};

using CreateFactory=HRESULT(WINAPI*)(UINT,REFIID,void**);
using CreateDevice=HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
}

Discovery discover_swapchain_methods(){
    Discovery out;
    // The game's own D3D12, never a fresh one: the Agility SDK binds its
    // implementation at the game's first d3d12 load.
    const auto d3d12=GetModuleHandleW(L"d3d12.dll");
    need(d3d12!=nullptr,"d3d12.dll is not loaded; this is not a running D3D12 game");
    out.agility=GetModuleHandleW(L"D3D12Core.dll")!=nullptr;
    const auto create_device=reinterpret_cast<CreateDevice>(GetProcAddress(d3d12,"D3D12CreateDevice"));
    need(create_device!=nullptr,"D3D12CreateDevice unavailable");

    // DXGI by path from System32, so the root-strategy proxy cannot re-enter itself.
    std::wstring system(MAX_PATH,L'\0');
    const auto n=GetSystemDirectoryW(system.data(),DWORD(system.size()));
    need(n&&n<system.size(),"System directory unavailable");
    system.resize(n);
    const auto dxgi=LoadLibraryExW((system+L"\\dxgi.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    need(dxgi!=nullptr,"System dxgi.dll unavailable");
    const auto create_factory=reinterpret_cast<CreateFactory>(GetProcAddress(dxgi,"CreateDXGIFactory2"));
    need(create_factory!=nullptr,"CreateDXGIFactory2 unavailable");

    ComPtr<IDXGIFactory2> factory;
    need(SUCCEEDED(create_factory(0,IID_PPV_ARGS(&factory))),"Late discovery factory");
    // Same adapter selection the fixtures use: the first non-software NVIDIA
    // adapter. A mismatch with the game's adapter does not matter here, because
    // only the implementation's code addresses are being read.
    ComPtr<IDXGIAdapter1> adapter;ComPtr<IDXGIFactory6> six;
    if(SUCCEEDED(factory.As(&six)))
        for(UINT i=0;;++i){
            ComPtr<IDXGIAdapter1> candidate;
            if(six->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate))==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 d{};
            if(SUCCEEDED(candidate->GetDesc1(&d))&&d.VendorId==0x10de&&!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter=candidate;break;}
        }

    ComPtr<ID3D12Device> device;
    need(SUCCEEDED(create_device(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))),"Late discovery device");
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    need(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Late discovery queue");

    HiddenWindow window;
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=1;desc.Height=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    need(SUCCEEDED(factory->CreateSwapChainForHwnd(queue.Get(),window.handle,&desc,nullptr,nullptr,&chain)),"Late discovery swapchain");
    ComPtr<IDXGISwapChain3> chain3;
    need(SUCCEEDED(chain.As(&chain3)),"Late discovery swapchain3");

    const auto slots=swapchain_slots();
    auto** v=*reinterpret_cast<void***>(chain3.Get());
    for(unsigned i=0;i<out.methods.size();++i){
        out.methods[i]=v[slots[i]];
        need(out.methods[i]!=nullptr,"Swapchain method address missing");
    }
    // Two different slots resolving to one address would make a hook bank refuse
    // the batch later; catch it here where the message can say why.
    for(unsigned i=0;i<out.methods.size();++i)
        for(unsigned k=i+1;k<out.methods.size();++k)
            need(out.methods[i]!=out.methods[k],"Swapchain methods alias in this implementation");
    out.ready=true;
    return out; // every throwaway object is released on the way out
}

bool matches(const Discovery& d,IDXGISwapChain3* chain)noexcept{
    if(!d.ready||!chain)return false;
    const auto slots=swapchain_slots();
    auto** v=*reinterpret_cast<void***>(chain);
    for(unsigned i=0;i<d.methods.size();++i)if(v[slots[i]]!=d.methods[i])return false;
    return true;
}
}
