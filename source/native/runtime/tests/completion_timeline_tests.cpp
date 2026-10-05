// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_completion_timeline.hpp"
#include "lab_frame_retirement.hpp"
#include "lab_platform.hpp"
#include <dxgi1_6.h>
#include <iostream>
#include <memory>
#include <set>
#include <string>
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
void need(bool b,const char* what){++checks;if(!b)throw std::runtime_error(what);}
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("GPU fixture HRESULT: "+std::to_string(r));}
ComPtr<ID3D12Resource> buffer(ID3D12Device* d,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=768;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;hr(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)));return r;}
struct World {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue,other;ComPtr<ID3D12Fence> gate;
    ComPtr<ID3D12Resource> upload,readback;std::array<ComPtr<ID3D12CommandAllocator>,3> alloc;
    std::array<ComPtr<ID3D12GraphicsCommandList>,3> list;std::unique_ptr<lab::CompletionTimeline> timeline;
    ~World()=default;
};
// Exercises the SAME coordinator used by PrePostFramePipeline, with no NR here.
struct TestPipeline {
    bool idle=true;ComPtr<ID3D12Fence> fence;UINT64 value=0;unsigned retires=0,discards=0;
    bool ready()const{return idle;}
    void bind_completion(ID3D12Fence* f,UINT64 v){fence=f;value=v;}
    bool retire_if_complete(){if(fence->GetCompletedValue()<value)return false;idle=true;++retires;return true;}
    void discard_recorded(){if(idle)throw std::logic_error("nothing to discard");idle=true;fence.Reset();value=0;++discards;}
    struct Selection {bool nr_recorded=false;void* resource=nullptr;};
    Selection record(){idle=false;return {true,this};}
    struct Preparation {bool gpu_recorded=false;bool nr_recorded=false;void* resource=nullptr;};
    Preparation prepare(){idle=false;return {true,false,this};}
};
struct LeaseCounts {unsigned released=0,abandoned=0;};
// Fake ownership counters: failure tests must not require uncertain real GPU work.
struct TestLease {
    LeaseCounts* counts=nullptr;
    TestLease()=default;explicit TestLease(LeaseCounts& c):counts(&c){}
    TestLease(const TestLease&)=delete;TestLease& operator=(const TestLease&)=delete;
    TestLease(TestLease&& other)noexcept:counts(std::exchange(other.counts,nullptr)){}
    TestLease& operator=(TestLease&& other)noexcept {release();counts=std::exchange(other.counts,nullptr);return *this;}
    ~TestLease(){release();}
    void release()noexcept {if(counts){++counts->released;counts=nullptr;}}
    void abandon()noexcept {if(counts){++counts->abandoned;counts=nullptr;}}
    bool held()const noexcept{return counts!=nullptr;}
};
}
int main(){auto w=std::make_unique<World>();try {
    ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
    for(unsigned i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND)break;
        hr(a->GetDesc1(&desc));if(desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter=a;break;}}
    need(adapter!=nullptr,"NVIDIA hardware required, no WARP");hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&w->device)));
    ComPtr<ID3D12InfoQueue> info;hr(w->device.As(&info));
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr(w->device->CreateCommandQueue(&q,IID_PPV_ARGS(&w->queue)));hr(w->device->CreateCommandQueue(&q,IID_PPV_ARGS(&w->other)));
    hr(w->device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&w->gate)));
    w->upload=buffer(w->device.Get(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    w->readback=buffer(w->device.Get(),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    std::array<unsigned,192> expected{};for(unsigned i=0;i<expected.size();++i)expected[i]=i*19+11;
    void* mapped=nullptr;D3D12_RANGE empty{0,0};hr(w->upload->Map(0,&empty,&mapped));memcpy(mapped,expected.data(),768);w->upload->Unmap(0,nullptr);
    for(unsigned i=0;i<3;++i){hr(w->device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&w->alloc[i])));
        hr(w->device->CreateCommandList(0,q.Type,w->alloc[i].Get(),nullptr,IID_PPV_ARGS(&w->list[i])));
        w->list[i]->CopyBufferRegion(w->readback.Get(),i*256,w->upload.Get(),i*256,256);hr(w->list[i]->Close());}
    w->timeline=std::make_unique<lab::CompletionTimeline>(w->queue.Get());auto& t=*w->timeline;using S=lab::CompletionTimeline::State;
    need(t.snapshot().signals==0,"Idle/OFF has no completion signals");need(t.reserve(0).generation==0,"No frame zero");
    std::array<lab::CompletionTimeline::Ticket,8> tickets{};for(unsigned i=0;i<8;++i){tickets[i]=t.reserve(i+1);need(tickets[i].generation!=0,"Reserve bounded slot");}
    need(t.reserve(9).generation==0&&t.reserve(1).generation==0,"No overwrite/full/old frame");
    need(t.inspect(tickets[0])==S::reserved&&!t.retire(tickets[0]),"Reservation not completed");
    need(!t.submitted(tickets[0],w->queue.Get()),"No submitted notification before recording");
    for(unsigned i=0;i<3;++i)need(t.recording(tickets[i]),"Mark recording before submission");
    need(!t.cancel_before_recording(tickets[0]),"Recorded work cannot cancel/release");
    need(!t.submitted(tickets[1],w->queue.Get()),"Later signal cannot overtake earlier unsubmitted ticket");
    need(!t.submitted(tickets[0],w->other.Get()),"Wrong queue rejected");
    auto forged=tickets[0];++forged.frame;need(t.inspect(forged)==S::invalid&&!t.recording(forged),"Ticket content/generation checked");
    // Fixture-only deliberate queue hold; production Timeline never calls Wait.
    hr(w->queue->Wait(w->gate.Get(),1));
    for(unsigned i=0;i<3;++i){ID3D12CommandList* list=w->list[i].Get();w->queue->ExecuteCommandLists(1,&list);
        need(t.submitted(tickets[i],w->queue.Get()),"Signal after real last-consumer submission");
        need(t.inspect(tickets[i])==S::submitted&&!t.retire(tickets[i]),"CPU return not GPU completion");}
    need(t.snapshot().signals==3,"Exactly one private signal per tracked submission");
    need(!t.submitted(tickets[2],w->queue.Get())&&t.snapshot().signals==3,"Repeated notification cannot signal twice");
    for(unsigned i=3;i<8;++i)need(t.cancel_before_recording(tickets[i]),"Unused reservations cancel without GPU work");
    hr(w->gate->Signal(1));lab::Handle done(CreateEventW(nullptr,FALSE,FALSE,nullptr));need(done.valid(),"Fixture event");
    hr(t.binding_fence()->SetEventOnCompletion(tickets[2].value,done.value));need(WaitForSingleObject(done.value,5000)==WAIT_OBJECT_0,"GPU completion bounded");
    for(unsigned i=0;i<3;++i){need(t.inspect(tickets[i])==S::complete,"Real fence completion");need(t.retire(tickets[i]),"Retire once complete");need(t.inspect(tickets[i])==S::invalid&&!t.retire(tickets[i]),"Old ticket invalid after retirement");}
    D3D12_RANGE range{0,768};hr(w->readback->Map(0,&range,&mapped));need(memcmp(mapped,expected.data(),768)==0,"Three in-flight copies actually completed");w->readback->Unmap(0,&empty);
    auto next=t.reserve(9);need(next.slot==tickets[0].slot&&next.generation!=tickets[0].generation&&t.inspect(tickets[0])==S::invalid,"Reused slot rejects earlier generation");
    need(t.cancel_before_recording(next),"Cancel unrecorded reuse");
    {
        TestPipeline pipeline;lab::nr::FrameRetirement coordinator(pipeline,t);
        auto off=coordinator.record(false,10,[]{return TestPipeline::Selection{};});
        need(!off.nr_recorded&&t.snapshot().signals==3&&coordinator.ready(),"Coordinator OFF reserves/signals nothing");
        coordinator.record(true,10,[&]{return pipeline.record();});
        const auto ticket=coordinator.ticket();need(ticket.frame==10&&!coordinator.poll()&&pipeline.retires==0,"Unsubmitted coordinator cannot retire");
        bool caught=false;try{coordinator.record(false,11,[]{return TestPipeline::Selection{};});}catch(const std::logic_error&){caught=true;}
        need(caught,"Busy NR does not reuse slot even for OFF");
        need(!coordinator.last_use_submitted(11,w->queue.Get())&&!coordinator.last_use_submitted(10,w->other.Get()),"Wrong frame/queue refused");
        hr(w->queue->Wait(w->gate.Get(),2)); // Fixture gate only.
        ID3D12CommandList* list=w->list[0].Get();w->queue->ExecuteCommandLists(1,&list);
        need(coordinator.last_use_submitted(10,w->queue.Get()),"Coordinator queues real completion signal");
        need(!coordinator.last_use_submitted(10,w->queue.Get())&&!coordinator.poll()&&pipeline.retires==0,"Pending GPU/repeated notification cannot retire");
        hr(w->gate->Signal(2));hr(t.binding_fence()->SetEventOnCompletion(ticket.value,done.value));
        need(WaitForSingleObject(done.value,5000)==WAIT_OBJECT_0,"Coordinator test completion bounded");
        need(coordinator.poll()&&coordinator.ready()&&pipeline.retires==1,"Actual completion retires pipeline once");
        need(coordinator.poll()&&pipeline.retires==1&&t.inspect(ticket)==S::invalid,"Polling never acknowledges twice");
        caught=false;std::thread wrong_owner([&]{try{coordinator.poll();}catch(const std::logic_error&){caught=true;}});wrong_owner.join();
        need(caught,"Cross-thread Session ownership refused");
    }
    {
        lab::SerialCallGate serial;
        TestPipeline pipeline;
        std::unique_ptr<lab::nr::FrameRetirement<TestPipeline>> coordinator;
        {auto lease=serial.try_enter();need(bool(lease),"Acquire retirement domain");
            coordinator=std::make_unique<lab::nr::FrameRetirement<TestPipeline>>(pipeline,t,&serial);}
        std::exception_ptr error;
        std::thread recorder([&]{try{auto lease=serial.try_enter();need(bool(lease),"Migrated recorder admitted");
            coordinator->record(true,11,[&]{return pipeline.record();});
            need(!coordinator->poll(),"CPU lease is not submission/completion");
        }catch(...){error=std::current_exception();}});recorder.join();if(error)std::rethrow_exception(error);
        bool rejected=false;try{coordinator->poll();}catch(const std::logic_error&){rejected=true;}
        need(rejected,"No unleased retirement state reads");
        lab::CompletionTimeline::Ticket ticket;
        {auto lease=serial.try_enter();need(bool(lease),"Acquire pending retirement domain");
            ticket=coordinator->ticket();need(!coordinator->ready()&&pipeline.retires==0,"Changing CPU owner preserves pending slot");
            hr(w->queue->Wait(w->gate.Get(),3));ID3D12CommandList* list=w->list[0].Get();w->queue->ExecuteCommandLists(1,&list);
            need(coordinator->last_use_submitted(11,w->queue.Get())&&!coordinator->poll(),"Actual pending submission not retired");}
        hr(w->gate->Signal(3));hr(t.binding_fence()->SetEventOnCompletion(ticket.value,done.value));
        need(WaitForSingleObject(done.value,5000)==WAIT_OBJECT_0,"Migrated coordinator completion bounded");
        std::thread retire([&]{try{auto lease=serial.try_enter();need(bool(lease),"Migrated completion owner admitted");
            need(coordinator->poll()&&coordinator->ready()&&pipeline.retires==1,"Cross-owner actual completion retires once");
            need(coordinator->poll()&&pipeline.retires==1,"Migration does not duplicate acknowledgement");
        }catch(...){error=std::current_exception();}});retire.join();if(error)std::rethrow_exception(error);
    }
    {
        lab::CompletionTimeline failed_timeline(w->queue.Get());TestPipeline p;lab::nr::FrameRetirement broken(p,failed_timeline);
        bool caught=false;try{broken.record(true,1,[]()->TestPipeline::Selection{throw std::runtime_error("Foreign recording failed");});}catch(const std::runtime_error&){caught=true;}
        need(caught&&broken.failed()&&!broken.poll()&&!broken.last_use_submitted(1,w->queue.Get()),"Foreign failure terminal, no fabricated completion");
        need(failed_timeline.snapshot().recorded==1&&failed_timeline.snapshot().signals==0,"Failed recording retained until process exit");
    }
    {
        lab::CompletionTimeline unused(w->queue.Get());TestPipeline p;lab::nr::FrameRetirement broken(p,unused);
        bool caught=false;try{broken.record(false,1,[&]{return p.record();});}catch(const std::logic_error&){caught=true;}
        need(caught&&broken.failed()&&!broken.ready()&&unused.snapshot().signals==0,"Incorrect OFF recorder fails closed");
    }
    {
        lab::CompletionTimeline owned_timeline(w->queue.Get());TestPipeline p;LeaseCounts counts;
        lab::nr::FrameRetirement<TestPipeline,TestLease> owned(p,owned_timeline);
        owned.record_owned(1,TestLease(counts),[&]{return p.record();});
        need(owned.borrowed_held()&&!owned.poll()&&!counts.released&&!counts.abandoned,"Borrowed ownership survives CPU recording return");
        hr(w->queue->Wait(w->gate.Get(),4));ID3D12CommandList* list=w->list[0].Get();w->queue->ExecuteCommandLists(1,&list);
        need(owned.last_use_submitted(1,w->queue.Get())&&!owned.poll()&&owned.borrowed_held()&&!counts.released,"Submitted but blocked GPU retains borrowed ownership");
        hr(w->gate->Signal(4));hr(owned_timeline.binding_fence()->SetEventOnCompletion(owned.ticket().value,done.value));
        need(WaitForSingleObject(done.value,5000)==WAIT_OBJECT_0,"Owned completion bounded");
        need(owned.poll()&&!owned.borrowed_held()&&counts.released==1&&!counts.abandoned,"Only real completion releases borrowed lease");
        need(owned.poll()&&counts.released==1,"Repeated polling cannot release twice");
    }
    {
        lab::CompletionTimeline untouched(w->queue.Get());TestPipeline p;LeaseCounts counts;
        lab::nr::FrameRetirement<TestPipeline,TestLease> owned(p,untouched);
        bool caught=false,invoked=false;
        try{owned.record_owned(0,TestLease(counts),[&]{invoked=true;return p.record();});}catch(const std::logic_error&){caught=true;}
        need(caught&&!invoked&&owned.ready()&&!owned.borrowed_held()&&counts.released==1&&!counts.abandoned,"Pre-record reservation rejection releases unused lease");
    }
    {
        lab::CompletionTimeline uncertain(w->queue.Get());TestPipeline p;LeaseCounts counts;
        {
            lab::nr::FrameRetirement<TestPipeline,TestLease> owned(p,uncertain);bool caught=false;
            try{owned.record_owned(1,TestLease(counts),[]()->TestPipeline::Selection{throw std::runtime_error("Partial foreign recording");});}
            catch(const std::runtime_error&){caught=true;}
            need(caught&&owned.failed()&&!owned.poll()&&!counts.released&&counts.abandoned==1,"Partial recording failure abandons instead of releasing uncertain ownership");
        }
        need(!counts.released&&counts.abandoned==1&&uncertain.snapshot().signals==0,"Failure destruction neither double abandons nor fabricates completion");
    }
    {
        lab::CompletionTimeline unsubmitted(w->queue.Get());TestPipeline p;LeaseCounts counts;
        {lab::nr::FrameRetirement<TestPipeline,TestLease> owned(p,unsubmitted);
            owned.record_owned(1,TestLease(counts),[&]{return p.record();});}
        need(!counts.released&&counts.abandoned==1&&unsubmitted.snapshot().signals==0,"Shutdown cannot release recorded unsubmitted dependencies");
    }
    {
        lab::CompletionTimeline prep_timeline(w->queue.Get());TestPipeline p;LeaseCounts counts;
        lab::nr::FrameRetirement<TestPipeline,TestLease> owned(p,prep_timeline);
        const auto prepared=owned.record_owned(1,TestLease(counts),[&]{return p.prepare();});
        need(prepared.gpu_recorded&&!prepared.nr_recorded&&!owned.poll()&&owned.borrowed_held(),"Non-NR GPU work holds lease until submission");
        hr(w->queue->Wait(w->gate.Get(),5));ID3D12CommandList* list=w->list[0].Get();w->queue->ExecuteCommandLists(1,&list);
        need(owned.last_use_submitted(1,w->queue.Get())&&!owned.poll()&&!counts.released,"Non-NR work still needs real GPU completion");
        hr(w->gate->Signal(5));hr(prep_timeline.binding_fence()->SetEventOnCompletion(owned.ticket().value,done.value));
        need(WaitForSingleObject(done.value,5000)==WAIT_OBJECT_0&&owned.poll()&&counts.released==1&&p.retires==1,"Preparation completes once without pretending to Evaluate");
        lab::CompletionTimeline bad_off(w->queue.Get());TestPipeline bad;lab::nr::FrameRetirement invalid(bad,bad_off);bool rejected=false;
        try{invalid.record(false,1,[&]{return bad.prepare();});}catch(const std::logic_error&){rejected=true;}
        need(rejected&&invalid.failed()&&bad_off.snapshot().signals==0,"Ordinary OFF must reject non-NR GPU preparation too");
    }
    {
        // The game Reset the list before submitting it: the recording is gone.
        // The timeline marks it discarded (not failed), later tickets are not
        // blocked by it, the owner retires it without any fence, and the
        // coordinator RELEASES (never abandons) the borrowed lease.
        lab::CompletionTimeline dt(w->queue.Get());TestPipeline p;LeaseCounts counts;using D=lab::CompletionTimeline::Discard;
        lab::nr::FrameRetirement<TestPipeline,TestLease> owned(p,dt);
        owned.record_owned(1,TestLease(counts),[&]{return p.record();});const auto first=owned.ticket();
        auto spare=dt.reserve(2);need(spare.generation&&dt.discard_before_submission(spare)==D::refused&&dt.cancel_before_recording(spare),"Only a recorded ticket can be discarded");
        need(!owned.discard_unsubmitted()&&owned.borrowed_held(),"Nothing to discard before the observer proves the Reset");
        need(dt.discard_before_submission(first)==D::discarded&&dt.inspect(first)==S::discarded&&dt.snapshot().discarded==1,"Recorded ticket becomes discarded");
        need(dt.discard_before_submission(first)==D::refused&&!dt.submitted(first,w->queue.Get())&&!dt.retire(first),"Discarded ticket is neither resubmittable nor completable");
        need(!owned.poll()&&owned.borrowed_held()&&!owned.ready()&&!owned.failed(),"Polling never treats a discard as completion or failure");
        need(owned.discard_unsubmitted()&&owned.ready()&&!owned.borrowed_held()&&counts.released==1&&!counts.abandoned&&p.discards==1,"Owner discard releases lease and pipeline without a fence");
        need(dt.inspect(first)==S::invalid&&!dt.retire_discarded(first)&&dt.snapshot().signals==0,"Discarded ticket retired once, no Signal ever queued");
        owned.record_owned(3,TestLease(counts),[&]{return p.record();});
        hr(w->queue->Wait(w->gate.Get(),6));ID3D12CommandList* list=w->list[0].Get();w->queue->ExecuteCommandLists(1,&list);
        need(owned.last_use_submitted(3,w->queue.Get()),"A later frame submits despite the earlier discarded ticket");
        hr(w->gate->Signal(6));hr(dt.binding_fence()->SetEventOnCompletion(owned.ticket().value,done.value));
        need(WaitForSingleObject(done.value,5000)==WAIT_OBJECT_0&&owned.poll()&&counts.released==2&&p.retires==1,"Next frame completes normally after a discard");
    }
    {   // ABI21. Every refusal to signal is named separately. Collapsed into one
        // bool, the caller declared NR terminally failed -- including on a plain
        // try-lock miss against the owner thread, which is what this class
        // provokes by design (seen in Alan Wake 2).
        using Sig=lab::CompletionTimeline::Signal;
        lab::CompletionTimeline named(w->queue.Get());
        const auto early=named.reserve(1),late=named.reserve(2);
        need(early.generation&&late.generation&&named.recording(early)&&named.recording(late),"Two tickets recorded in order");
        need(named.submit_signal(late,w->queue.Get())==Sig::out_of_order,"An earlier unsubmitted ticket is named, not merged into one failure");
        need(named.submit_signal(early,w->other.Get())==Sig::wrong_queue,"A foreign queue is named");
        need(named.submit_signal(early,w->queue.Get())==Sig::ok,"The correct queue signals");
        need(named.submit_signal(early,w->queue.Get())==Sig::wrong_state,"A repeated signal is named, and never queues a second Signal");
        need(named.submit_signal(late,w->queue.Get())==Sig::ok,"The later ticket signals once the earlier one has");
        need(named.snapshot().signals==2,"Exactly two signals were queued");
        // Distinct, non-empty text for each reason: the fault record has to say
        // which one happened, which is the whole point of the change.
        std::set<std::string> names;
        for(auto s:{Sig::ok,Sig::busy,Sig::already_failed,Sig::wrong_queue,Sig::wrong_state,Sig::out_of_order,Sig::fence_overtaken,Sig::signal_failed}){
            const std::string text=lab::CompletionTimeline::signal_name(s);
            need(!text.empty()&&text!="unknown","Every signal reason has its own text");names.insert(text);
        }
        need(names.size()==8,"No two signal reasons share a name");
        hr(named.binding_fence()->SetEventOnCompletion(late.value,done.value));
        need(WaitForSingleObject(done.value,5000)==WAIT_OBJECT_0,"Both named signals reach the GPU");
        need(named.retire(early)&&named.retire(late),"Both tickets retire");
        named.fail();
        need(named.submit_signal(early,w->queue.Get())==Sig::already_failed,"A failed timeline names itself rather than reporting a generic refusal");
    }
    t.fail();need(t.reserve(10).generation==0&&!t.submitted(tickets[0],w->queue.Get()),"Terminal context does not schedule new work");
    hr(w->device->GetDeviceRemovedReason());unsigned debug_errors=0;
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;hr(info->GetMessage(i,nullptr,&bytes));std::vector<char> data(bytes);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());hr(info->GetMessage(i,m,&bytes));if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)++debug_errors;}
    need(debug_errors==0,"D3D12 debug validation");
    auto stats=t.snapshot();std::cout<<lab::json{{"passed",true},{"checks",checks},{"purpose","functional-verification"},{"origin","synthetic-d3d12-real-gpu"},
        {"gpu",lab::utf8(desc.Description)},{"copies",6},{"inflight_observed",3},{"exact_readback_bytes",768},{"signals",stats.signals},{"retired",stats.retired},{"pipeline_coordinator_test_double",true},{"serial_owner_migration",true},{"borrowed_ownership_failure_tests",true},
        {"debug_errors",debug_errors},{"raw_texture_files",0},{"nr_executed",false},{"game_control_available",false},{"p0_game_gate_open",false},
        {"production_cpu_waits",0},{"production_queue_waits",0},{"module",lab::module_identity(nullptr)}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"Completion fixture failed: "<<e.what()<<'\n';if(w&&w->gate)w->gate->Signal(5);
    // Do not tear down uncertain GPU dependencies during failure unwinding.
    (void)w.release();return 1;}}
