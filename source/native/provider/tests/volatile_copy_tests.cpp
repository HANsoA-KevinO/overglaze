// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Cyberpunk 2077 with DLSS-G on: legacy tags declared
// eOnlyValidNow for depth and motion, through the REAL adapter, hooks and
// binding layer of a self-configuring (controller-like) host, on WARP with the
// D3D12 debug layer. Synthetic SL calls; no game, no Streamline DLL, no NR.
//
// What this proves:
//   * not wanted (NR OFF): no copy, no allocation, the refusal names why;
//   * wanted: the render thread never allocates -- the first tag is refused as
//     awaiting allocation, the worker allocates, and from then on the copy is
//     recorded on the tag's own list at the tag call;
//   * the game OVERWRITES its depth and motion after the tag and a Present
//     lands between tag and Evaluate: what the insertion point reads through the
//     bound resource is bit-exact the content at the tag call (WARP executes it);
//   * a copy texture still held by an in-flight frame is never reused; all held
//     is "copy-slots-busy" by name, and a third texture is allocated;
//   * the output colour declared eOnlyValidNow stays refused, by name;
//   * a copy taken out of service before its Evaluate is refused, by name;
//   * no command buffer, a multisample guide: refused per tag, by name;
//   * a Streamline proxy list is unwrapped for the copy; a copy on another list
//     than the Evaluate's is counted as such;
//   * role x lifecycle counters show what the game declared, per Evaluate;
//   * two depth semantics in one call cost one copy (the
//     linear one is "superseded"), a separate call's linear depth has a pool of
//     its own, and nothing ever changes shape or allocates per frame;
//   * a run of copies bound and refused for another reason (the output colour
//     itself OnlyValidNow) backs the copier off, named, with one probe per
//     second; the first admitted Evaluate ends it;
//   * one semantic alternating two shapes changes its pool's shape at most once
//     per hold time; the other shape waits, named; nothing churns or is left unreleased.
#include "lab_workbench_adapter.hpp"
#include "lab_game_profile.hpp"
#include "lab_rejected_call.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace lab::slboundary;
namespace {
unsigned checks=0;
void need(bool b,const std::string& text){++checks;if(!b)throw std::runtime_error(text);}
void hr(HRESULT h,const char* what="D3D12"){if(FAILED(h))throw std::runtime_error(std::string(what)+" HRESULT "+std::to_string(static_cast<unsigned long>(h)));}
struct Token:sl::FrameToken {operator uint32_t()const override{throw std::runtime_error("No private token reads");}} token;
__declspec(noinline) sl::Result get(sl::FrameToken*& out,const uint32_t*){out=&token;return sl::Result::eOk;}
__declspec(noinline) sl::Result constants(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&){return sl::Result::eOk;}
__declspec(noinline) sl::Result legacy(const sl::ViewportHandle&,const sl::ResourceTag*,uint32_t,sl::CommandBuffer*){return sl::Result::eOk;}
__declspec(noinline) sl::Result options(const sl::ViewportHandle&,const lab::rr::OptionsV3&){return sl::Result::eOk;}
__declspec(noinline) sl::Result eval(sl::Feature,const sl::FrameToken&,const sl::BaseStructure**,uint32_t,void*){return sl::Result::eOk;}
// A Streamline proxy list: only slGetNativeInterface leads to the native one.
struct ProxyList final:IUnknown {
    ID3D12GraphicsCommandList* real=nullptr;ULONG refs=1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;if(id==__uuidof(IUnknown)){*out=static_cast<IUnknown*>(this);AddRef();return S_OK;}
        return real?real->QueryInterface(id,out):E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {return --refs;}
} proxy_list;
sl::Result native_interface(void* proxy,void** base){
    if(proxy==&proxy_list){proxy_list.real->AddRef();*base=proxy_list.real;return sl::Result::eOk;}
    static_cast<IUnknown*>(proxy)->AddRef();*base=proxy;return sl::Result::eOk;}
struct Gpu {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator,allocator2;ComPtr<ID3D12GraphicsCommandList> list,list2;
    ComPtr<ID3D12Fence> fence;HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 value=0;
    Gpu(){
        ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp;hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"CreateDevice");
        D3D12_COMMAND_QUEUE_DESC q{};hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"CreateCommandQueue");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator2)));
        hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator2.Get(),nullptr,IID_PPV_ARGS(&list2)));
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"CreateFence");
    }
    // list2 (when used) executes first: the tag's list before the Evaluate's.
    void execute(bool with_second=false){
        std::vector<ID3D12CommandList*> lists;
        if(with_second){hr(list2->Close(),"Close 2");lists.push_back(list2.Get());}
        hr(list->Close(),"Close");lists.push_back(list.Get());
        queue->ExecuteCommandLists(static_cast<UINT>(lists.size()),lists.data());hr(queue->Signal(fence.Get(),++value));
        hr(fence->SetEventOnCompletion(value,event));need(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"GPU completion");
        hr(device->GetDeviceRemovedReason(),"Device removed");
        hr(allocator->Reset(),"Allocator reset");hr(list->Reset(allocator.Get(),nullptr),"List reset");
        if(with_second){hr(allocator2->Reset());hr(list2->Reset(allocator2.Get(),nullptr));}
    }
    ComPtr<ID3D12Resource> texture(UINT w,UINT h,DXGI_FORMAT f,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE,UINT samples=1){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=samples;d.Format=f;d.Flags=flags;
        ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r)),"Texture");return r;
    }
    ComPtr<ID3D12Resource> buffer(UINT64 bytes,D3D12_HEAP_TYPE type){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;
        d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&r)),"Buffer");return r;
    }
    void barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,0,a,b};l->ResourceBarrier(1,&x);}
    // Texel bytes, tightly packed; the texture stays in NON_PIXEL_SHADER_RESOURCE.
    void upload(ID3D12Resource* t,const std::vector<unsigned char>& bytes){
        const auto d=t->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows=0;UINT64 row=0,total=0;
        device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&row,&total);auto staging=buffer(total,D3D12_HEAP_TYPE_UPLOAD);
        unsigned char* mapped=nullptr;hr(staging->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"Map upload");
        for(UINT y=0;y<rows;++y)std::memcpy(mapped+fp.Offset+UINT64(y)*fp.Footprint.RowPitch,bytes.data()+UINT64(y)*row,static_cast<size_t>(row));staging->Unmap(0,nullptr);
        barrier(list.Get(),t,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to{t,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}},from{};from.pResource=staging.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);barrier(list.Get(),t,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);execute();
    }
    // A readback recorded on `l` NOW (for the insertion point), read after execution.
    struct Read {ComPtr<ID3D12Resource> buffer;D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows=0;UINT64 row=0,total=0;
        std::vector<unsigned char> bytes() const {std::vector<unsigned char> out(static_cast<size_t>(row*rows));unsigned char* m=nullptr;
            D3D12_RANGE all{0,static_cast<SIZE_T>(total)};hr(buffer->Map(0,&all,reinterpret_cast<void**>(&m)),"Map readback");
            for(UINT y=0;y<rows;++y)std::memcpy(out.data()+UINT64(y)*row,m+fp.Offset+UINT64(y)*fp.Footprint.RowPitch,static_cast<size_t>(row));
            D3D12_RANGE none{};buffer->Unmap(0,&none);return out;}};
    Read record_read(ID3D12GraphicsCommandList* l,ID3D12Resource* t,D3D12_RESOURCE_STATES state){
        Read r;const auto d=t->GetDesc();device->GetCopyableFootprints(&d,0,1,0,&r.fp,&r.rows,&r.row,&r.total);r.buffer=buffer(r.total,D3D12_HEAP_TYPE_READBACK);
        barrier(l,t,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{},from{t,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};to.pResource=r.buffer.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=r.fp;
        l->CopyTextureRegion(&to,0,0,0,&from,nullptr);barrier(l,t,D3D12_RESOURCE_STATE_COPY_SOURCE,state);return r;
    }
};
// The NR runtime's stand-in at the insertion point: it reads the bound guides on
// the Evaluate's list and HOLDS them (as the runtime's in-flight frame does)
// until the test retires the frame.
struct Receiver:lab::SlAdmissionSink {
    Gpu* gpu=nullptr;unsigned entries=0,acks=0;std::string stage,reason;
    Resolution last{};std::vector<ComPtr<ID3D12Resource>> held;std::vector<Gpu::Read> reads;
    void enter(const Resolution& r,const lab::rr::Packet&,void* command)noexcept override{
        ++entries;last=r;
        try{auto* l=static_cast<ID3D12GraphicsCommandList*>(command);
            for(unsigned i=1;i<3;++i){auto* t=static_cast<ID3D12Resource*>(r.binding.resources[i].native);held.emplace_back(t);
                reads.push_back(gpu->record_read(l,t,static_cast<D3D12_RESOURCE_STATES>(r.binding.resources[i].state)));}}
        catch(...){reason="receiver-read-failed";}
    }
    void boundary_returned(std::uint64_t,bool ok)noexcept override{acks+=ok?1u:0u;}
    void aborted(std::uint64_t)noexcept override{}
    void unavailable()noexcept override{}
    void unavailable(const lab::RejectedCall& r)noexcept override{stage=r.stage;reason=r.reason;}
};
std::vector<unsigned char> pattern(size_t bytes,unsigned seed){std::vector<unsigned char> v(bytes);for(size_t i=0;i<bytes;++i)v[i]=static_cast<unsigned char>((i*37+seed*101+(i>>3))&0xff);return v;}
std::vector<unsigned char> depth_pattern(unsigned texels,float base){std::vector<unsigned char> v(texels*4);
    for(unsigned i=0;i<texels;++i){const float d=base+float(i%29)/64.f;std::memcpy(v.data()+i*4,&d,4);}return v;}
}
int main(){lab::WorkbenchAdapterHost* host=nullptr;try{
    Gpu gpu;
    constexpr UINT w=16,h=8,gw=8,gh=4;
    auto color=gpu.texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    // 2077: R32_TYPELESS hardware depth (a depth/stencil resource), RG16F motion on the SR route, RGBA16F on RR.
    auto depth=gpu.texture(gw,gh,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto motion=gpu.texture(gw,gh,DXGI_FORMAT_R16G16_FLOAT);
    auto depth_garbage=gpu.texture(gw,gh,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),motion_garbage=gpu.texture(gw,gh,DXGI_FORMAT_R16G16_FLOAT);
    const auto depth_a=depth_pattern(gw*gh,.25f),depth_b=depth_pattern(gw*gh,.75f),motion_a=pattern(gw*gh*4,1),motion_b=pattern(gw*gh*4,2);
    gpu.upload(depth.Get(),depth_a);gpu.upload(motion.Get(),motion_a);gpu.upload(depth_garbage.Get(),depth_b);gpu.upload(motion_garbage.Get(),motion_b);
    auto* receiver=new Receiver;receiver->gpu=&gpu;
    host=new lab::WorkbenchAdapterHost(true,receiver,lab::WorkbenchScope::full_metadata,nullptr); // hooks pinned until exit
    host->set_game_profile("cyberpunk2077-rr-v1");host->self_configure_before_attach();
    const std::array<void*,4> api{reinterpret_cast<void*>(get),reinterpret_cast<void*>(eval),reinterpret_cast<void*>(constants),reinterpret_cast<void*>(legacy)};
    need(host->attach_fixture(api,reinterpret_cast<void*>(options),{nullptr,nullptr},nullptr,false,nullptr,false,&native_interface),"Attach on public evidence, self-configuring");
    sl::ViewportHandle viewport(0u);
    lab::rr::OptionsV3 o;o.mode=1;o.width=w;o.height=h;o.pre_exposure=o.exposure_scale=1;o.hdr=sl::eTrue;o.flip_x=o.flip_y=o.alpha=sl::eFalse;
    {auto volatile opt=&options;opt(viewport,o);}
    unsigned frame=0;
    struct Frame {sl::ResourceLifecycle output=sl::eValidUntilEvaluate,guides=sl::eValidUntilEvaluate;
        ID3D12Resource* motion=nullptr;void* tag_list=nullptr;void* eval_list=nullptr;bool null_tag_list=false;
        std::function<void()> between;bool second_list=false;
        // Another depth resource, and a linear depth in the same call.
        ID3D12Resource* depth=nullptr;ID3D12Resource* linear=nullptr;bool linear_first=false;};
    auto run=[&](Frame f){
        ++frame;host->present(1); // the previous frame's Present
        sl::FrameToken* issued=nullptr;auto volatile g=&get;uint32_t index=frame;g(issued,&index);
        sl::Constants c;c.mvecScale={1.f,1.f};auto volatile s=&constants;s(c,*issued,viewport);
        sl::Extent ce{0,0,w,h},ge{0,0,gw,gh};
        sl::Resource rc(sl::ResourceType::eTex2d,color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            rd(sl::ResourceType::eTex2d,f.depth?f.depth:depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            rm(sl::ResourceType::eTex2d,f.motion?f.motion:motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            rl(sl::ResourceType::eTex2d,f.linear,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        std::vector<sl::ResourceTag> tags{sl::ResourceTag(&rc,sl::kBufferTypeScalingOutputColor,f.output,&ce),
            sl::ResourceTag(&rd,sl::kBufferTypeDepth,f.guides,&ge),sl::ResourceTag(&rm,sl::kBufferTypeMotionVectors,f.guides,&ge)};
        if(f.linear){const sl::ResourceTag linear_tag(&rl,sl::kBufferTypeLinearDepth,f.guides,&ge);
            if(f.linear_first)tags.insert(tags.begin(),linear_tag);else tags.push_back(linear_tag);}
        auto volatile l=&legacy;
        void* tag_list=f.null_tag_list?nullptr:f.tag_list?f.tag_list:(f.second_list?static_cast<void*>(gpu.list2.Get()):static_cast<void*>(gpu.list.Get()));
        need(l(viewport,tags.data(),static_cast<uint32_t>(tags.size()),static_cast<sl::CommandBuffer*>(tag_list))==sl::Result::eOk,"Tag result unchanged");
        if(f.between)f.between();
        const sl::BaseStructure* inputs[]{&viewport};auto volatile e=&eval;
        need(e(sl::kFeatureDLSS_RR,*issued,inputs,1,f.eval_list?f.eval_list:gpu.list.Get())==sl::Result::eOk,"Evaluate result unchanged");
        gpu.execute(f.second_list);
    };
    auto retire=[&]{receiver->held.clear();receiver->reads.clear();};
    auto snapshot=[&]{return host->snapshot();};
    auto copies=[&]{return snapshot()["only_valid_now_copies"];};
    // Settle: the first RR call selects the viewport, a few more settle it.
    for(unsigned i=0;i<8;++i){run({});retire();}
    need(receiver->entries>0,"Durable tags are admitted once the viewport settled: "+receiver->stage+" "+receiver->reason);
    const Frame volatile_guides{sl::eValidUntilEvaluate,sl::eOnlyValidNow};
    // ---- A. Not wanted: no copy, no allocation, named.
    host->service_tag_copies(false);
    {const auto before=receiver->entries;run(volatile_guides);
     need(receiver->entries==before,"OnlyValidNow guides are refused while no frame is wanted");
     need(receiver->reason=="global-only-valid-now-expired; depth=not-copied-no-nr-frame-wanted,motion=not-copied-no-nr-frame-wanted","Refusal names roles and cause: "+receiver->reason);
     const auto cp=copies();need(cp["allocations"]==0&&cp["slots"].empty()&&cp["refused"]["not-copied-no-nr-frame-wanted"]==2,"No allocation and the cause counted: "+cp.dump());}
    // ---- B. Wanted: the render thread asks, never allocates.
    host->service_tag_copies(true);
    {const auto before=receiver->entries;run(volatile_guides);
     need(receiver->entries==before&&receiver->reason=="only-valid-now-copy-refused; depth=copy-texture-awaiting-allocation,motion=copy-texture-awaiting-allocation",
          "First wanted tag waits for the worker: "+receiver->reason);
     need(copies()["allocations"]==0,"The render thread allocated nothing");}
    host->service_tag_copies(true);
    {const auto cp=copies();need(cp["allocations"]==4&&cp["slots"]["depth"][0]==2&&cp["slots"]["motion"][0]==2&&cp["slots"]["depth"][3]==unsigned(DXGI_FORMAT_R32_TYPELESS),
        "The worker allocated two single-mip copies per role of the tagged format: "+cp.dump());}
    // ---- C. The game overwrites its guides after the tag, and a Present lands
    // between tag and Evaluate. The insertion point must read the tag-time bits.
    {const auto before=receiver->entries;
     Frame f=volatile_guides;f.between=[&]{
        gpu.barrier(gpu.list.Get(),depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.barrier(gpu.list.Get(),depth_garbage.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->CopyResource(depth.Get(),depth_garbage.Get());
        gpu.barrier(gpu.list.Get(),depth_garbage.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gpu.barrier(gpu.list.Get(),depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gpu.barrier(gpu.list.Get(),motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.barrier(gpu.list.Get(),motion_garbage.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->CopyResource(motion.Get(),motion_garbage.Get());
        gpu.barrier(gpu.list.Get(),motion_garbage.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gpu.barrier(gpu.list.Get(),motion.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        host->present(1);};
     run(f);
     need(receiver->entries==before+1,"A copied OnlyValidNow frame is admitted: "+receiver->stage+" "+receiver->reason);
     const auto& b=receiver->last.binding;
     need(b.resources[1].native!=depth.Get()&&b.resources[2].native!=motion.Get()&&b.resources[1].lab_copy&&b.resources[2].lab_copy,"The binding names Lab copies, not the game's resources");
     need(b.resources[1].state==D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE&&b.resources[1].lifecycle==sl::eOnlyValidNow,"Copy state is ours; the game's lifecycle is kept");
     need(receiver->reads.size()==2&&receiver->reads[0].bytes()==depth_a&&receiver->reads[1].bytes()==motion_a,"The insertion point reads the TAG-TIME depth and motion, bit-exact");
     auto now=gpu.record_read(gpu.list.Get(),depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);gpu.execute();
     need(now.bytes()==depth_b,"The game's own depth really was overwritten after the tag");
     retire();gpu.upload(depth.Get(),depth_a);gpu.upload(motion.Get(),motion_a);}
    // ---- D. A held copy is never reused; all held is named; a third is allocated.
    {run(volatile_guides);const auto first=receiver->last.binding.resources[1].native; // held
     run(volatile_guides);const auto second=receiver->last.binding.resources[1].native;  // held
     need(first&&second&&first!=second,"The next frame copies into another texture while the first is held");
     const auto before=receiver->entries;run(volatile_guides);
     need(receiver->entries==before&&receiver->reason.find("depth=copy-slots-busy")!=std::string::npos,"All copies held: refused by name: "+receiver->reason);
     host->service_tag_copies(true);need(copies()["slots"]["depth"][0]==3,"A third copy texture is allocated");
     run(volatile_guides);const auto third=receiver->last.binding.resources[1].native;
     need(receiver->entries==before+1&&third!=first&&third!=second,"The third texture serves the next frame");
     need(receiver->reads.back().bytes()==motion_a,"and carries the tag-time motion");
     retire();run(volatile_guides);need(receiver->entries==before+2,"Retired copies serve again");retire();}
    // ---- E. The output colour declared OnlyValidNow is never copied.
    {const auto before=receiver->entries;Frame f;f.output=sl::eOnlyValidNow;run(f);
     need(receiver->entries==before&&receiver->reason=="only-valid-now-copy-refused; output=output-colour-written-in-place-not-copyable","Output colour refused by name: "+receiver->reason);}
    // ---- F. No command buffer; a multisample motion guide: per tag, by name.
    {const auto before=receiver->entries;Frame f=volatile_guides;f.null_tag_list=true;run(f);
     need(receiver->entries==before&&receiver->reason.find("depth=no-command-buffer")!=std::string::npos,"No command buffer, named: "+receiver->reason);}
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ms{DXGI_FORMAT_R16G16_FLOAT,4};bool msaa=false;
    if(SUCCEEDED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&ms,sizeof(ms)))&&ms.NumQualityLevels){msaa=true;
        auto multisample=gpu.texture(gw,gh,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,4);
        const auto before=receiver->entries;Frame f=volatile_guides;f.motion=multisample.Get();run(f);
        need(receiver->entries==before&&receiver->reason=="only-valid-now-copy-refused; motion=multisample-unsupported","A multisample guide is refused by name, the other role is copied: "+receiver->reason);
        }
    // ---- G. A Streamline proxy list is unwrapped for the copy and the Evaluate.
    {proxy_list.real=gpu.list.Get();const auto before=receiver->entries;const auto same=copies()["bound_same_list"].get<std::uint64_t>();
     Frame f=volatile_guides;f.tag_list=&proxy_list;f.eval_list=&proxy_list;run(f);
     need(receiver->entries==before+1&&receiver->reads[0].bytes()==depth_a,"A proxy list is unwrapped; the copy is still exact: "+receiver->reason);
     need(copies()["bound_same_list"].get<std::uint64_t>()==same+2,"Copy and Evaluate on one native list are counted as such, per role: "+copies().dump());retire();}
    // ---- H. The tag set on another list (executed first): counted, still exact.
    {const auto other=copies()["bound_other_list"].get<std::uint64_t>();Frame f=volatile_guides;f.second_list=true;run(f);
     need(copies()["bound_other_list"].get<std::uint64_t>()==other+2&&receiver->reads[0].bytes()==depth_a&&receiver->reads[1].bytes()==motion_a,
          "A copy on another list than the Evaluate's is counted as such and is exact when that list runs first");retire();}
    // ---- I. A copy taken out of service before its Evaluate is refused by name.
    {const auto before=receiver->entries;Frame f=volatile_guides;
     f.between=[&]{host->service_tag_copies(false);Sleep(VolatileCopies::kHoldMs+150);host->service_tag_copies(false);};
     run(f);
     need(receiver->entries==before&&receiver->reason=="only-valid-now-copy-refused; depth=copy-replaced-before-evaluate,motion=copy-replaced-before-evaluate",
          "A copy no longer in service is refused at its Evaluate: "+receiver->reason);
     const auto cp=copies();need(cp["slots"].empty()&&cp["retired_pending"].get<unsigned>()>=6,"Not wanted for the hold time: every texture leaves service into the hold: "+cp.dump());
     // Released only after 16 Presents, 2 s and with no holder left.
     for(unsigned i=0;i<16;++i)host->present(1);Sleep(VolatileCopies::kHoldMs+150);host->service_tag_copies(false);
     const auto after=copies();need(after["releases"].get<unsigned>()>=6&&after["retired_pending"]==0,"Held textures are released after the hold: "+after.dump());}
    // ---- K. Hardware AND linear depth tagged OnlyValidNow in
    // one call (both fill the depth role self-configuring). Only the one the
    // binding keeps -- hardware -- is copied, whichever comes first; a linear
    // depth in a call of its own gets a pool of its own. Nothing ever changes
    // shape, allocates per frame or overflows the hold.
    auto linear=gpu.texture(gw*2,gh*2,DXGI_FORMAT_R32_FLOAT);
    {host->service_tag_copies(true);
     Frame f=volatile_guides;f.linear=linear.Get();
     run(f);retire();host->service_tag_copies(true); // awaiting allocation once, then allocated
     const auto start=copies();const auto entries=receiver->entries;
     for(unsigned i=0;i<40;++i){f.linear_first=i%2!=0;run(f);
        need(receiver->entries==entries+i+1&&receiver->last.binding.resources[1].type==sl::kBufferTypeDepth&&receiver->last.binding.resources[1].lab_copy,
             "Both depth semantics in one call: hardware depth copied and bound: "+receiver->stage+" "+receiver->reason);
        retire();host->service_tag_copies(true);}
     auto cp=copies();
     need(cp["allocations"]==start["allocations"]&&!cp["slots"].contains("linear-depth")&&cp.value("reshapes",0ULL)==0&&cp["retire_deferred_hold_full"]==0&&
          cp["refused"]["superseded-by-another-tag-of-the-role"].get<std::uint64_t>()>=40,"The superseded linear depth is never copied; no allocation, no reshape: "+cp.dump());
     // A linear depth in a separate call (the binding keeps the fresh hardware one):
     // copied into its own pool, never into the hardware depth's.
     const auto pools=cp["allocations"].get<std::uint64_t>();
     auto separate=[&]{sl::Extent e{0,0,gw*2,gh*2};sl::Resource r(sl::ResourceType::eTex2d,linear.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const sl::ResourceTag t(&r,sl::kBufferTypeLinearDepth,sl::eOnlyValidNow,&e);auto volatile l=&legacy;
        need(l(viewport,&t,1,static_cast<sl::CommandBuffer*>(gpu.list.Get()))==sl::Result::eOk,"Separate tag result unchanged");};
     Frame g=volatile_guides;g.between=separate;
     for(unsigned i=0;i<20;++i){run(g);retire();host->service_tag_copies(true);}
     cp=copies();
     need(cp["slots"]["linear-depth"][0]==2&&cp["slots"]["depth"][0]==2&&cp["allocations"].get<std::uint64_t>()==pools+2&&cp.value("reshapes",0ULL)==0&&
          receiver->last.binding.resources[1].type==sl::kBufferTypeDepth,"A separate call's linear depth has its own pool; hardware depth still bound: "+cp.dump());}
    // ---- M. Backoff: the output colour itself OnlyValidNow,
    // so every Evaluate is refused whatever is copied. After kWastedStreak such
    // Evaluates the copies stop, named; one probe per kProbeMs copies a single
    // Evaluate interval; the first admitted Evaluate ends the backoff.
    {Frame f=volatile_guides;f.output=sl::eOnlyValidNow;const auto entries=receiver->entries;
     for(unsigned i=0;i<VolatileCopies::kWastedStreak;++i){run(f);retire();host->service_tag_copies(true);}
     auto cp=copies();
     need(receiver->entries==entries&&cp["backoff"]["active"]==true&&cp["wasted_evaluates"].get<std::uint64_t>()>=VolatileCopies::kWastedStreak,
          "A run of Evaluates refused for an uncopyable output backs the copier off: "+cp.dump());
     // The first probe may already be requested: at most one copy per probe since.
     const auto copied=cp["copied"]["depth"].get<std::uint64_t>();
     for(unsigned i=0;i<10;++i){run(f);retire();host->service_tag_copies(true);}
     cp=copies();
     need(cp["copied"]["depth"].get<std::uint64_t>()-copied<=cp["backoff"]["probes"].get<std::uint64_t>()&&cp["backoff"]["entered"]==1&&
          cp["refused"]["not-copied-backoff-after-refused-evaluates"].get<std::uint64_t>()>=16,"Backing off: only probes are copied: "+cp.dump());
     need(receiver->reason.find("depth=not-copied-backoff-after-refused-evaluates")!=std::string::npos,"The refusal names the backoff: "+receiver->reason);
     // The output becomes durable: the next probe window is admitted and ends it.
     Sleep(VolatileCopies::kProbeMs+100);host->service_tag_copies(true);
     run(volatile_guides);retire(); // its return opens the probe window
     run(volatile_guides);
     need(receiver->entries==entries+1&&copies()["backoff"]["active"]==false,"The probe's admitted Evaluate ends the backoff: "+receiver->reason+" "+copies().dump());
     retire();host->service_tag_copies(true);run(volatile_guides);retire();
     need(receiver->entries==entries+2,"Copies resume every frame after the backoff");}
    // ---- L. One semantic, two shapes alternating: the pool changes shape at
    // most once per kHoldMs, the other shape waits by name, and no texture is
    // churned through the hold or left unreleased.
    {auto big=gpu.texture(gw*2,gh*2,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
     const auto before=copies();
     for(unsigned i=0;i<24;++i){Frame f=volatile_guides;if(i%2)f.depth=big.Get();run(f);retire();host->service_tag_copies(true);}
     const auto cp=copies();
     need(cp.value("reshapes",0ULL)-before.value("reshapes",0ULL)<=1&&cp["allocations"].get<std::uint64_t>()-before["allocations"].get<std::uint64_t>()<=2&&
          cp["retire_deferred_hold_full"]==0&&cp["retired_pending"].get<unsigned>()<=VolatileCopies::kSlots*3&&
          cp["refused"]["copy-texture-awaiting-reshape"].get<std::uint64_t>()>=10,"Alternating shapes: at most one reshape per hold, the rest named: "+cp.dump());}
    // ---- J. Role x lifecycle counters, per Evaluate, and the stats.
    {const auto s=snapshot()["binding_stats"];const auto& rl=s["role_lifecycles"];
     need(rl["depth"]["global"]["only-valid-now"].get<std::uint64_t>()>=10&&rl["motion"]["global"]["only-valid-now"].get<std::uint64_t>()>=10&&
          rl["output-colour"]["global"]["only-valid-now"].get<std::uint64_t>()>=1&&rl["depth"]["global"]["until-evaluate"].get<std::uint64_t>()>=7,"Role x lifecycle counted per Evaluate: "+rl.dump());
     need(s["copies_bound"]["depth"].get<std::uint64_t>()>=6&&s["outcomes"].contains("only-valid-now-copy-refused")&&s["outcomes"].contains("global-only-valid-now-expired"),"Bound copies and both refusals counted: "+s.dump());}
    need(receiver->acks==receiver->entries,"Every admission acknowledged");
    {const auto bytes=snapshot().dump().size();need(bytes<=lab::adapter_snapshot_limit,"Snapshot within the controller bound: "+std::to_string(bytes));}
    host->stop();
    ComPtr<ID3D12InfoQueue> info;unsigned errors=0;
    if(SUCCEEDED(gpu.device.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);
        std::vector<char> m(size);auto* msg=reinterpret_cast<D3D12_MESSAGE*>(m.data());info->GetMessage(i,msg,&size);
        if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<"debug: "<<msg->pDescription<<'\n';}}
    need(errors==0,"D3D12 debug layer: no errors");
    std::cout<<"PASS checks="<<checks<<" admitted="<<receiver->entries<<" multisample_case="<<msaa<<" debug_layer="<<(info?1:0)
             <<" WARP=true NR=0 game_dlls=0 files=0\n";
    return 0;
}catch(const std::exception& e){if(host)host->stop();std::cerr<<e.what()<<'\n';return 1;}}
