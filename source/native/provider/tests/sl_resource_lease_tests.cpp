// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_sl_resource_lease.hpp"
#include "lab_workbench_adapter.hpp"
#include <dxgi1_6.h>
#include <iostream>
#include <thread>
#include "lab_sl_host_contract.hpp"
using Microsoft::WRL::ComPtr;
namespace lab {
// Deterministic reproduction of the worker/callback collision. Never hold a
// production lock longer just to make this race likely in a real game.
struct WorkbenchAdapterTestAccess {
    template<class F> static void while_worker_locked(WorkbenchAdapter& adapter,F callback){
        std::lock_guard lock(adapter.mutex_);std::thread thread(callback);thread.join();
    }
};
}
namespace {
unsigned checks=0;
struct Receiver:lab::SlAdmissionSink,lab::IResearchObserver {
    unsigned entries=0,acks=0,rejected=0,aborts=0;std::uint64_t call=0;
    bool valid=true;lab::WorkbenchAdapter* adapter=nullptr;
    lab::RejectedCall rejection;
    void enter(const lab::slboundary::Resolution& r,const lab::rr::Packet& p,void* command) noexcept override {
        ++entries;call=r.binding.call;valid=valid&&r.ready()&&p.valid()&&command==r.binding.command;
        // Query is intentionally reentrant: dispatch must be outside adapter lock.
        try {valid=valid&&adapter->snapshot()["inner_window"]["outer_return_succeeded"]==true;
            valid=valid&&static_cast<ID3D12Resource*>(r.binding.resources[0].native)->GetDesc().Width==1280;
        }catch(...){valid=false;}
    }
    void boundary_returned(std::uint64_t id,bool ok) noexcept override {++acks;valid=valid&&id==call&&ok;}
    void aborted(std::uint64_t) noexcept override {++aborts;}
    void unavailable() noexcept override {++rejected;}
    void unavailable(const lab::RejectedCall& why) noexcept override {++rejected;rejection=why;}
};
__declspec(noinline) sl::Result option_set(const sl::ViewportHandle&,const lab::rr::OptionsV3&){return sl::Result::eOk;}
void need(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
void hr(HRESULT v){if(FAILED(v))throw std::runtime_error("D3D12 HRESULT "+std::to_string(v));}
ComPtr<ID3D12Resource> texture(ID3D12Device* device,UINT width,UINT height,DXGI_FORMAT format){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=1;d.MipLevels=1;
    d.Format=format;d.SampleDesc.Count=1;ComPtr<ID3D12Resource> r;
    hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)));return r;
}
}
int main(){try{
    ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
    for(unsigned i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND)break;
        hr(a->GetDesc1(&desc));if(desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter=a;break;}}
    need(adapter!=nullptr,"NVIDIA device, no WARP");ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info;hr(device.As(&info));ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> command;
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
    hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&command)));
    using namespace lab::slboundary;
    Resolution r{Rejection::none,{}};auto& b=r.binding;b.call=4;b.command=command.Get();b.frame_index=1;b.token_generation=1;
    b.viewport=0;b.thread=GetCurrentThreadId();b.present_epoch=1;b.tag_calls={3,3,3};
    std::array<ComPtr<ID3D12Resource>,3> resources{texture(device.Get(),1280,720,DXGI_FORMAT_R16G16B16A16_FLOAT),
        texture(device.Get(),640,360,DXGI_FORMAT_R32_TYPELESS),texture(device.Get(),640,360,DXGI_FORMAT_R16G16B16A16_FLOAT)};
    for(unsigned i=0;i<3;++i){b.resources[i].native=resources[i].Get();b.resources[i].state=0;}
    auto lease=ResourceLease::acquire(r);need(lease.held()&&lease.facts().inspected&&lease.facts().same_device,"Acquire actual COM resources at entry");
    need(lease.matches(r),"Exact return matches");auto wrong=r;++wrong.binding.call;need(!lease.matches(wrong),"Call mismatch refused");
    wrong=r;++wrong.binding.token_generation;need(!lease.matches(wrong),"Token reuse refused");
    wrong=r;++wrong.binding.tag_calls[1];need(!lease.matches(wrong),"Tag revision mismatch refused");
    wrong=r;wrong.rejection=Rejection::stale;need(!lease.matches(wrong),"Stale return refused");
    auto bad=r;bad.binding.resources[1].extent={0,0,1280,720};auto bad_lease=ResourceLease::acquire(bad);
    need(!bad_lease.facts().inspected&&bad_lease.facts().reason==std::string("tag-extent-mismatch"),"Declared sizes cannot replace actual GetDesc");
    bad_lease={};bad=r;bad.binding.resources[1].native=command.Get();bad_lease=ResourceLease::acquire(bad);
    need(!bad_lease.facts().inspected,"Wrong COM interface refused");bad_lease={};
    // A shape refusal names its role and still describes every role (Plague Tale).
    {D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
     d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=640;d.Height=360;d.DepthOrArraySize=1;d.MipLevels=2;d.Format=DXGI_FORMAT_R32_TYPELESS;d.SampleDesc.Count=1;
     ComPtr<ID3D12Resource> mipped;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&mipped)));
     auto shaped=r;shaped.binding.resources[1].native=mipped.Get();auto l=ResourceLease::acquire(shaped);
     need(!l.facts().inspected&&l.facts().reason==std::string("unsupported-resource-shape")&&l.facts().failed_role==1,"A two-mip depth is refused and named");
     need(l.facts().described[0]&&l.facts().described[1]&&l.facts().described[2],"Every role is still described");
     const auto j=describe(l.facts());
     need(j.value("failed_role","")=="depth"&&j["resources"].size()==3&&j["resources"][1].value("mips",0u)==2u,"The refusal reports the full shape");}
    // Adapter acquires references at entry; the simulated host then releases its
    // references during Evaluate. Returned descriptors are not read from stale pointers.
    // The options hook is process-pinned; its owner deliberately lives until exit.
    auto* option_watch=new lab::rr::Watch;lab::json option_hooks;
    need(option_watch->install(reinterpret_cast<void*>(option_set),option_hooks),"Options hook installed");
    auto volatile set_option=reinterpret_cast<lab::rr::SetOptions*>(option_set);
    sl::ViewportHandle viewport(0u);lab::rr::OptionsV3 options;
    options.mode=1;options.width=1280;options.height=720;options.pre_exposure=1;options.exposure_scale=1;
    options.hdr=sl::eTrue;options.flip_x=options.flip_y=options.alpha=sl::eFalse;
    need(set_option(viewport,options)==sl::Result::eOk,"Accepted options request");
    lab::WorkbenchAdapter workbench(option_watch,true);workbench.present(1);
    Call token;token.api=Api::token;token.id=1;token.token=reinterpret_cast<void*>(0x1000);token.frame_index_known=true;token.frame_index=1;token.result=sl::Result::eOk;
    workbench.returned(token);Call constants=token;constants.api=Api::constants;constants.id=2;constants.thread=b.thread;
    constants.inputs.viewport_present=true;constants.inputs.viewport=0;constants.inputs.constants_present=true;workbench.returned(constants);
    Call tags=constants;tags.api=Api::tags;tags.id=3;tags.inputs.constants_present=false;tags.inputs.tag_count=3;
    const sl::BufferType roles[]{sl::kBufferTypeScalingOutputColor,sl::kBufferTypeDepth,sl::kBufferTypeMotionVectors};
    for(unsigned i=0;i<3;++i){tags.inputs.tags[i]=b.resources[i];tags.inputs.tags[i].type=roles[i];tags.inputs.tags[i].lifecycle=sl::eValidUntilEvaluate;}
    workbench.returned(tags);Call evaluate=tags;evaluate.id=4;evaluate.api=Api::evaluate;evaluate.feature=sl::kFeatureDLSS_RR;evaluate.command=command.Get();evaluate.inputs.tag_count=0;
    workbench.entering(evaluate);need(workbench.snapshot()["cpu_resource_lease_pending"]==true,"Entry actually holds resources");
    InnerCall inner;inner.phase=InnerCall::Phase::end;inner.command=command.Get();inner.frame=1;inner.viewport=0;
    inner.metadata_valid=inner.sequence_valid=true;inner.result=sl::Result::eOk;
    workbench.inner_returned(evaluate,inner);
    auto inner_status=workbench.snapshot()["inner_window"];
    need(inner_status["candidates"]==1&&inner_status["metadata_matched"]==true&&inner_status["native_command_matched"]==true,"Inner window requires real entry lease and unchanged options");
    need(inner_status["runtime_restoration_verified"]==false&&inner_status["nr_executed"]==false,"Inner association not state restoration or NR execution");
    RestoreCall restore;restore.command=command.Get();restore.result=0;restore.caller_verified=restore.sequence_valid=true;
    workbench.restore_returned(evaluate,restore);inner_status=workbench.snapshot()["inner_window"];
    need(inner_status["restore_call_matched"]==true&&inner_status["restore_result"]==0,"Restore receipt pairs with still-live entry resources and options");
    need(inner_status["outer_return_succeeded"]==false&&inner_status["full_state_restoration_verified"]==false,"Restore function return is not outer success or full state proof");
    restore.sequence_valid=false;workbench.restore_returned(evaluate,restore);
    need(workbench.snapshot()["inner_window"]["restore_call_matched"]==false,"Rejected hook sequence cannot become a restore match");restore.sequence_valid=true;
    restore.result=1;workbench.restore_returned(evaluate,restore);
    need(workbench.snapshot()["inner_window"]["restore_call_matched"]==false,"Failed restore function is not accepted");restore.result=0;
    auto wrong_inner=inner;wrong_inner.command=reinterpret_cast<void*>(0x5678);workbench.inner_returned(evaluate,wrong_inner);
    need(workbench.snapshot()["inner_window"]["candidates"]==1,"Unverified wrapper/native command refused");
    wrong_inner=inner;++wrong_inner.frame;workbench.inner_returned(evaluate,wrong_inner);
    need(workbench.snapshot()["inner_window"]["candidates"]==1,"Wrong inner frame refused");
    set_option(viewport,options);workbench.inner_returned(evaluate,inner);
    need(workbench.snapshot()["inner_window"]["candidates"]==1,"Even identical options update invalidates entry revision");
    workbench.restore_returned(evaluate,restore);
    need(workbench.snapshot()["inner_window"]["restore_call_matched"]==false,"Changed options cannot inherit successful restoration");
    for(auto& resource:resources)resource.Reset();
    need(lease.resource(1)->GetDesc().Width==640,"Lease survives host dropping original refs");lease={};
    workbench.returned(evaluate);auto s=workbench.snapshot();
    need(s["cpu_resource_lease_pending"]==false&&s["resource_entry"]["entry_return_matched"]==true,"CPU lease retired at matched return without stale query");
    need(s["resource_entry"]["resources"][0]["width"]==1280&&s["resource_entry"]["resources"][1]["width"]==640,"Actual output/guide dimensions reach worker");
    need(s["resource_entry"]["gpu_lifetime_verified"]==false&&s["render_admission"]==false,"COM lifetime not GPU lifetime or admission");
    need(s["inner_window"]["outer_return_succeeded"]==true,"Outer API success recorded separately from invalidated restore association");
    workbench.restore_returned(evaluate,restore);
    need(workbench.snapshot()["inner_window"]["restore_call_matched"]==false,"Late receipt cannot use resources already released at CPU return");
    need(!s["resource_entry"].contains("native")&&!s["resource_entry"].contains("command"),"Worker gets no stale pointers");
    resources={texture(device.Get(),1280,720,DXGI_FORMAT_R16G16B16A16_FLOAT),
        texture(device.Get(),640,360,DXGI_FORMAT_R32_TYPELESS),texture(device.Get(),640,360,DXGI_FORMAT_R16G16B16A16_FLOAT)};
    for(unsigned i=0;i<3;++i)tags.inputs.tags[i].native=resources[i].Get();
    auto enter_frame=[&](unsigned base){
        workbench.present(1);token.id=base;token.frame_index=base;workbench.returned(token);
        constants.id=base+1;workbench.returned(constants);tags.id=base+2;workbench.returned(tags);
        evaluate.id=base+3;workbench.entering(evaluate);
        need(workbench.snapshot()["cpu_resource_lease_pending"]==true,"Fresh invocation owns a CPU lease");
    };
    // Real resources, but an extent larger than the allocation. Holding COM
    // references alone must not admit this inner callback.
    tags.inputs.tags[1].extent={0,0,1280,720};enter_frame(5);inner.frame=5;
    workbench.inner_returned(evaluate,inner);workbench.returned(evaluate);tags.inputs.tags[1].extent={};
    need(workbench.snapshot()["inner_window"]["candidates"]==1,"Uninspected descriptor facts cannot become an inner candidate");
    sl::ViewportHandle other_viewport(1u);set_option(other_viewport,options);enter_frame(6);inner.frame=6;
    workbench.inner_returned(evaluate,inner);workbench.returned(evaluate);set_option(viewport,options);
    need(workbench.snapshot()["inner_window"]["candidates"]==1,"Options from another viewport cannot be paired");
    enter_frame(10);
    lab::WorkbenchAdapterTestAccess::while_worker_locked(workbench,[&]{workbench.returned(evaluate);});
    auto collision=workbench.snapshot();
    need(collision["callback_losses"]==1&&collision["cpu_resource_lease_pending"]==false,"Busy return cannot wedge lease; worker reclaims only completed invocation");
    need(collision["current_metadata_candidate"]==false,"Missed metadata is not promoted after reclamation");
    enter_frame(20);workbench.returned(evaluate);auto recovered=workbench.snapshot();
    need(recovered["resource_entry"]["entry_return_matched"]==true&&recovered["cpu_resource_lease_pending"]==false,"Next frame descriptors recover without process restart");
    need(recovered["latest"]["call"]==23&&recovered["callback_losses"]==1,"Recovered sample has new call and preserves loss history");
    enter_frame(30);
    auto old_evaluate=evaluate;
    lab::WorkbenchAdapterTestAccess::while_worker_locked(workbench,[&]{workbench.aborted(evaluate);});
    need(!workbench.snapshot()["cpu_resource_lease_pending"].get<bool>(),"Busy abort is reclaimed without GPU wait");
    enter_frame(40);workbench.aborted(old_evaluate);
    need(workbench.snapshot()["cpu_resource_lease_pending"]==true,"Late old abort does not retire new invocation");
    workbench.aborted(evaluate);need(workbench.snapshot()["cpu_resource_lease_pending"]==false,"Matching abort releases current CPU lease");
    enter_frame(50);workbench.stop();
    lab::WorkbenchAdapterTestAccess::while_worker_locked(workbench,[&]{workbench.returned(evaluate);});
    need(workbench.snapshot()["cpu_resource_lease_pending"]==false&&workbench.snapshot()["stopped"]==true,"Late stopped return can retire but never republish");
    // Controlled route uses the actual accepted host preference, not presence
    // or absence of a synthetic common restore. No NR work in this test.
    Receiver receiver;lab::WorkbenchAdapter controlled(option_watch,true,&receiver);receiver.adapter=&controlled;
    std::uint64_t flags=0;controlled.set_host_flags_address(&flags);
    auto begin_controlled=[&](unsigned base){controlled.present(1);token.id=base;token.frame_index=base;controlled.returned(token);
        constants.id=base+1;controlled.returned(constants);tags.id=base+2;controlled.returned(tags);
        evaluate.id=base+3;evaluate.result=sl::Result::eOk;controlled.entering(evaluate);inner.frame=base;
        controlled.inner_returned(evaluate,inner);
    };
    need(!lab::slhost::read_host_contract(nullptr).host_rebind_required()&&
        !lab::slhost::read_host_contract(reinterpret_cast<void*>(1)).readable,"Unreadable policy never grants rendering");
    begin_controlled(100);controlled.returned(evaluate);need(receiver.entries==0&&receiver.rejected==1,"No explicit host obligation, no injection");
    flags=4;begin_controlled(110);need(receiver.entries==0,"Wait for complete outer result, not just RR inner return");
    controlled.returned(evaluate);need(receiver.entries==1&&receiver.acks==1&&receiver.valid,"Manual host path works WITHOUT common restore");
    need(controlled.snapshot()["inner_window"]["restore_call_matched"]==false&&
        controlled.snapshot()["host_rebind_contract"]["actual_host_rebind_observed"]==false,"Host obligation is not fabricated restoration evidence");
    flags=1;begin_controlled(120);controlled.restore_entering(evaluate,restore);controlled.restore_returned(evaluate,restore);
    need(receiver.entries==1,"Optional common branch does not independently execute NR");
    controlled.returned(evaluate);need(receiver.entries==2&&receiver.acks==2,"Explicit disabled tracking uses host-rebind contract too");
    begin_controlled(130);flags=5;controlled.returned(evaluate);need(receiver.entries==2,"Mid-call preferences change refused even if both require host rebind");
    begin_controlled(140);evaluate.result=sl::Result::eErrorInvalidParameter;controlled.returned(evaluate);
    need(receiver.entries==2,"Outer failure never injects after a successful inner callback");
    begin_controlled(150);set_option(viewport,options);controlled.returned(evaluate);need(receiver.entries==2,"Mid-call options update refused");
    need(receiver.rejection.call==evaluate.id&&receiver.rejection.frame==150&&std::string(receiver.rejection.stage)=="RR-options"&&std::string(receiver.rejection.reason)=="options-changed-during-evaluate","Exact rejected invocation and options reason reach live receiver");
    begin_controlled(160);wrong_inner=inner;wrong_inner.sequence_valid=false;controlled.inner_returned(evaluate,wrong_inner);controlled.returned(evaluate);
    need(receiver.entries==2,"Invalidated or duplicate inner sequence refused at outer return");
    begin_controlled(170);controlled.aborted(evaluate);need(receiver.entries==2&&receiver.aborts==1,"Aborted host invocation never injects");
    const auto prior_losses=controlled.snapshot()["callback_losses"].get<unsigned>();
    begin_controlled(180);lab::WorkbenchAdapterTestAccess::while_worker_locked(controlled,[&]{controlled.returned(evaluate);});
    need(receiver.entries==2&&controlled.snapshot()["callback_losses"]==prior_losses+1,"Lost outer metadata does not trigger rendering");
    begin_controlled(190);controlled.returned(evaluate);
    need(receiver.entries==3&&controlled.snapshot()["callback_losses"]==prior_losses+1,"Fresh complete call recovers after startup abort and metadata loss, without erasing loss history");
    begin_controlled(200);
    lab::WorkbenchAdapterTestAccess::while_worker_locked(controlled,[&]{controlled.inner_returned(evaluate,inner);});
    controlled.returned(evaluate);need(receiver.entries==3,"Loss inside current invocation must still veto rendering");
    begin_controlled(210);controlled.returned(evaluate);need(receiver.entries==4,"Next complete call recovers after current-call veto");
    begin_controlled(220);controlled.invalidate();controlled.returned(evaluate);
    need(receiver.entries==4,"Preparation invalidation rejects a call already in progress");
    need(receiver.rejection.call==evaluate.id&&std::string(receiver.rejection.stage)=="frozen-binding"&&std::string(receiver.rejection.reason)=="snapshot-invalidated; present-boundary","Preparation boundary invalidation reaches frozen first-error receipt");
    begin_controlled(230);controlled.returned(evaluate);need(receiver.entries==5,"Fresh tags/constants after preparation remain usable");
    // A Present between entry and return still invalidates that call (present
    // boundary); a single swapchain change alone is a recreation, not ambiguity.
    begin_controlled(240);controlled.present(2);controlled.returned(evaluate);
    need(receiver.entries==5&&controlled.snapshot()["multiple_swapchains"]==false,"A single swapchain change is a recreation, not two presentation sources");
    // begin_controlled presents chain 1 again: A,B,A within the window = ambiguous.
    begin_controlled(250);controlled.returned(evaluate);
    need(receiver.entries==5&&controlled.snapshot()["multiple_swapchains"]==true,"Chains presenting alternately make frame association ambiguous");
    for(unsigned i=0;i<300;++i)controlled.present(1);
    need(controlled.snapshot()["multiple_swapchains"]==false,"A long run on one chain clears the ambiguity");
    begin_controlled(260);controlled.returned(evaluate);need(receiver.entries==6,"Admission resumes after the ambiguity clears");
    need(receiver.valid&&receiver.acks==receiver.entries,"Every admitted entry receives exactly one boundary receipt");
    controlled.stop();
    Receiver missing_receiver;lab::WorkbenchAdapter missing(option_watch,true,&missing_receiver);missing_receiver.adapter=&missing;
    missing.set_host_flags_address(&flags);
    auto missing_frame=[&](unsigned base,bool ok,bool lose){missing.present(1);token.id=base;token.frame_index=base;missing.returned(token);
        tags.id=base+2;missing.returned(tags);evaluate.id=base+3;evaluate.result=sl::Result::eOk;missing.entering(evaluate);
        if(lose)lab::WorkbenchAdapterTestAccess::while_worker_locked(missing,[&]{missing.inner_returned(evaluate,inner);});
        evaluate.result=ok?sl::Result::eOk:sl::Result::eErrorInvalidParameter;missing.returned(evaluate);};
    missing_frame(300,true,false);
    need(missing_receiver.entries==0&&missing_receiver.rejection.disposition==lab::RejectedDisposition::constants_missing_before_nr&&missing_receiver.rejection.frame==300,"Exact successful RR without constants can bypass before any NR recording");
    missing_frame(310,false,false);
    need(missing_receiver.rejection.disposition==lab::RejectedDisposition::skipped_before_insertion,"Failed outer return is skippable, never the constants bypass");
    missing_frame(320,true,true);
    need(missing_receiver.rejection.disposition==lab::RejectedDisposition::skipped_before_insertion,"Metadata loss is skippable, never mistaken for the constants bypass");
    evaluate.concurrent=true;evaluate.overlapped_api=2;evaluate.overlapped_thread=77;missing_frame(330,true,false);
    need(missing_receiver.entries==0&&missing_receiver.rejection.disposition==lab::RejectedDisposition::skipped_before_insertion&&missing_receiver.rejection.frame==330&&
         std::string(missing_receiver.rejection.reason).rfind("overlapping-api-call",0)==0&&std::string(missing_receiver.rejection.reason).find("other=tags@77")!=std::string::npos,
         "Concurrent successful RR is skippable before any NR recording and names the other call");
    missing_frame(340,false,false);
    need(missing_receiver.rejection.disposition==lab::RejectedDisposition::skipped_before_insertion,"A failed outer RR call with nothing inserted is skippable");
    evaluate.concurrent=false;evaluate.overlapped_api=UINT32_MAX;evaluate.parent=1;missing_frame(350,true,false);
    need(missing_receiver.rejection.disposition==lab::RejectedDisposition::terminal,"Nested call stays terminal");evaluate.parent=0;
    missing.stop();option_watch->stop();hr(command->Close());unsigned errors=0;
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;hr(info->GetMessage(i,nullptr,&bytes));std::vector<char> data(bytes);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());hr(info->GetMessage(i,message,&bytes));if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)++errors;}
    need(!errors,"D3D12 debug validation");
    std::cout<<lab::json{{"passed",true},{"checks",checks},{"gpu",lab::utf8(desc.Description)},{"debug_errors",errors},
        {"gpu_submissions",0},{"raw_texture_files",0},{"nr_executed",false},{"actual_sl_loaded",false},{"adapter",s}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
