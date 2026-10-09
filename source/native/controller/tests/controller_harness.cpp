// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The controller host, executed. A synthetic game: our own DXGI factory and
// swapchain (so the shipping dxgi.dll proxy hooks us exactly as it would hook a
// game), our own public Streamline surface, real D3D12 resources and the real
// NR model through the controller bridge.
//
// It proves the things the link-closure test cannot: that the controller host
// starts, that its panel and control pipe come up, that the Streamline adapter
// admits a frame on PUBLIC evidence with no private callbacks and no research
// discovery, that the bridge it loads is the controller variant, that NR really
// executes, and that OFF leaves the game's own texture byte-identical.
//
// It is NOT a game and claims nothing about one: the profile is synthetic, the
// inputs are synthetic, and no research collector exists in this binary to
// record anything about them.
#include "lab_controller_fixture.hpp"
#include "lab_standalone.hpp"
#include "lab_pipe.hpp"
#include "lab_platform.hpp"
#include "lab_installation.hpp"
#include "lab_rr_options.hpp"
#include "lab_nr_mode.hpp"
#include "indirect_layout.hpp"
#include <sl_core_api.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using lab::json;
namespace {
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
void hr(HRESULT r,const char* what){if(FAILED(r))throw std::runtime_error(std::string(what)+" HRESULT="+std::to_string(static_cast<unsigned long>(r)));}

constexpr UINT width=256,height=128,guide_width=width/2,guide_height=height/2;

// ---- the synthetic public Streamline surface. Each is a distinct, non-inlined
// function in this image; /OPT:NOICF keeps their addresses distinct so the
// per-address validation in LabControllerAttachFixture stays meaningful.
// The token's public index accessor is allowed only while the fixture fetches
// tokens WITHOUT an index (Streamline then numbers frames itself); any other
// read of it is a controller bug and throws.
bool token_interface_allowed=false;unsigned streamline_numbered=0;
struct Token final:sl::FrameToken {
    unsigned index=UINT_MAX;
    operator uint32_t()const override{if(!token_interface_allowed)throw std::runtime_error("Private token access forbidden");return index;}
} tokens[3];
unsigned token_slot=0,current_frame=0;
__declspec(noinline) sl::Result get_token(sl::FrameToken*& out,const uint32_t* index){
    const unsigned value=index?*index:streamline_numbered;
    if(tokens[token_slot].index!=value){token_slot=(token_slot+1)%3;tokens[token_slot].index=value;}
    out=&tokens[token_slot];current_frame=value;return sl::Result::eOk;
}
__declspec(noinline) sl::Result set_constants(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&){return sl::Result::eOk;}
__declspec(noinline) sl::Result set_tags(const sl::ViewportHandle&,const sl::ResourceTag*,uint32_t,sl::CommandBuffer*){return sl::Result::eOk;}
__declspec(noinline) sl::Result set_options(const sl::ViewportHandle&,const lab::rr::OptionsV3&){return sl::Result::eOk;}
__declspec(noinline) sl::Result set_sr_options(const sl::ViewportHandle&,const lab::rr::SrOptionsPrefix&){return sl::Result::eOk;}
__declspec(noinline) sl::Result evaluate(sl::Feature,const sl::FrameToken&,const sl::BaseStructure**,uint32_t,void*){return sl::Result::eOk;}
// --sr also records through a stand-in for a Streamline proxy command list, as
// a game that creates its device through Streamline does: the object handed to
// Evaluate is not the native list, and only slGetNativeInterface leads to it.
struct ProxyList final:IUnknown {
    ID3D12GraphicsCommandList* real=nullptr;ULONG refs=1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;
        if(id==__uuidof(IUnknown)){*out=static_cast<IUnknown*>(this);AddRef();return S_OK;}
        return real?real->QueryInterface(id,out):E_NOINTERFACE; // a proxy answers for the interfaces it wraps
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {return --refs;}
} proxy_list;
// Stands in for the interposer's public slGetNativeInterface. Real Streamline
// AddRefs what it returns and the adapter releases it, so do the same.
__declspec(noinline) sl::Result native_interface(void* proxy,void** base){
    if(proxy==&proxy_list){proxy_list.real->AddRef();*base=proxy_list.real;return sl::Result::eOk;}
    auto* out=static_cast<IUnknown*>(proxy);out->AddRef();*base=out;return sl::Result::eOk;
}

struct Window {
    HWND h=nullptr;
    Window(){
        WNDCLASSEXW c{sizeof(c)};c.lpfnWndProc=DefWindowProcW;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"LabControllerHarness";
        RegisterClassExW(&c);
        h=CreateWindowExW(0,c.lpszClassName,L"Overglaze controller fixture",WS_OVERLAPPEDWINDOW,0,0,int(width),int(height),
                          nullptr,nullptr,c.hInstance,nullptr);
        if(!h)throw std::runtime_error("Fixture window");
    }
};

struct Gpu {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;lab::Handle event;UINT64 value=0;
    ComPtr<IDXGIFactory2> factory;ComPtr<IDXGISwapChain1> chain;
    explicit Gpu(HWND window){
        // CreateDXGIFactory* goes through the Lab proxy next to us, which is the
        // whole point: the controller host hooks this factory.
        hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIFactory6> six;hr(factory.As(&six),"IDXGIFactory6");
        for(UINT i=0;;++i){
            ComPtr<IDXGIAdapter1> candidate;
            if(six->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate))==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 d{};hr(candidate->GetDesc1(&d),"GetDesc1");
            if(d.VendorId==0x10de&&!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter=candidate;break;}
        }
        need(adapter!=nullptr,"NVIDIA adapter for the controller fixture");
        hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"CreateDevice");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"CreateCommandQueue");
        hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"CreateCommandAllocator");
        hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"CreateCommandList");
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"CreateFence");
        event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);need(event.valid(),"Fence event");
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=width;desc.Height=height;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&chain),"CreateSwapChainForHwnd");
    }
    void execute(){
        hr(commands->Close(),"Close");
        ID3D12CommandList* list[]{commands.Get()};queue->ExecuteCommandLists(1,list);
        hr(queue->Signal(fence.Get(),++value),"Signal");
        hr(fence->SetEventOnCompletion(value,event.value),"SetEventOnCompletion");
        need(WaitForSingleObject(event.value,30000)==WAIT_OBJECT_0,"GPU completion timeout");
        hr(device->GetDeviceRemovedReason(),"Device removed");
        hr(allocator->Reset(),"Allocator reset");hr(commands->Reset(allocator.Get(),nullptr),"Command list reset");
    }
    ComPtr<ID3D12Resource> texture(UINT w,UINT h,DXGI_FORMAT format,bool uav,D3D12_RESOURCE_STATES state,UINT16 mips=1){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=1;d.MipLevels=mips;
        d.SampleDesc.Count=1;d.Format=format;if(uav)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"CreateCommittedResource");
        return r;
    }
    // A game's own compute bindings before the SL Evaluate: a shader-visible
    // heap, an empty root signature and a trivial compute PSO. Binding
    // preservation only inserts once it has seen the game's compute root and a
    // pipeline on the list; RE9 and 007 then keep using them after the Evaluate.
    struct ComputeState {ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12CommandSignature> signature,unobserved;ComPtr<ID3D12Resource> arguments;bool observed=false;};
    ComputeState compute_state(){
        ComputeState s;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.heap)),"Game descriptor heap");
        D3D12_ROOT_SIGNATURE_DESC rd{};ComPtr<ID3DBlob> blob,error;
        hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Serialize game root signature");
        hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&s.root)),"Game root signature");
        static constexpr char source[]="[numthreads(1,1,1)] void main(){}";ComPtr<ID3DBlob> cs;
        hr(D3DCompile(source,sizeof(source)-1,"game_cs",nullptr,nullptr,"main","cs_5_0",0,0,&cs,&error),"Compile game compute shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=s.root.Get();pd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
        hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&s.pso)),"Game compute PSO");
        // Two dispatch command signatures and a zeroed argument buffer. Both are
        // created after the host attached: the controller host observes signature
        // creation on its early path (binding preservation, 007), so it recorded
        // the layout on each. The second one then loses that record: to the host it
        // is exactly a signature created before it attached -- RE9's after a late
        // attach -- since the record is all it keeps.
        D3D12_INDIRECT_ARGUMENT_DESC dispatch{};dispatch.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        const D3D12_COMMAND_SIGNATURE_DESC sd{sizeof(D3D12_DISPATCH_ARGUMENTS),1,&dispatch,0};
        hr(device->CreateCommandSignature(&sd,nullptr,IID_PPV_ARGS(&s.signature)),"Game command signature");
        hr(device->CreateCommandSignature(&sd,nullptr,IID_PPV_ARGS(&s.unobserved)),"Second game command signature");
        lab::host::indirect::Layout layout;s.observed=lab::host::indirect::read(s.signature.Get(),layout)&&layout.error==lab::host::indirect::Error::none&&layout.count==1;
        hr(s.unobserved->SetPrivateData(lab::host::indirect::layout_key,0,nullptr),"Drop the host's layout record");
        need(!lab::host::indirect::read(s.unobserved.Get(),layout),"The second signature carries no layout record");
        D3D12_HEAP_PROPERTIES up{};up.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=256;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr(device->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&s.arguments)),"Indirect argument buffer");
        void* mapped=nullptr;hr(s.arguments->Map(0,nullptr,&mapped),"Map arguments");std::memset(mapped,0,256);s.arguments->Unmap(0,nullptr);
        return s;
    }
    // RE Engine's hardware depth: a two-plane R32G8X24_TYPELESS depth/stencil
    // resource, cleared through a DSV (depth 0.375, stencil 0x5A) and then left
    // in the combined read state it is sampled in while depth testing.
    ComPtr<ID3D12Resource> depth_stencil(UINT w,UINT h,D3D12_RESOURCE_STATES read_state){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=1;d.MipLevels=1;
        d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G8X24_TYPELESS;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};clear.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;clear.DepthStencil={.375f,0x5A};
        ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&r)),"CreateCommittedResource depth/stencil");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
        ComPtr<ID3D12DescriptorHeap> dsv_heap;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv_heap)),"DSV heap");
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        const auto handle=dsv_heap->GetCPUDescriptorHandleForHeapStart();device->CreateDepthStencilView(r.Get(),&dsv,handle);
        commands->ClearDepthStencilView(handle,D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,.375f,0x5A,0,nullptr);
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=r.Get();
        b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_DEPTH_WRITE;b.Transition.StateAfter=read_state;
        commands->ResourceBarrier(1,&b);execute();
        return r;
    }
};

// Minimal upload/readback for one texture, enough to prove ON changed the image
// and OFF did not. No capture collector exists in this binary.
struct TextureIO {
    Gpu& gpu;
    explicit TextureIO(Gpu& g):gpu(g){}
    ComPtr<ID3D12Resource> buffer(UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;
        d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;hr(gpu.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"Buffer");
        return r;
    }
    void upload(ID3D12Resource* target,const std::vector<unsigned char>& bytes,UINT pixel,D3D12_RESOURCE_STATES state){
        const auto desc=target->GetDesc();
        const UINT row=UINT(desc.Width)*pixel,padded=(row+255)&~255u;
        auto staging=buffer(UINT64(padded)*desc.Height,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped=nullptr;D3D12_RANGE none{0,0};hr(staging->Map(0,&none,&mapped),"Map upload");
        for(UINT y=0;y<desc.Height;++y)std::memcpy(static_cast<unsigned char*>(mapped)+size_t(y)*padded,bytes.data()+size_t(y)*row,row);
        staging->Unmap(0,nullptr);
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={target,0,state,D3D12_RESOURCE_STATE_COPY_DEST};gpu.commands->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION dst{target,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}},src{};
        src.pResource=staging.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint={desc.Format,UINT(desc.Width),desc.Height,1,padded};
        gpu.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        b.Transition={target,0,D3D12_RESOURCE_STATE_COPY_DEST,state};gpu.commands->ResourceBarrier(1,&b);
        gpu.execute();
    }
    std::vector<unsigned char> read(ID3D12Resource* source,UINT pixel,D3D12_RESOURCE_STATES state){
        const auto desc=source->GetDesc();
        const UINT row=UINT(desc.Width)*pixel,padded=(row+255)&~255u;
        auto staging=buffer(UINT64(padded)*desc.Height,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={source,0,state,D3D12_RESOURCE_STATE_COPY_SOURCE};gpu.commands->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION src{source,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}},dst{};
        dst.pResource=staging.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint={desc.Format,UINT(desc.Width),desc.Height,1,padded};
        gpu.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        b.Transition={source,0,D3D12_RESOURCE_STATE_COPY_SOURCE,state};gpu.commands->ResourceBarrier(1,&b);
        gpu.execute();
        std::vector<unsigned char> out(size_t(row)*desc.Height);
        void* mapped=nullptr;D3D12_RANGE all{0,size_t(padded)*desc.Height};hr(staging->Map(0,&all,&mapped),"Map readback");
        for(UINT y=0;y<desc.Height;++y)std::memcpy(out.data()+size_t(y)*row,static_cast<unsigned char*>(mapped)+size_t(y)*padded,row);
        D3D12_RANGE none{0,0};staging->Unmap(0,&none);
        return out;
    }
};

// half-float encode, enough for a deterministic RGBA16F test image.
std::uint16_t half(float v){
    std::uint32_t bits;std::memcpy(&bits,&v,4);
    const std::uint32_t sign=(bits>>16)&0x8000u;
    int exponent=int((bits>>23)&0xff)-127+15;
    std::uint32_t mantissa=(bits>>13)&0x3ff;
    if(exponent<=0)return std::uint16_t(sign);
    if(exponent>=31)return std::uint16_t(sign|0x7c00u);
    return std::uint16_t(sign|(std::uint32_t(exponent)<<10)|mantissa);
}

json request(const char* method,json params,const json& status,const char* client="controller-fixture"){
    return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id",client},
        {"session_id",status.at("session_id")},{"expected_revision",status.at("revision")},{"method",method},{"params",params}};
}
json status_now(){
    auto reply=lab::pipe_request(GetCurrentProcessId(),{{"protocol","1.0"},{"request_id",lab::uuid()},
        {"client_id","controller-fixture"},{"method","GetStatus"}});
    need(reply.value("ok",false),"GetStatus over the control pipe");
    return reply.at("status");
}
// A game keeps rendering while it waits. So does this fixture: the host creates
// its bridge client on its own worker, and a client that appears after the last
// frame would never see one.
template<class Predicate,class Pump> json await(Predicate ready,const char* what,Pump pump,unsigned seconds=20){
    const auto deadline=GetTickCount64()+seconds*1000ull;
    json last;
    while(GetTickCount64()<deadline){last=status_now();if(ready(last))return last;pump();Sleep(10);}
    // Dump the two blocks that explain every timeout this fixture can hit.
    json why{{"host",last.value("host",json())},{"nr_adapter",last.value("nr_adapter",json())},
        {"nr_runtime",last.value("nr_runtime",json())},{"nr_frame_control",last.value("nr_frame_control",json())}};
    throw std::runtime_error(std::string("Timed out waiting for ")+what+": "+why.dump());
}
}

int wmain(int argc,wchar_t** argv){
    json report{{"purpose","functional-verification"},{"origin","synthetic-game-input-real-nr"},
        {"host","controller-d3d12"},{"game_started",false},{"research_collectors",false}};
    try{
        bool compute_only=false,super_resolution=false,depth_stencil=false,only_valid_now=false;
        for(int i=2;i<argc;++i){
            if(std::wcscmp(argv[i],L"--compute-only")==0&&!compute_only)compute_only=true;
            else if(std::wcscmp(argv[i],L"--sr")==0&&!super_resolution)super_resolution=true;
            else if(std::wcscmp(argv[i],L"--depth-stencil")==0&&!depth_stencil)depth_stencil=true;
            else if(std::wcscmp(argv[i],L"--only-valid-now")==0&&!only_valid_now)only_valid_now=true;
            else if(std::wcscmp(argv[i],L"--unknown-indirect")==0){}
            else need(false,"Usage: lab_controller_harness <report path> [--compute-only] [--sr] [--depth-stencil] [--only-valid-now]");
        }
        need(argc>=2,"Usage: lab_controller_harness <report path> [--compute-only] [--sr] [--depth-stencil] [--only-valid-now]");
        need(!(depth_stencil&&super_resolution),"--depth-stencil is an RR variant");
        // --only-valid-now (Cyberpunk 2077 with DLSS-G on): depth and
        // motion are tagged eOnlyValidNow through the legacy slSetTag; the game then
        // OVERWRITES both and a Present lands between the tag and the Evaluate. NR
        // must run on the copies taken at the tag call. Two-plane depth is refused
        // by the copier by design, so this is not combined with --depth-stencil.
        need(!(only_valid_now&&depth_stencil),"--only-valid-now copies single-plane guides; do not combine with --depth-stencil");
        report["only_valid_now_variant"]=only_valid_now;
        // --depth-stencil (RE9 / RE Engine's measured shape):
        // two-plane R32G8X24_TYPELESS depth in DEPTH_READ|NON_PIXEL_SHADER_RESOURCE,
        // two-channel R16G16_FLOAT motion, and the constants set from another
        // thread. The two-plane resource is never handed to NR unconverted (the
        // NVIDIA driver faulted on it); this variant runs the plane-0 copy
        // (DepthCodec) on real hardware, beyond its WARP test.
        report["depth_stencil_variant"]=depth_stencil;
        // Binding preservation (Test-ControllerHost -BindingPreservation): RE9 and
        // 007 keep recording on the bindings they had before the SL Evaluate, so
        // the insertion must restore them. Here: the controller host turns it on
        // and restores at least once around a real NR insertion.
        wchar_t preserve[4]{};const bool binding_preservation=GetEnvironmentVariableW(L"OVERGLAZE_FIXTURE_BINDING_PRESERVATION",preserve,4)==1&&preserve[0]==L'1';
        report["binding_preservation_variant"]=binding_preservation;
        // --unknown-indirect: before the Evaluate the game also runs an
        // ExecuteIndirect whose signature layout the host cannot know. The host
        // must be asked to tolerate it (OVERGLAZE_FIXTURE_UNKNOWN_INDIRECT=1).
        bool unknown_indirect=false;
        for(int i=2;i<argc;++i)if(std::wcscmp(argv[i],L"--unknown-indirect")==0)unknown_indirect=true;
        need(!unknown_indirect||binding_preservation,"--unknown-indirect needs binding preservation");
        report["unknown_indirect_variant"]=unknown_indirect;
        report["compute_only_variant"]=compute_only;
        // --sr: the game evaluates DLSS super resolution, not RR, on viewport 1
        // and with its own pre-exposure. None of the three matches a package
        // default, so every one has to be read from the calls.
        report["super_resolution_variant"]=super_resolution;
        const unsigned game_viewport=super_resolution?1u:0u;
        const std::filesystem::path output=argv[1];
        // Host contract V4: the fixture's data root is the parent of its run
        // directory (OVERGLAZE_LIVE_DATA_PATH), exactly what the host checks.
        need(output.parent_path().parent_path()==lab::fixture_data_root(),
             "Fixture report must land in this run directory");
        // The file that sits in a game's own directory. It must be OUR proxy and
        // it must carry no host at all -- the whole point of the root strategy is
        // that the game's loader resolves nothing of ours before the game runs.
        auto proxy=GetModuleHandleW(L"dxgi.dll");
        need(proxy!=nullptr,"Controller DXGI proxy not loaded");
        need(GetProcAddress(proxy,"LabDXGIProxyMarker")!=nullptr,"That dxgi.dll is not the Lab root proxy");
        need(!GetProcAddress(proxy,"LabControllerAttachFixture"),"The root proxy must carry no host entry point");
        need(!GetProcAddress(proxy,"LabControllerFactoryCreated"),"The root proxy must carry no host entry point");
        need(!GetProcAddress(proxy,"LabStandaloneAttachFixture"),"This is the controller bootstrap, not the research one");
        need(GetModuleHandleW(L"overglaze_controller.dll")==nullptr,"The host must not be loaded before the renderer exists");

        Window window;
        Gpu gpu(window.h); // the proxy loads the host HERE and hands it this factory

        // Only now, and only from the payload subdirectory's DLL.
        auto host=GetModuleHandleW(L"overglaze_controller.dll");
        need(host!=nullptr,"The proxy did not load the controller host at the first factory");
        auto attach=reinterpret_cast<lab::AttachControllerFixture>(GetProcAddress(host,"LabControllerAttachFixture"));
        auto stop=reinterpret_cast<lab::ControllerStop>(GetProcAddress(host,"LabControllerStop"));
        need(attach&&stop,"Controller fixture entry points");

        auto standby=await([](const json& s){return s.at("host").is_object()&&
            (s["host"].value("state","")=="standby"||s["host"].value("state","")=="awaiting-native-swapchain");},
            "the controller host to start",[]{});
        need(standby["host"].value("backend","")=="controller-d3d12","Controller backend");
        need(standby["host"].value("origin","")=="synthetic","Fixture origin is not claimed to be a game");
        need(standby["host"].value("research_observers",true)==false,"Host declares no research observer");
        need(standby["host"].value("installation_config",true)==false,"Fixture carries no installation config");
        report["host_started"]=standby["host"].value("state","");

        // Attach the public Streamline surface. No private pair, no flags word.
        lab::ControllerFixtureBindings bindings;
        bindings.public_api[0]=reinterpret_cast<void*>(&get_token);
        bindings.public_api[1]=reinterpret_cast<void*>(&evaluate);
        bindings.public_api[2]=reinterpret_cast<void*>(&set_constants);
        bindings.public_api[3]=reinterpret_cast<void*>(&set_tags);
        bindings.options=reinterpret_cast<void*>(&set_options);
        bindings.sr_options=reinterpret_cast<void*>(&set_sr_options);
        bindings.native_interface=reinterpret_cast<void*>(&native_interface);
        need(attach(&bindings),"Attach the controller fixture on public evidence");
        need(!attach(&bindings),"Fixture attachment is one-shot");

        // MinHook patches the target function in place, so calling these very
        // addresses is what a game does and is what goes through the adapter.
        // The volatile pointers stop the compiler from inlining our own bodies.
        auto volatile hooked_token=&get_token;
        auto volatile hooked_evaluate=&evaluate;
        auto volatile hooked_constants=&set_constants;
        auto volatile hooked_tags=&set_tags;
        auto volatile hooked_options=&set_options;
        auto volatile hooked_sr_options=&set_sr_options;

        // --sr also hands over the shapes A Plague Tale: Resonance does: an output
        // colour and a depth that each carry a mip chain; NR must use level 0 only.
        auto target=gpu.texture(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,true,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,super_resolution?4:1);
        const auto depth_state=depth_stencil?D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_DEPTH_READ|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE):D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto depth=depth_stencil?gpu.depth_stencil(guide_width,guide_height,depth_state)
                                :gpu.texture(guide_width,guide_height,DXGI_FORMAT_R32_TYPELESS,false,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,super_resolution?3:1);
        report["mip_chains"]={{"target",target->GetDesc().MipLevels},{"depth",depth->GetDesc().MipLevels}};
        auto motion=gpu.texture(guide_width,guide_height,depth_stencil?DXGI_FORMAT_R16G16_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT,false,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // What the game writes into its guides after tagging them OnlyValidNow.
        ComPtr<ID3D12Resource> depth_after,motion_after;
        if(only_valid_now){const auto d=depth->GetDesc(),m=motion->GetDesc();
            depth_after=gpu.texture(UINT(d.Width),d.Height,d.Format,false,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,d.MipLevels);
            motion_after=gpu.texture(UINT(m.Width),m.Height,m.Format,false,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
        bool output_only_valid_now=false; // the output colour can never be copied: NR writes back into it
        // With OnlyValidNow guides and NR OFF no copy is made, so the latest record
        // at the end is a refusal; the admitted verdict is read while NR was ON.
        json admitted_latest=nullptr;
        report["guide_formats"]={{"depth",unsigned(depth->GetDesc().Format)},{"motion",unsigned(motion->GetDesc().Format)},{"depth_state",unsigned(depth_state)}};

        std::vector<unsigned char> input(size_t(width)*height*8);
        for(size_t p=0;p<size_t(width)*height;++p){
            const std::uint16_t rgba[4]{half(.15f+float(p%97)/512.f),half(.25f+float(p%53)/384.f),half(.5f),half(1.f)};
            std::memcpy(input.data()+p*8,rgba,8);
        }
        TextureIO io(gpu);
        io.upload(target.Get(),input,8,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        sl::ViewportHandle viewport(game_viewport);
        sl::Constants constants;constants.mvecScale={-.5f,.5f};constants.jitterOffset={.25f,-.125f};
        constants.depthInverted=sl::eFalse;constants.reset=sl::eFalse;constants.cameraMotionIncluded=sl::eTrue;
        constants.motionVectors3D=sl::eFalse;constants.motionVectorsJittered=sl::eFalse;
        constants.orthographicProjection=sl::eFalse;constants.motionVectorsDilated=sl::eFalse;
        lab::rr::OptionsV3 options{};options.mode=1;options.width=width;options.height=height;
        options.hdr=sl::eTrue;options.pre_exposure=1.f;options.exposure_scale=1.f;
        options.flip_x=options.flip_y=options.alpha=sl::eFalse;
        lab::rr::SrOptionsPrefix sr_options{};sr_options.mode=1;sr_options.width=width;sr_options.height=height;
        sr_options.hdr=sl::eTrue;sr_options.pre_exposure=.5f;sr_options.exposure_scale=1.f;

        const auto game_compute=binding_preservation?gpu.compute_state():Gpu::ComputeState{};
        // Binding preservation: the host observed the creation of a signature the
        // game made after it attached, and recorded its layout (007 First Light).
        if(binding_preservation)need(game_compute.observed,"The controller host recorded the layout of a signature created after it attached");
        report["signature_layout_recorded_after_attach"]=binding_preservation?json(game_compute.observed):json(nullptr); // no signature without preservation
        // Which signature the game's ExecuteIndirect uses: the observed one, or the
        // one without a record (--unknown-indirect, and the strict phase below).
        bool indirect_unobserved=unknown_indirect;
        unsigned frame_index=0;bool pass_index=true;
        const auto submit_frame=[&]{
            // Present first, the way a game ends its previous frame: the adapter
            // treats a Present as an invalidation boundary, so the metadata this
            // frame publishes must be produced after it.
            gpu.chain->Present(0,DXGI_PRESENT_TEST);
            ++frame_index;
            sl::FrameToken* token=nullptr;uint32_t index=frame_index;streamline_numbered=frame_index;
            need(hooked_token(token,pass_index?&index:nullptr)==sl::Result::eOk,"Public token");
            if(depth_stencil){
                // RE Engine sets the frame's constants on another thread than the Evaluate.
                sl::Result constants_result=sl::Result::eErrorInvalidParameter;
                std::thread([&]{constants_result=hooked_constants(constants,*token,viewport);}).join();
                need(constants_result==sl::Result::eOk,"Public constants (other thread)");
            }else need(hooked_constants(constants,*token,viewport)==sl::Result::eOk,"Public constants");
            if(super_resolution)need(hooked_sr_options(viewport,sr_options)==sl::Result::eOk,"Public SR options");
            else need(hooked_options(viewport,options)==sl::Result::eOk,"Public RR options");
            sl::Extent colour{0,0,width,height},guide{0,0,guide_width,guide_height};
            std::array<sl::Resource,3> resources{
                sl::Resource(sl::ResourceType::eTex2d,target.Get(),unsigned(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)),
                sl::Resource(sl::ResourceType::eTex2d,depth.Get(),unsigned(depth_state)),
                sl::Resource(sl::ResourceType::eTex2d,motion.Get(),unsigned(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))};
            const auto guides=only_valid_now?sl::eOnlyValidNow:sl::eValidUntilEvaluate;
            std::array<sl::ResourceTag,3> tags{
                sl::ResourceTag(&resources[0],sl::kBufferTypeScalingOutputColor,output_only_valid_now?sl::eOnlyValidNow:sl::eValidUntilEvaluate,&colour),
                sl::ResourceTag(&resources[1],sl::kBufferTypeDepth,guides,&guide),
                sl::ResourceTag(&resources[2],sl::kBufferTypeMotionVectors,guides,&guide)};
            if(binding_preservation){
                // The game's own bindings, set before the Evaluate and NOT set again after it.
                ID3D12DescriptorHeap* heaps[]{game_compute.heap.Get()};gpu.commands->SetDescriptorHeaps(1,heaps);
                gpu.commands->SetComputeRootSignature(game_compute.root.Get());gpu.commands->SetPipelineState(game_compute.pso.Get());
                gpu.commands->ExecuteIndirect((indirect_unobserved?game_compute.unobserved:game_compute.signature).Get(),1,game_compute.arguments.Get(),0,nullptr,0);
            }
            need(hooked_tags(viewport,tags.data(),3,gpu.commands.Get())==sl::Result::eOk,"Public tags");
            if(only_valid_now){
                // The game reuses its guides right after tagging them, and a Present
                // (frame generation's) lands before the Evaluate.
                for(auto [game,after]:{std::pair{depth.Get(),depth_after.Get()},std::pair{motion.Get(),motion_after.Get()}}){
                    D3D12_RESOURCE_BARRIER b[2]{};for(auto& x:b)x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b[0].Transition={game,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST};
                    b[1].Transition={after,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE};
                    gpu.commands->ResourceBarrier(2,b);gpu.commands->CopyResource(game,after);
                    for(auto& x:b)std::swap(x.Transition.StateBefore,x.Transition.StateAfter);gpu.commands->ResourceBarrier(2,b);}
                gpu.chain->Present(0,DXGI_PRESENT_TEST);
            }
            const sl::BaseStructure* inputs[]{&viewport};
            proxy_list.real=gpu.commands.Get();
            void* const recorded=super_resolution?static_cast<void*>(static_cast<IUnknown*>(&proxy_list)):static_cast<void*>(gpu.commands.Get());
            need(hooked_evaluate(super_resolution?sl::kFeatureDLSS:sl::kFeatureDLSS_RR,*token,inputs,1,recorded)==sl::Result::eOk,"Public Evaluate");
            // An unrebound consumer: the game dispatches on the bindings it had
            // before the Evaluate. Only correct if the insertion restored them.
            if(binding_preservation)gpu.commands->Dispatch(1,1,1);
            gpu.execute();
        };

        // The host stands by with NR OFF and never self-arms; a control request
        // is what loads the bridge, exactly as the panel would.
        auto s=status_now();
        need(lab::pipe_request(GetCurrentProcessId(),request("TakeControl",json::object(),s)).value("ok",false),"Take control");
        s=status_now();
        need(lab::pipe_request(GetCurrentProcessId(),request("SetNrMode",{{"mode","on"}},s)).value("ok",false),"Request NR ON");

        for(unsigned i=0;i<4;++i)submit_frame();
        auto admitted=await([](const json& st){const auto& a=st.at("nr_adapter");
            return a.is_object()&&a.value("metadata_candidates",0u)>0u;},"the adapter to see a complete frame",submit_frame);
        need(admitted["nr_adapter"].value("admission_mode","")=="public-evaluate-evidence-only",
             "The controller admits on public evidence only: "+admitted["nr_adapter"].value("admission_mode","<missing>"));
        need(admitted["nr_adapter"]["rr_inner"].value("installed",true)==false,"No private RR callbacks in this build");
        need(admitted["nr_adapter"]["common_restore"].value("installed",true)==false,"No common restore hook in this build");
        report["admission"]={{"mode",admitted["nr_adapter"].value("admission_mode","")},{"private_callbacks",false}};

        auto ready=await([&](const json& st){const auto& r=st.at("nr_runtime");
            return r.is_object()&&r.value("state","")=="ready";},"the controller bridge to become ready",submit_frame,60);
        need(ready["nr_runtime"].value("bridge_variant","")=="controller","The controller host loaded the controller bridge");
        need(ready["nr_runtime"].value("research_collectors_available",true)==false,"No research collectors in the controller bridge");
        need(ready["nr_runtime"].value("profile","")=="synthetic-standalone-fixture","Fixture profile is not a game profile");
        report["bridge"]={{"variant",ready["nr_runtime"].value("bridge_variant","")},
            {"module",ready["nr_runtime"].value("bridge",json())},{"research_collectors",false}};

        for(unsigned i=0;i<6;++i)submit_frame();
        auto executing=await([](const json& st){const auto& r=st.at("nr_runtime");
            return r.is_object()&&r.value("evaluates",0u)>0u&&r.value("execution_mode","")!="off";},"NR to execute",submit_frame,40);
        const auto changed=io.read(target.Get(),8,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        need(changed!=input,"NR ON really changed the game's own texture");
        {const auto& bp=executing["nr_runtime"]["binding_preservation"];
         need(bp.is_object()&&bp.value("enabled",false)==binding_preservation,"Binding preservation is on exactly when the fixture asks for it: "+bp.dump());
         if(binding_preservation){
             const auto restored=await([](const json& st){const auto& b=st.at("nr_runtime")["binding_preservation"];
                 return b.is_object()&&b.value("restores",0ULL)>0;},"the insertion to restore the game's bindings",submit_frame,40);
             report["binding_restores"]=restored["nr_runtime"]["binding_preservation"].value("restores",0ULL);
             const auto& rb=restored["nr_runtime"]["binding_preservation"];
             report["unknown_indirect_policy"]=rb.value("unknown_indirect_policy",std::string());report["unknown_indirect_calls"]=rb.value("unknown_indirect_calls",0ULL);
             if(unknown_indirect)need(rb.value("unknown_indirect_policy",std::string())=="assume-untouched (late attach)"&&rb.value("unknown_indirect_calls",0ULL)>0,
                 "An unknown-layout ExecuteIndirect is tolerated only under the asked-for policy, and counted: "+rb.dump());
             else{need(rb.value("unknown_indirect_policy",std::string())=="strict","Strict unknown-indirect policy unless asked: "+rb.dump());
                 // Every frame ran an ExecuteIndirect with the observed signature and
                 // was restored around: admitted with its layout, nothing tolerated.
                 need(rb.value("unknown_indirect_calls",1ULL)==0,"An observed signature is never an unknown one: "+rb.dump());
                 const auto host_indirect=restored.at("host").value("indirect_signatures",json());
                 need(host_indirect.is_object()&&host_indirect.value("recorded",0ULL)>=2&&host_indirect.value("failures",1ULL)==0,
                      "The host publishes the signatures it observed: "+host_indirect.dump());
                 // Strict, a signature without a record is still refused by name --
                 // a skip of that frame, NR kept ON -- and NR resumes once the game
                 // goes back to an observed one.
                 const auto blocked_before=rb.value("observation",json::object()).value("blocked_frames",0ULL);
                 indirect_unobserved=true;
                 const auto refused=await([&](const json& st){const auto o=st.at("nr_runtime")["binding_preservation"].value("observation",json::object());
                     return o.value("reason",std::string())=="indirect-signature-unobserved"&&o.value("blocked_frames",0ULL)>blocked_before;},
                     "an unobserved signature to be refused under the strict policy",submit_frame,40);
                 indirect_unobserved=false;
                 const auto& refusal=refused.at("nr_runtime");
                 need(refusal.value("state","")=="ready"&&refusal.value("error","").empty()&&
                      refusal.value("rejected_call",json::object()).value("disposition","")=="skipped-before-insertion-history-reset"&&
                      refusal.value("rejected_call",json::object()).value("reason","")=="indirect-signature-unobserved",
                      "An unobserved signature is a refused frame (skipped, history reset), never a fault: "+refusal.value("rejected_call",json()).dump());
                 const auto restores_before=refusal.at("binding_preservation").value("restores",0ULL);
                 const auto resumed=await([&](const json& st){const auto& b=st.at("nr_runtime")["binding_preservation"];
                     return b.value("restores",0ULL)>restores_before&&b.value("observation",json::object()).value("reason",std::string()).empty();},
                     "NR to resume on the observed signature",submit_frame,40);
                 const auto& after=resumed.at("nr_runtime").at("binding_preservation");
                 report["indirect_signatures"]={{"host",resumed.at("host").value("indirect_signatures",json())},
                     {"refused",refusal.at("binding_preservation").value("observation",json())},
                     {"resumed_restores",after.value("restores",0ULL)},{"unknown_indirect_calls",after.value("unknown_indirect_calls",0ULL)}};
                 need(report["indirect_signatures"]["unknown_indirect_calls"]==0,"Strict: nothing was tolerated");}}}
        if(only_valid_now){
            // Every admitted frame bound the copies taken at the tag call; the
            // adapter says so per role, and the role x lifecycle counters show
            // what the game declared at each Evaluate.
            const auto copied=await([](const json& st){const auto& a=st.at("nr_adapter");
                if(!a.is_object())return false;const auto bound=a.value("binding_stats",json::object()).value("copies_bound",json::object());
                return bound.value("depth",0ULL)>2&&bound.value("motion",0ULL)>2;},
                "the binding to bind several Lab copies",submit_frame,40);
            const auto& a=copied["nr_adapter"];const auto& cp=a["only_valid_now_copies"];const auto& rl=a["binding_stats"]["role_lifecycles"];
            need(cp.value("wanted",false)&&cp["copied"].value("depth",0ULL)>2&&cp["copied"].value("motion",0ULL)>2&&cp.value("allocations",0ULL)>=4&&
                 cp.value("bound_same_list",0ULL)>0&&cp.value("allocation_failures",1ULL)==0,"Depth and motion copied at their tag call and bound: "+cp.dump());
            need(rl["depth"]["global"].value("only-valid-now",0ULL)>0&&rl["motion"]["global"].value("only-valid-now",0ULL)>0&&
                 rl["output-colour"]["global"].value("until-evaluate",0ULL)>0,"Role x lifecycle counters show the declared lifecycles: "+rl.dump());
            need(a["binding_stats"].value("presents_while_tags_fresh_tolerated",0ULL)>0,"The Present between tag and Evaluate happened and was tolerated");
            admitted_latest=a["latest"];
            report["only_valid_now"]={{"copies",cp},{"role_lifecycles",rl},{"copies_bound",a["binding_stats"]["copies_bound"]},
                {"evaluates",copied["nr_runtime"].value("evaluates",0ULL)}};
            // The output colour declared OnlyValidNow is refused by name -- a skip, ON kept.
            output_only_valid_now=true;
            const auto refused=await([](const json& st){const auto& r=st.at("nr_runtime");const auto rc=r.value("rejected_call",json());
                return rc.is_object()&&rc.value("reason",std::string()).find("output=output-colour-written-in-place-not-copyable")!=std::string::npos;},
                "the OnlyValidNow output colour to be refused by name",submit_frame,40);
            output_only_valid_now=false;
            need(refused["nr_runtime"].value("state","")=="ready"&&refused["nr_runtime"].value("error","").empty()&&
                 refused["nr_runtime"]["rejected_call"].value("disposition","")=="skipped-before-insertion-history-reset","An uncopyable output is a skip, never a fault");
            const auto before=refused["nr_runtime"].value("evaluates",0ULL);
            await([&](const json& st){return st.at("nr_runtime").value("evaluates",0ULL)>before+1;},"NR to resume after the refused output",submit_frame,40);
            report["only_valid_now"]["output_refusal"]=refused["nr_runtime"]["rejected_call"];
        }
        report["nr_on"]={{"evaluates",executing["nr_runtime"].value("evaluates",0u)},
            {"retired",executing["nr_runtime"].value("retired",0u)},
            {"execution_mode",executing["nr_runtime"].value("execution_mode","")}};

        // The capability handshake Phase B reads. Each flag is evidence from this
        // process, so we wait for the read-back rather than asserting it on the
        // first executing poll: the DLL reports its values during Evaluate.
        // Wait for exactly what is about to be asserted, not for a proxy. L3 means
        // the DLL read Tone/Structure/Style back; can_observe_gpu needs a frame to
        // have RETIRED, which happens later. Waiting only for L3 and then asserting
        // the flags was a race, and it lost roughly one run in three.
        static constexpr const char* kProven[]{"can_load","can_toggle","can_set_tone","can_set_structure",
                                               "can_set_style","can_set_exposure","can_observe_gpu"};
        auto proven=await([](const json& st){const auto c=st.at("nr_runtime").value("capabilities",json::object());
            if(c.value("evidence_level","")!="L3")return false;
            for(const char* flag:kProven)if(!c.value(flag,false))return false;
            return true;},"every capability this fixture asserts to be proven, including GPU completion",submit_frame,40);
        const auto caps=proven["nr_runtime"].value("capabilities",json::object());
        for(const char* flag:kProven)
            need(caps.value(flag,false),std::string("Capability proven in this process: ")+flag);
        need(caps.value("can_capture",true)==false,"The controller reports no capture capability");
        need(caps.value("evidence_level","")=="L3","Evidence level L3 after the DLL read Tone/Structure/Style back");
        report["capabilities"]=caps;

        // Frame-index gates: the GAME's frame index is the game's counter.
        // A repeated index skips one frame, a lower one (a reload) is a history
        // break, and a token fetched without an index is identified through its
        // public interface. None of them may stop NR.
        {
            const auto before=status_now()["nr_runtime"];
            const auto evaluates=before.value("evaluates",0ULL),skipped=before.value("skipped_frames",0ULL),gaps=before.value("history_gaps",0ULL);
            --frame_index;submit_frame();                 // the same index again
            frame_index=1;submit_frame();                 // back to 2: a reload
            for(unsigned i=0;i<3;++i)submit_frame();
            auto resumed=await([&](const json& st){const auto& r=st.at("nr_runtime");
                return r.value("evaluates",0ULL)>evaluates+2&&r.value("state","")=="ready";},"NR to continue after a repeated and a lower frame index",submit_frame,40);
            const auto& r=resumed["nr_runtime"];
            need(r.value("skipped_frames",0ULL)>skipped,"A repeated frame index is a skipped frame");
            need(r.value("history_gaps",0ULL)>gaps,"A lower frame index is a history break");
            need(r.value("execution_mode","")=="on"&&r.value("error","").empty(),"Neither stopped NR: "+r.value("error",""));
            token_interface_allowed=true;pass_index=false;
            const auto before_numbered=r.value("evaluates",0ULL);
            auto numbered=await([&](const json& st){return st.at("nr_runtime").value("evaluates",0ULL)>before_numbered+2;},
                                "NR to run on tokens fetched without a frame index",submit_frame,40);
            token_interface_allowed=false;pass_index=true;
            for(unsigned i=0;i<2;++i)submit_frame();
            report["frame_order"]={{"skipped_frames",r.value("skipped_frames",0ULL)},{"history_gaps",r.value("history_gaps",0ULL)},
                {"evaluates_without_caller_index",numbered["nr_runtime"].value("evaluates",0ULL)-before_numbered}};
        }

        // Edit extrapolation (Live ABI25) through the real bridge: requested like
        // any panel setting, applied by the composite with real NR, reported as
        // the factor the composite used, and the settings receipt (observed ==
        // requested, the two composite-only keys included) still matches. Then
        // off again: the plain composite, factor 1.
        report["extrapolation"]=json::array();
        for(const float factor:{3.f,1.f}){
            const bool extrapolate=factor!=1.f;
            s=status_now();const auto before=s["nr_runtime"].value("evaluates",0ULL);
            need(lab::pipe_request(GetCurrentProcessId(),request("SetNrSettings",{{"tone",1},{"structure",1},{"extrapolate",extrapolate},{"extrapolate_factor",3.f}},s)).value("ok",false),
                 "Request edit extrapolation");
            const auto applied=await([&](const json& st){const auto& r=st.at("nr_runtime");const auto set=r.value("settings",json::object());const auto o=set.value("observed",json());
                return r.value("evaluates",0ULL)>before+2&&set.value("applied_extrapolate",!extrapolate)==extrapolate&&set.value("applied_extrapolate_factor",0.f)==factor&&
                    o.is_object()&&o.value("extrapolate",2u)==(extrapolate?1u:0u)&&o.value("extrapolate_factor",0.f)==3.f&&
                    st.at("nr_frame_control").value("state","")=="applied";},"the bridge to apply and acknowledge edit extrapolation",submit_frame,40);
            const auto& r=applied["nr_runtime"];
            need(r.value("state","")=="ready"&&r.value("error","").empty()&&r.value("execution_mode","")=="on","Edit extrapolation never stops NR: "+r.value("error",""));
            report["extrapolation"].push_back({{"requested",extrapolate},{"applied_factor",r["settings"].value("applied_extrapolate_factor",0.f)},{"evaluates",r.value("evaluates",0ULL)}});
        }

        // Compute-only: the conversion, NR and composite all run, but the game's
        // colour is never written back. It is the controller's own diagnostic
        // mode, and without enable_nr_compute_only() the request is refused, so
        // this phase is the regression test for that wiring too.
        if(compute_only){
            s=status_now();
            need(s["capabilities"].value("nr_compute_only",false),"Controller publishes the compute-only capability");
            need(lab::pipe_request(GetCurrentProcessId(),request("SetNrMode",{{"mode","compute-only"}},s)).value("ok",false),"Request compute-only");
            const auto before=proven["nr_runtime"].value("evaluates",0ULL);
            await([](const json& st){const auto& r=st.at("nr_runtime");
                return r.is_object()&&r.value("execution_mode","")=="compute-only";},"NR to run compute-only",submit_frame,40);
            io.upload(target.Get(),input,8,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            auto more=await([&](const json& st){return st.at("nr_runtime").value("evaluates",0ULL)>before+2;},
                            "further NR evaluates while compute-only",submit_frame,40);
            need(io.read(target.Get(),8,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)==input,
                 "compute-only leaves the game's texture byte-identical");
            need(more["nr_runtime"].value("game_color_writeback",true)==false,
                 "compute-only reports no game colour writeback");
            report["compute_only"]={{"evaluates",more["nr_runtime"].value("evaluates",0ULL)},{"texture_untouched",true}};
            s=status_now();
            need(lab::pipe_request(GetCurrentProcessId(),request("SetNrMode",{{"mode","on"}},s)).value("ok",false),"Return to NR ON");
            await([](const json& st){return st.at("nr_runtime").value("execution_mode","")=="on";},"NR to return to ON",submit_frame,40);
        }

        s=status_now();
        // Tags refused as "no frame wanted" before OFF (the bridge did not exist yet).
        const auto not_wanted_before_off=s["nr_adapter"].value("only_valid_now_copies",json::object()).value("refused",json::object()).value("not-copied-no-nr-frame-wanted",0ULL);
        need(lab::pipe_request(GetCurrentProcessId(),request("SetNrMode",{{"mode","off"}},s)).value("ok",false),"Request NR OFF");
        for(unsigned i=0;i<4;++i)submit_frame();
        auto off=await([](const json& st){const auto& f=st.at("nr_frame_control");
            return f.is_object()&&f.value("observed_mode","")=="off";},"NR to return to OFF",submit_frame,40);
        io.upload(target.Get(),input,8,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for(unsigned i=0;i<3;++i)submit_frame();
        need(io.read(target.Get(),8,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)==input,"OFF leaves the game's texture byte-identical");
        report["nr_off"]={{"observed_mode",off["nr_frame_control"].value("observed_mode","")},{"bypass_exact",true}};
        if(only_valid_now){
            // NR OFF on a ready runtime: no frame is wanted, so no copy is made.
            // Waits for a tag refused as "no frame wanted" AFTER the OFF request and a
            // latest call that says so; the snapshot lags the render thread by a poll.
            auto idle=await([&](const json& st){const auto& a=st.at("nr_adapter");const auto cp=a.value("only_valid_now_copies",json::object());
                return !cp.value("wanted",true)&&cp.value("refused",json::object()).value("not-copied-no-nr-frame-wanted",0ULL)>not_wanted_before_off+2&&
                    a.value("latest",json::object()).value("binding_result","")=="global-only-valid-now-expired";},"copies to stop while NR is OFF",submit_frame,40);
            const auto copied=idle["nr_adapter"]["only_valid_now_copies"]["copied"];
            for(unsigned i=0;i<4;++i)submit_frame();
            const auto later=await([&](const json& st){return st.at("nr_adapter")["only_valid_now_copies"].value("refused",json::object()).value("not-copied-no-nr-frame-wanted",0ULL)>=
                idle["nr_adapter"]["only_valid_now_copies"]["refused"].value("not-copied-no-nr-frame-wanted",0ULL)+8;},"the OFF frames to be seen",[]{},10);
            need(later["nr_adapter"]["only_valid_now_copies"]["copied"]==copied,"No copy while NR is OFF: "+later["nr_adapter"]["only_valid_now_copies"].dump());
            report["only_valid_now"]["off"]=later["nr_adapter"]["only_valid_now_copies"];
        }

        // The panel exists and belongs to this host.
        const auto panel=status_now();
        need(panel["host"]["overlay"].is_object(),"Panel snapshot published");
        need(panel["host"]["overlay"].contains("visible"),"Panel reports its visibility");
        report["panel"]={{"published",true},{"visible",panel["host"]["overlay"].value("visible",false)}};

        // Self-configuration: the facts a package used to carry are read from
        // the synthetic game's own RR calls and published (this run has no
        // installation, so nothing else could have supplied them).
        const auto& self=panel["host"]["sl_self_config"];
        need(self.is_object(),"Self-configured SL facts published");
        need(self.value("viewport",-1)==int(game_viewport)&&self.value("calls",0ULL)>0,"Viewport read from the resolved call, calls counted");
        const auto& selection=self["viewport_selection"];
        need(selection.value("locked",false)&&selection.value("viewport",-1)==int(game_viewport)&&selection.value("switches",1ULL)==0&&
             selection.value("other_viewport_calls",1ULL)==0,"The binding layer selected the game's viewport from its first call");
        need(self.value("feature","")==(super_resolution?"sr":"rr"),"The upscaler the game used is the one admitted");
        need(panel["nr_adapter"].value("sr_target",false),"The controller targets SR as well as RR");
        {const auto latest=only_valid_now?admitted_latest:panel["nr_adapter"]["latest"];
         if(only_valid_now)need(panel["nr_adapter"]["latest"].value("binding_result","")=="global-only-valid-now-expired",
             "NR OFF: OnlyValidNow guides are not copied and the refusal keeps its name: "+panel["nr_adapter"]["latest"].dump());
         const auto adm=latest.value("admission",json());
         need(adm.is_object()&&adm.value("stage","")=="admitted"&&adm.value("command","")==(super_resolution?"sl-proxy-unwrapped":"native"),
              "Admission verdict published; a proxy list is admitted through its native list: "+adm.dump());
         // The RE Engine variant's constants come from another thread: that must be
         // recorded on the admitted source, not inferred from the absence of a refusal.
         const auto sources=latest.value("cross_thread_sources",std::string());
         report["cross_thread_sources"]=sources;
         if(depth_stencil)need(sources.find("constants-set-on-another-thread")!=std::string::npos,"Cross-thread constants recorded on the admitted source: "+sources);
         else need(sources.find("constants-set-on-another-thread")==std::string::npos,"Same-thread constants are not recorded as cross-thread: "+sources);}
        if(super_resolution){
            need(panel["nr_adapter"]["sr_options"].value("host_state","")=="observing-requests","SR options setter watched");
            need(self.value("pre_exposure",0.f)==.5f,"A game pre-exposure other than 1 is admitted");
        }
        need(self.value("depth","")=="linear"||self.value("depth","")=="hardware","Depth kind read from the tag type");
        need(self["formats"].is_object()&&self["formats"]["depth"].is_number(),"Resource formats reported");
        report["sl_self_config"]=self;

        // Nothing research-shaped is published by this host.
        const auto final_status=status_now();
        for(const char* absent:{"capture","frame_pair","display_pair","post_inspection","nr_access_inspection",
                                "binding_restore_probe","input_preparation_probe","observer"}){
            need(final_status.at(absent).is_null(),std::string("Controller host publishes no ")+absent);
        }
        need(final_status["capabilities"].value("capture_pair",true)==false,"No capture capability");
        need(final_status["capabilities"].value("post_binding_inspection",true)==false,"No post inspection capability");
        need(final_status["capabilities"].value("nr_access_inspection",true)==false,"No access inspection capability");
        report["research_surface_absent"]=true;

        stop();
        auto stopped=await([](const json& st){return st.at("host").value("state","")=="stopped";},"the host to stop",[]{},30);
        need(stopped["nr_runtime"].value("state","")=="stopped"||stopped["nr_runtime"].value("state","")=="failed",
             "The bridge is stopped with the host");
        report["stopped"]=true;
        report["checks"]=checks;
        report["passed"]=true;
        std::ofstream out(output,std::ios::binary);out<<report.dump(2);
        std::cout<<"PASS "<<checks<<" controller host checks; synthetic game, real NR, no research collector\n";
        return 0;
    }catch(const std::exception& e){
        report["passed"]=false;report["error"]=e.what();report["checks"]=checks;
        if(argc>=2){std::ofstream out(std::filesystem::path(argv[1]),std::ios::binary);out<<report.dump(2);}
        std::cerr<<e.what()<<'\n';
        return 1;
    }
}
