// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Admission on PUBLIC evidence only, for Streamline builds
// with no hand-reviewed begin/end profile. Synthetic SL calls + WARP COM
// resources; no game DLL, no Streamline DLL, no rendering and no NR.
//
// What this proves: without the private plugin callbacks the adapter still
// refuses everything it refused before EXCEPT the inner cross-check, and it
// substitutes a public one -- the buffer the game handed to Evaluate must be
// the native D3D12 list, not a Streamline proxy.
#include "lab_workbench_adapter.hpp"
#include "lab_game_profile.hpp"
#include "lab_rejected_call.hpp"
#include <dxgi1_6.h>
#include <string>
#include <iostream>
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0,current_frame=0;
bool fail_outer=false,native_fails=false;
void* native_override=nullptr; // non-null => Evaluate's buffer unwraps elsewhere
struct Token:sl::FrameToken {operator uint32_t()const override{throw std::runtime_error("No private token reads");}} token;
void need(bool b,const char* text){++checks;if(!b)throw std::runtime_error(text);}
void hr(HRESULT h){if(FAILED(h))throw std::runtime_error("D3D12 HRESULT "+std::to_string(h));}
__declspec(noinline) sl::Result get(sl::FrameToken*& out,const uint32_t* frame){out=&token;current_frame=*frame;return sl::Result::eOk;}
__declspec(noinline) sl::Result constants(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&){return sl::Result::eOk;}
__declspec(noinline) sl::Result legacy(const sl::ViewportHandle&,const sl::ResourceTag*,uint32_t,sl::CommandBuffer*){return sl::Result::eOk;}
__declspec(noinline) sl::Result options(const sl::ViewportHandle&,const lab::rr::OptionsV3&){return sl::Result::eOk;}
// Deliberately NO begin/end plugin callbacks: this build has no reviewed profile.
__declspec(noinline) sl::Result eval(sl::Feature,const sl::FrameToken&,const sl::BaseStructure**,uint32_t,void*){
    return fail_outer?sl::Result::eErrorInvalidParameter:sl::Result::eOk;
}
// Stands in for the interposer's public slGetNativeInterface. Real Streamline
// AddRefs what it returns, and the adapter releases it, so do the same here.
sl::Result native_interface(void* proxy,void** base){
    if(native_fails)return sl::Result::eErrorInvalidParameter;
    auto* out=static_cast<IUnknown*>(native_override?native_override:proxy);
    out->AddRef();*base=out;return sl::Result::eOk;
}
struct Receiver:lab::SlAdmissionSink,lab::IResearchObserver {
    unsigned entries=0,acks=0,aborts=0,rejections=0;bool valid=true;std::uint64_t call=0;
    sl::Feature admitted_feature=0;
    void enter(const lab::slboundary::Resolution& r,const lab::rr::Packet& p,void* c)noexcept override{
        ++entries;call=r.binding.call;admitted_feature=r.binding.feature;
        valid=valid&&r.ready()&&p.valid()&&p.feature==r.binding.feature&&r.binding.frame_index==current_frame&&r.binding.command==c;
    }
    // The read-only call observation. The Evaluate's return must be
    // seen only after the controlled insertion (enter) has run inside it.
    unsigned evaluate_entries=0,evaluate_returns=0,entries_at_last_return=0;bool observe_order_error=false;std::uint64_t open=0;
    void observe_call(const lab::slboundary::Call& c,lab::ObservedPoint point)noexcept override{
        if(c.api!=lab::slboundary::Api::evaluate)return;
        if(point==lab::ObservedPoint::entering){++evaluate_entries;if(open)observe_order_error=true;open=c.id;}
        else if(point==lab::ObservedPoint::returned){++evaluate_returns;if(open!=c.id)observe_order_error=true;open=0;entries_at_last_return=entries;}
    }
    void boundary_returned(std::uint64_t c,bool ok)noexcept override{++acks;valid=valid&&ok&&c==call;}
    void aborted(std::uint64_t)noexcept override{++aborts;}
    void unavailable()noexcept override{++rejections;}
    std::string stage,reason;
    void unavailable(const lab::RejectedCall& r)noexcept override{++rejections;stage=r.stage;reason=r.reason;}
};
ComPtr<ID3D12Resource> texture(ID3D12Device* d,UINT w,UINT h,DXGI_FORMAT format){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;desc.DepthOrArraySize=1;
    desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=format;
    ComPtr<ID3D12Resource> r;hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)));return r;
}
// A RESEARCH-configured host (reviewed private discovery attached,
// so RR requires its inner evidence) that also targets DLSS super resolution.
// The private evidence exists for RR only: an SR call must be admitted on the
// public proof, and an RR call must still be refused without its inner evidence.
struct ClaimedResearch final:lab::WorkbenchResearchExtension {
    bool attach_inner_callbacks(HMODULE,const lab::json&)override{return false;}
    // Claims the reviewed hooks are installed, so the adapter REQUIRES private RR
    // evidence -- and none ever arrives, exactly the negative case to prove.
    bool install_fixture_inner_callbacks(const std::array<void*,2>&)override{return true;}
    void poll_restore(lab::WorkbenchResearchHost&)override{}
    void stop()noexcept override{}
    void describe(lab::json& s)override{s["rr_inner"]={{"installed",true},{"scope","claimed-by-fixture-never-delivered"}};s["common_restore"]={{"installed",false}};}
};
__declspec(noinline) sl::Result sr_options(const sl::ViewportHandle&,const lab::rr::SrOptionsPrefix&){return sl::Result::eOk;}
__declspec(noinline) sl::Result inner_begin(void*){return sl::Result::eOk;}
__declspec(noinline) sl::Result inner_end(void*){return sl::Result::eErrorInvalidParameter;}
int per_feature(){lab::WorkbenchAdapterHost* host=nullptr;try{
    ComPtr<IDXGIFactory4> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp;hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> command;
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command)));
    std::array<ComPtr<ID3D12Resource>,3> resources{texture(device.Get(),16,8,DXGI_FORMAT_R16G16B16A16_FLOAT),
        texture(device.Get(),8,4,DXGI_FORMAT_R32_FLOAT),texture(device.Get(),8,4,DXGI_FORMAT_R16G16_FLOAT)};
    auto* receiver=new Receiver;auto* research=new ClaimedResearch;
    host=new lab::WorkbenchAdapterHost(true,receiver,lab::WorkbenchScope::full_metadata,receiver); // Hooks pinned until process exit.
    host->set_game_profile("cyberpunk2077-rr-v1");host->set_research_extension(research);host->target_super_resolution_before_attach();
    const std::array<void*,4> api{reinterpret_cast<void*>(get),reinterpret_cast<void*>(eval),
        reinterpret_cast<void*>(constants),reinterpret_cast<void*>(legacy)};
    need(host->attach_fixture(api,reinterpret_cast<void*>(options),{reinterpret_cast<void*>(inner_begin),reinterpret_cast<void*>(inner_end)},nullptr,false,nullptr,false,
        &native_interface,reinterpret_cast<void*>(sr_options)),"Research-configured attach with an SR target");
    auto snapshot=host->snapshot();
    need(snapshot["admission_mode"]=="private-inner-callbacks-required","RR keeps requiring the reviewed private evidence");
    need(snapshot["admission_mode_sr"]=="public-evaluate-evidence-only"&&snapshot["sr_target"]==true,"SR is a target and is admitted on public evidence");
    const unsigned viewport_id=lab::profiles::game("cyberpunk2077-rr-v1")->viewport;sl::ViewportHandle viewport(viewport_id);
    lab::rr::OptionsV3 o;o.mode=1;o.width=16;o.height=8;o.pre_exposure=o.exposure_scale=1;
    o.hdr=sl::eTrue;o.flip_x=o.flip_y=o.alpha=sl::eFalse;auto volatile opt=&options;opt(viewport,o);
    lab::rr::SrOptionsPrefix so;so.mode=1;so.width=16;so.height=8;so.pre_exposure=so.exposure_scale=1;so.hdr=sl::eTrue;
    auto volatile sopt=&sr_options;sopt(viewport,so);
    auto run=[&](unsigned index,sl::Feature feature){
        sl::ViewportHandle local(viewport_id);
        host->present(1);sl::FrameToken* issued=nullptr;auto volatile g=&get;g(issued,&index);
        sl::Constants c;auto volatile s=&constants;s(c,*issued,local);
        sl::Extent ce{0,0,16,8},ge{0,0,8,4};
        sl::Resource color(sl::ResourceType::eTex2d,resources[0].Get(),0),depth(sl::ResourceType::eTex2d,resources[1].Get(),0),
            motion(sl::ResourceType::eTex2d,resources[2].Get(),0);
        sl::ResourceTag tags[]{sl::ResourceTag(&color,sl::kBufferTypeScalingOutputColor,sl::eValidUntilEvaluate,&ce),
            sl::ResourceTag(&depth,sl::kBufferTypeDepth,sl::eValidUntilEvaluate,&ge),
            sl::ResourceTag(&motion,sl::kBufferTypeMotionVectors,sl::eValidUntilEvaluate,&ge)};
        auto volatile l=&legacy;need(l(local,tags,3,command.Get())==sl::Result::eOk,"Original Tag result");
        const sl::BaseStructure* inputs[]{&local};auto volatile e=&eval;
        return e(feature,*issued,inputs,1,command.Get());
    };
    // RR on the research configuration: no inner evidence ever arrives, so it
    // is refused, and refused by the private-evidence stage.
    need(run(1,sl::kFeatureDLSS_RR)==sl::Result::eOk&&receiver->entries==0,"RR without its reviewed inner evidence is never admitted");
    need(receiver->stage=="host-contract"||receiver->stage=="inner-callback","RR refusal names the private-evidence requirement");
    const auto rr_stage=receiver->stage;
    // SR on the same host: admitted on the public proof alone.
    const auto sr=run(2,sl::kFeatureDLSS);
    if(receiver->entries!=1)throw std::runtime_error("SR not admitted; stage="+receiver->stage+" reason="+receiver->reason);
    need(sr==sl::Result::eOk&&receiver->admitted_feature==sl::kFeatureDLSS&&receiver->valid,"SR is admitted on public evidence with its own options");
    need(host->snapshot()["latest"]["feature"]=="sr"&&host->snapshot()["latest"]["admission"]["stage"]=="admitted","Admission record names the SR call");
    // The public SR proof is still the public proof: a proxy command buffer is refused.
    native_override=allocator.Get();run(3,sl::kFeatureDLSS);native_override=nullptr;
    need(receiver->entries==1&&receiver->stage=="public-command","SR still requires a native command buffer");
    need(run(4,sl::kFeatureDLSS)==sl::Result::eOk&&receiver->entries==2,"SR recovers");
    need(run(5,sl::kFeatureDLSS_RR)==sl::Result::eOk&&receiver->entries==2,"RR is still refused after SR admissions");
    need(receiver->evaluate_entries==5&&receiver->evaluate_returns==5&&!receiver->observe_order_error,"Every Evaluate observed at entry and return");
    need(receiver->entries_at_last_return==2,"The return is observed after the insertion that ran inside it");
    need(receiver->acks==2,"Each admission acknowledged");
    host->stop();hr(command->Close());
    std::cout<<"PASS per-feature checks="<<checks<<" rr_refused_stage="<<rr_stage<<" sr_admitted="<<receiver->entries
             <<" mode=private-inner-callbacks-required(rr)+public(sr) WARP_metadata_only=true NR=0\n";
    return 0;
}catch(const std::exception& e){if(host)host->stop();std::cerr<<e.what()<<'\n';return 1;}}
}
int main(int argc,char** argv){if(argc==2&&std::string(argv[1])=="per-feature")return per_feature();lab::WorkbenchAdapterHost* host=nullptr;try{
    ComPtr<IDXGIFactory4> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp;hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> command;
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command)));
    std::array<ComPtr<ID3D12Resource>,3> resources{texture(device.Get(),16,8,DXGI_FORMAT_R16G16B16A16_FLOAT),
        texture(device.Get(),8,4,DXGI_FORMAT_R32_FLOAT),texture(device.Get(),8,4,DXGI_FORMAT_R16G16_FLOAT)};
    auto* receiver=new Receiver;host=new lab::WorkbenchAdapterHost(true,receiver,lab::WorkbenchScope::full_metadata,receiver); // Hooks pinned until process exit.
    host->set_game_profile("cyberpunk2077-rr-v1");
    const std::array<void*,4> api{reinterpret_cast<void*>(get),reinterpret_cast<void*>(eval),
        reinterpret_cast<void*>(constants),reinterpret_cast<void*>(legacy)};
    // No inner callbacks and no host-flags address: exactly what a Streamline
    // build with no reviewed private profile looks like.
    need(host->attach_fixture(api,reinterpret_cast<void*>(options),{nullptr,nullptr},nullptr,false,nullptr,false,&native_interface),
        "Attaches without any reviewed begin/end profile");
    auto snapshot=host->snapshot();
    need(snapshot["admission_mode"]=="public-evaluate-evidence-only","Reports public-evidence admission");
    const unsigned viewport_id=lab::profiles::game("cyberpunk2077-rr-v1")->viewport;
    sl::ViewportHandle viewport(viewport_id);
    lab::rr::OptionsV3 o;o.mode=1;o.width=16;o.height=8;o.pre_exposure=o.exposure_scale=1;
    o.hdr=sl::eTrue;o.flip_x=o.flip_y=o.alpha=sl::eFalse;auto volatile opt=&options;opt(viewport,o);
    auto run=[&](unsigned index){
        sl::ViewportHandle local(viewport_id);
        host->present(1);sl::FrameToken* issued=nullptr;auto volatile g=&get;g(issued,&index);
        sl::Constants c;auto volatile s=&constants;s(c,*issued,local);
        sl::Extent ce{0,0,16,8},ge{0,0,8,4};
        sl::Resource color(sl::ResourceType::eTex2d,resources[0].Get(),0),depth(sl::ResourceType::eTex2d,resources[1].Get(),0),
            motion(sl::ResourceType::eTex2d,resources[2].Get(),0);
        sl::ResourceTag tags[]{sl::ResourceTag(&color,sl::kBufferTypeScalingOutputColor,sl::eValidUntilEvaluate,&ce),
            sl::ResourceTag(&depth,sl::kBufferTypeDepth,sl::eValidUntilEvaluate,&ge), // 2077 profile: hardware depth
            sl::ResourceTag(&motion,sl::kBufferTypeMotionVectors,sl::eValidUntilEvaluate,&ge)};
        auto volatile l=&legacy;need(l(local,tags,3,command.Get())==sl::Result::eOk,"Original Tag result");
        const sl::BaseStructure* inputs[]{&local};auto volatile e=&eval;
        return e(sl::kFeatureDLSS_RR,*issued,inputs,1,command.Get());
    };
    const auto first=run(1);
    if(receiver->entries!=1)throw std::runtime_error("No public admission; stage="+receiver->stage+" reason="+receiver->reason);
    need(first==sl::Result::eOk&&receiver->entries==1,"A clean public frame is admitted without any inner callback");
    need(run(2)==sl::Result::eOk&&receiver->entries==2,"Public admission repeats");
    // The public substitute for the plugin-internal native-command check.
    native_override=allocator.Get();run(3);native_override=nullptr;
    need(receiver->entries==2,"A command buffer that unwraps to a different object is refused");
    need(receiver->stage=="public-command","Rejection names the public command check");
    native_fails=true;run(4);native_fails=false;
    need(receiver->entries==2,"An unresolvable command buffer is refused, not assumed native");
    need(run(5)==sl::Result::eOk&&receiver->entries==3,"Recovery after a refused command identity");
    fail_outer=true;
    need(run(6)==sl::Result::eErrorInvalidParameter&&receiver->entries==3,"A failed outer Evaluate is never admitted");
    fail_outer=false;
    need(run(7)==sl::Result::eOk&&receiver->entries==4,"Recovery after an outer failure");
    opt(sl::ViewportHandle(viewport_id+1),o);run(8);
    need(receiver->entries==4,"Options for another viewport cannot admit this frame");
    opt(viewport,o);need(run(9)==sl::Result::eOk&&receiver->entries==5,"Matching options viewport recovers");
    snapshot=host->snapshot();
    need(snapshot["texture_reads"]==0&&snapshot["event_files"]==0,"Metadata only");
    need(snapshot["last_public_tag_call"].is_object()&&snapshot["last_public_tag_call"]["tags"].size()==3&&
         snapshot["last_public_tag_call"]["viewport"]==viewport_id&&snapshot["last_public_tag_call"]["tags"][1][0]==0,
         "Observed public tag call (viewport + types) is exposed for profile discovery");
    // The controller refuses snapshots above adapter_snapshot_limit; the host
    // must trim rather than fail. Pad the install diagnostics the way long
    // module paths / verbose hook reports would.
    const auto bytes=snapshot.dump().size();
    need(bytes<=lab::adapter_snapshot_limit&&!snapshot.contains("trimmed"),("Fixture snapshot fits the controller bound untrimmed: bytes="+std::to_string(bytes)+" trimmed="+snapshot.value("trimmed",lab::json()).dump()).c_str());
    need(lab::bound_adapter_snapshot(snapshot,lab::adapter_snapshot_limit)==snapshot,"A fitting snapshot is returned unchanged");
    auto inflated=snapshot;inflated["hooks"]["padding"]=std::string(6*1024,'x');
    const auto bounded=lab::bound_adapter_snapshot(inflated,lab::adapter_snapshot_limit);
    need(bounded.dump().size()<=lab::adapter_snapshot_limit,"Oversized snapshot is trimmed under the bound");
    need(bounded["mode"]=="admission-only"&&bounded["render_admission"]==false&&bounded["nr_executed"]==false&&
         bounded["game_control_available"]==false&&bounded["admission_mode"]=="public-evaluate-evidence-only"&&
         bounded["latest"]["binding_result"]=="ready","Required admission keys survive trimming");
    need(bounded["trimmed"].is_array()&&!bounded["trimmed"].empty()&&!bounded.contains("hooks")&&
         bounded["last_public_tag_call"].is_object(),"Trimming is recorded, removed the padded diagnostics and kept the observed tag record");
    // The trimming note counts against the bound. Many small paths over the
    // bound would overrun a fixed 384-byte reserve, and the controller would
    // then clip binding_stats whole.
    {auto many=snapshot;many["binding_stats"]["padding"]=std::string(1024,'b');
     for(const char* path:{"/admission_scope","/inner_window/scope","/latest/guide_constants/scope","/sr_evaluate/scope","/hooks/public_api/scope"}){
        const lab::json::json_pointer p(path);if(many.contains(p.parent_pointer())&&many.at(p.parent_pointer()).is_object())many[p]=std::string(160,'s');}
     many["latest"]["local_tags"]=std::string(200,'l');
     const auto fitted=lab::bound_adapter_snapshot(many,lab::adapter_snapshot_limit);
     need(fitted.dump().size()<=lab::adapter_snapshot_limit&&fitted.contains("binding_stats")&&fitted["trimmed"].is_array(),
          ("Trimmed with its own note under the bound; binding_stats survives: bytes="+std::to_string(fitted.dump().size())+" trimmed="+fitted.value("trimmed",lab::json()).dump()).c_str());}
    need(receiver->acks==5&&receiver->valid,"Every admission acknowledged with its own call id");
    need(receiver->evaluate_entries==9&&receiver->evaluate_returns==9&&!receiver->observe_order_error&&receiver->entries_at_last_return==5,
         "Read-only observer sees every Evaluate entry and return, the return after the insertion");
    host->stop();run(10);need(receiver->entries==5,"Stop forwards but never admits");
    hr(command->Close());
    std::cout<<"PASS checks="<<checks<<" admitted="<<receiver->entries<<" snapshot_bytes="<<bytes
             <<" mode=public-evaluate-evidence-only WARP_metadata_only=true game_dlls=0 NR=0 GPU_submissions=0 files=0\n";
    return 0;
}catch(const std::exception& e){if(host)host->stop();std::cerr<<e.what()<<'\n';return 1;}}
