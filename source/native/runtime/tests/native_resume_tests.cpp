// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_native_resume.hpp"
#include "lab_frame_retirement.hpp"
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <thread>
#include <iostream>
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
void need(bool b,const char* reason){++checks;if(!b)throw std::runtime_error(reason);}
void hr(HRESULT h){if(FAILED(h))throw std::runtime_error("D3D12 HRESULT "+std::to_string(h));}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::logic_error&){caught=true;}need(caught,"Missing rejection");}
struct Pipeline {
    bool idle=true;unsigned retired=0;ComPtr<ID3D12Fence> fence;UINT64 value=0;
    struct Selection{bool nr_recorded;void* resource;};
    bool ready()const{return idle;}
    Selection record(){idle=false;return {true,this};}
    void bind_completion(ID3D12Fence* f,UINT64 v){fence=f;value=v;}
    bool retire_if_complete(){if(fence->GetCompletedValue()<value)return false;idle=true;++retired;return true;}
};
struct World {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue,other;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12RootSignature> root,poison_root;ComPtr<ID3D12PipelineState> pso,poison_pso;
    ComPtr<ID3D12Resource> output,readback;ComPtr<ID3D12Fence> gate,fixture;
    lab::resume::Context* context=nullptr;std::unique_ptr<lab::CompletionTimeline> timeline;
    ComPtr<ID3D12InfoQueue> info;lab::Handle event;DXGI_ADAPTER_DESC1 adapter_desc{};
    void wait(ID3D12Fence* f,UINT64 v){hr(f->SetEventOnCompletion(v,event.value));need(WaitForSingleObject(event.value,5000)==WAIT_OBJECT_0,"Bounded fixture completion");}
    void drain(UINT64 v){hr(queue->Signal(fixture.Get(),v));wait(fixture.Get(),v);}
    void reset(){hr(allocator->Reset());hr(list->Reset(allocator.Get(),nullptr));}
    void bind(unsigned seed){ID3D12DescriptorHeap* empty=nullptr;list->SetDescriptorHeaps(0,&empty);list->SetPipelineState(pso.Get());list->SetComputeRootSignature(root.Get());
        list->SetComputeRootUnorderedAccessView(0,output->GetGPUVirtualAddress());const UINT initial[]{seed,0};list->SetComputeRoot32BitConstants(1,2,initial,0);
        list->SetComputeRoot32BitConstant(1,64,1);list->SetComputeRoot32BitConstant(1,0xC0DEC0DE,2);}
    void poison(){list->SetComputeRootSignature(poison_root.Get());list->SetPipelineState(poison_pso.Get());list->SetComputeRootUnorderedAccessView(0,output->GetGPUVirtualAddress());
        const UINT values[]{999,64,0};list->SetComputeRoot32BitConstants(1,3,values,0);list->Dispatch(1,1,1);
        D3D12_RESOURCE_BARRIER u{};u.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;u.UAV.pResource=output.Get();list->ResourceBarrier(1,&u);}
    void consume(){list->Dispatch(1,1,1);D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={output.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&b);
        list->CopyBufferRegion(readback.Get(),0,output.Get(),0,256);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);}
    void check(unsigned seed){void* data=nullptr;D3D12_RANGE range{0,256};hr(readback->Map(0,&range,&data));
        bool exact=true;for(unsigned i=0;i<64;++i)exact &= static_cast<UINT*>(data)[i]==seed+i*7;
        D3D12_RANGE none{};readback->Unmap(0,&none);need(exact,"Native restored consumer output mismatch");}
    void init(bool completion_only=false){
        ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> adapter;
        for(unsigned i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND)break;
            hr(a->GetDesc1(&adapter_desc));if(adapter_desc.VendorId==0x10de&&!(adapter_desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter=a;break;}}
        need(adapter!=nullptr,"Real NVIDIA GPU required");hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));hr(device.As(&info));
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&other)));
        hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));hr(list->Close());
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fixture)));
        event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);need(event.valid(),"Fixture event");
        D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[0].Descriptor={0,0};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,3};
        D3D12_ROOT_SIGNATURE_DESC rd{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> bytes,error;
        hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&error));
        hr(device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&root)));
        params[1].Constants.Num32BitValues=4;hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&error));
        hr(device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&poison_root)));
        need(root.Get()!=poison_root.Get(),"Different root layouts must not alias in this fixture");
        const char* shader=R"(RWStructuredBuffer<uint> outData:register(u0);cbuffer C:register(b0){uint seed;uint limit;uint cookie;};
[numthreads(64,1,1)]void main(uint3 p:SV_DispatchThreadID){if(p.x<limit)outData[p.x]=cookie==0xC0DEC0DE?seed+p.x*7:0xBAD;})";
        ComPtr<ID3DBlob> cs;hr(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&cs,&error));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)));
        const char* poison="RWStructuredBuffer<uint> outData:register(u0);[numthreads(64,1,1)]void main(uint3 p:SV_DispatchThreadID){outData[p.x]=123456;}";
        hr(D3DCompile(poison,strlen(poison),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&cs,&error));pd.pRootSignature=poison_root.Get();pd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&poison_pso)));
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=256;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;bd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)));
        hp.Type=D3D12_HEAP_TYPE_READBACK;bd.Flags=D3D12_RESOURCE_FLAG_NONE;hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        rejects([&]{lab::resume::Context::install(list.Get(),queue.Get());});
        context=completion_only?lab::resume::Context::install_completion_only(list.Get(),queue.Get()):lab::resume::Context::install(list.Get(),queue.Get(),true);
        const lab::ComputeBindings::Parameter layout[]{{lab::ComputeBindings::Kind::uav,0},{lab::ComputeBindings::Kind::constants,3}};
        if(!completion_only)context->register_root(root.Get(),layout);timeline=std::make_unique<lab::CompletionTimeline>(queue.Get());
    }
    void debug_check(){hr(device->GetDeviceRemovedReason());unsigned errors=0;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n=0;hr(info->GetMessage(i,nullptr,&n));std::vector<char> b(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data());hr(info->GetMessage(i,m,&n));
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<m->pDescription<<'\n';}}
        need(errors==0,"Native D3D12 debug errors");}
};
}
int main(int argc,char** argv){auto w=std::make_unique<World>();try{
    std::string mode=argc>1?argv[1]:"normal";const bool completion_only=mode.starts_with("completion-");
    if(completion_only)mode=mode.substr(11);
    w->init(completion_only);auto& c=*w->context;auto& t=*w->timeline;
    Pipeline p;lab::nr::FrameRetirement coordinator(p,t);
    const unsigned frames=mode=="normal"?3:1;
    for(unsigned frame=1;frame<=frames;++frame){
        const bool on=mode!="normal" || frame!=1;w->reset();need(!c.ready(),"Reset cannot infer missing bindings");w->bind(11+frame);
        need(completion_only?c.recording_ready()&&!c.ready():c.ready(),"Actual native state/completion-only recording missing");
        const auto generation=c.generation();
        if(on){
            if(completion_only){rejects([&]{c.preserving([&]{return p.record();});});
                coordinator.record(true,frame,[&]{w->poison();return p.record();});w->bind(11+frame);}
            else coordinator.record(true,frame,[&]{return c.preserving([&]{w->poison();return p.record();});});
            rejects([&]{c.arm_last_use(t,coordinator.ticket(),generation+1);});c.arm_last_use(t,coordinator.ticket(),generation);
            need(!coordinator.poll(),"Unsubmitted context retired");rejects([&]{c.release_completed();});}
        else coordinator.record(false,frame,[]{return Pipeline::Selection{false,nullptr};});
        w->consume();hr(w->list->Close());ID3D12CommandList* lists[]{w->list.Get()};
        if(mode=="wrong-queue"){
            w->other->ExecuteCommandLists(1,lists);hr(w->other->Signal(w->fixture.Get(),1));w->wait(w->fixture.Get(),1);
            need(c.status()["terminal_fault"]==true&&t.snapshot().signals==0&&!coordinator.poll(),"Wrong actual queue must invalidate ticket");break;
        }
        if(on)hr(w->queue->Wait(w->gate.Get(),frame));
        std::thread submitter([&]{w->queue->ExecuteCommandLists(1,lists);});submitter.join();
        if(on){need(t.inspect(coordinator.ticket())==lab::CompletionTimeline::State::submitted&&!coordinator.poll(),"Native return cannot stand for completed GPU");
            hr(w->gate->Signal(frame));w->wait(t.binding_fence(),coordinator.ticket().value);
            if(mode=="duplicate"){
                w->queue->ExecuteCommandLists(1,lists);w->drain(10);
                need(c.status()["terminal_fault"]==true&&t.snapshot().signals==1&&!coordinator.poll(),"Repeated actual recording invalidates completed-but-unretired ticket");break;
            }
            need(coordinator.poll()&&coordinator.ready(),"Owner did not retire native completion");c.release_completed();
        }else w->drain(frame);
        w->check(11+frame);
    }
    if(mode=="normal"&&completion_only){
        const auto result=c.status();need(result["hooks"].size()==3&&result["restores"]==0&&result["setters"]==0&&result["injected_setters"]==0,"Completion-only path must not track or restore bindings");
        need(result["signals"]==2&&result["matched_submissions"]==2&&p.retired==2,"Completion-only OFF/ON counts");c.stop();
    }else if(mode=="normal"){
        auto result=c.status();need(result["restores"]==2&&result["signals"]==2&&result["matched_submissions"]==2&&result["callback_losses"]==0,"Native ON/OFF counts wrong");
        need(result["injected_setters"].get<unsigned>()>0&&p.retired==2,"Injected work contaminated host or missing retirement");
        w->reset();w->bind(13);w->list->SetComputeRootSignature(w->poison_root.Get());need(!c.ready(),"Unknown root must refuse capture");hr(w->list->Close());ID3D12CommandList* l=w->list.Get();w->queue->ExecuteCommandLists(1,&l);w->drain(10);
        w->reset();w->bind(14);w->list->ClearState(nullptr);need(!c.ready(),"Uncovered state must refuse capture");hr(w->list->Close());w->queue->ExecuteCommandLists(1,&l);w->drain(11);
        w->reset();w->bind(15);need(c.ready(),"Fresh recording should recover from soft unsupported state");hr(w->list->Close());w->queue->ExecuteCommandLists(1,&l);w->drain(12);
        c.stop();const auto stopped=c.status();w->reset();w->bind(16);hr(w->list->Close());w->queue->ExecuteCommandLists(1,&l);w->drain(13);need(c.status()==stopped,"Stopped observer changed state");
    }else c.stop();
    w->debug_check();auto result=c.status();std::cout<<lab::json{{"passed",true},{"purpose","functional-verification"},{"origin","synthetic-native-d3d12-real-hooks"},
        {"case",mode},{"completion_only",completion_only},{"checks",checks},{"context",result},{"gpu",lab::utf8(w->adapter_desc.Description)},{"debug_errors",0},{"raw_texture_files",0},
        {"nr_executed",false},{"manual_setter_notifications",0},{"manual_submission_notifications",0},{"cross_thread_submission",mode!="wrong-queue"},
        {"game_control_available",false},{"p0_game_gate_open",false},{"module",lab::module_identity(nullptr)}}.dump()<<'\n';
    // Context and native code are process-pinned. No hot-unload claim. All GPU
    // calls above completed; keep armed failure dependencies until process exit.
    (void)w.release();return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';if(w->gate)w->gate->Signal(100);if(w->context)w->context->stop();(void)w.release();return 1;}}
