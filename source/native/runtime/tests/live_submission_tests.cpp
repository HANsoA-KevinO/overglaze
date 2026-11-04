// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_live_submission.hpp"
#include "lab_live_submission_fixture.hpp"
#include <dxgi1_6.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
using Microsoft::WRL::ComPtr;
namespace lab {
struct CompletionTimelineTestAccess {
    static auto lock(CompletionTimeline& timeline){return std::unique_lock(timeline.mutex_);}
};
}
namespace {
// Hold the real metadata lock on another CPU thread. The GPU remains gated
// independently, so CPU publication cannot masquerade as completed GPU work.
struct TimelineHold {
    lab::Handle held{CreateEventW(nullptr,TRUE,FALSE,nullptr)},release{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::atomic<bool> expired{false};std::jthread worker;
    explicit TimelineHold(lab::CompletionTimeline& timeline):worker([&,this]{
        auto lock=lab::CompletionTimelineTestAccess::lock(timeline);SetEvent(held.value);
        expired=WaitForSingleObject(release.value,5000)!=WAIT_OBJECT_0;
    }){if(WaitForSingleObject(held.value,5000)!=WAIT_OBJECT_0)throw std::runtime_error("Contention fixture did not acquire timeline lock");}
    void unlock(){SetEvent(release.value);if(worker.joinable())worker.join();}
    ~TimelineHold(){unlock();}
};
// Pause before one native callback's metadata acquisition, then independently
// hold the real mutex. This covers both entry and return of native Execute.
struct LifecyclePause {
    lab::LiveSubmission& router;unsigned phase;
    lab::Handle entered{CreateEventW(nullptr,TRUE,FALSE,nullptr)},proceed{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::atomic<bool> expired{false};
    static void pause(void* context,unsigned at){auto& s=*static_cast<LifecyclePause*>(context);if(at!=s.phase)return;
        SetEvent(s.entered.value);s.expired=WaitForSingleObject(s.proceed.value,5000)!=WAIT_OBJECT_0;}
    LifecyclePause(lab::LiveSubmission& r,unsigned p):router(r),phase(p){lab::LiveSubmissionFixture::pause_before_lock(r,pause,this);}
    bool reached(){return WaitForSingleObject(entered.value,5000)==WAIT_OBJECT_0;}
    void resume(){SetEvent(proceed.value);}
    ~LifecyclePause(){resume();lab::LiveSubmissionFixture::pause_before_lock(router,nullptr,nullptr);}
};
struct MetadataHold {
    lab::Handle held{CreateEventW(nullptr,TRUE,FALSE,nullptr)},release{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::atomic<bool> expired{false};std::jthread worker;
    static void hold(void* context){auto& s=*static_cast<MetadataHold*>(context);SetEvent(s.held.value);s.expired=WaitForSingleObject(s.release.value,5000)!=WAIT_OBJECT_0;}
    explicit MetadataHold(lab::LiveSubmission& r):worker([&,this]{lab::LiveSubmissionFixture::with_metadata(r,hold,this);}){
        if(WaitForSingleObject(held.value,5000)!=WAIT_OBJECT_0)throw std::runtime_error("Metadata holder did not start");}
    void unlock(){SetEvent(release.value);if(worker.joinable())worker.join();}
    ~MetadataHold(){unlock();}
};
unsigned checks=0;
void need(bool b,const char* what){++checks;if(!b)throw std::runtime_error(what);}
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("D3D12 HRESULT "+std::to_string(r));}
struct World {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue,other;
    ComPtr<ID3D12Fence> gate,drain;lab::LiveSubmission* router=nullptr;
    std::array<ComPtr<ID3D12CommandAllocator>,6> alloc;
    std::array<ComPtr<ID3D12GraphicsCommandList>,6> lists;
    std::unique_ptr<lab::CompletionTimeline> timeline;
    void flush(){hr(gate->Signal(99));hr(queue->Signal(drain.Get(),1));
        lab::Handle event(CreateEventW(nullptr,FALSE,FALSE,nullptr));hr(drain->SetEventOnCompletion(1,event.value));
        need(WaitForSingleObject(event.value,5000)==WAIT_OBJECT_0,"Queue drained");
        hr(other->Signal(drain.Get(),2));hr(drain->SetEventOnCompletion(2,event.value));
        need(WaitForSingleObject(event.value,5000)==WAIT_OBJECT_0,"Other queue drained");}
    void submit(unsigned index,ID3D12CommandQueue* q=nullptr){ID3D12CommandList* p=lists[index].Get();(q?q:queue.Get())->ExecuteCommandLists(1,&p);}
};
}
int main(int argc,char** argv){auto w=std::make_unique<World>();try{
    const std::string mode=argc>1?argv[1]:"normal";
    ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> adapter;
    for(unsigned i=0;;++i){ComPtr<IDXGIAdapter1> candidate;
        if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate))==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 desc{};hr(candidate->GetDesc1(&desc));if(desc.VendorId==0x10de&&!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter=candidate;break;}}
    need(adapter!=nullptr,"NVIDIA hardware required");hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&w->device)));
    ComPtr<ID3D12InfoQueue> info;hr(w->device.As(&info));D3D12_COMMAND_QUEUE_DESC q{};
    hr(w->device->CreateCommandQueue(&q,IID_PPV_ARGS(&w->queue)));hr(w->device->CreateCommandQueue(&q,IID_PPV_ARGS(&w->other)));
    hr(w->device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&w->gate)));hr(w->device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&w->drain)));
    w->router=lab::LiveSubmission::install(w->queue.Get(),nullptr);auto& r=*w->router;
    auto contend=[&](unsigned phase,auto invoke){
        LifecyclePause pause(r,phase);lab::Handle finished(CreateEventW(nullptr,TRUE,FALSE,nullptr));std::exception_ptr failure;
        std::jthread callback([&]{try{invoke();}catch(...){failure=std::current_exception();}SetEvent(finished.value);});
        const bool reached=pause.reached();
        MetadataHold held(r);pause.resume();const bool preserved=WaitForSingleObject(finished.value,50)==WAIT_TIMEOUT;
        held.unlock();callback.join();if(failure)std::rethrow_exception(failure);
        need(reached&&!pause.expired&&!held.expired,"Controlled lifecycle callback and metadata reader ran");
        need(preserved&&!r.snapshot()["terminal_fault"].get<bool>(),"Lifecycle metadata contention must preserve its observation without a terminal fault");
    };
    for(unsigned i=0;i<6;++i){hr(w->device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&w->alloc[i])));
        hr(w->device->CreateCommandList(0,q.Type,w->alloc[i].Get(),nullptr,IID_PPV_ARGS(&w->lists[i])));}
    need(!r.observed_queue(),"Seed queue is not assumed to be the RR queue");
    need(!r.enroll(w->lists[0].Get(),false),"Cannot render before live queue probe");
    need(r.enroll(w->lists[0].Get(),true),"Enroll existing open list without invented Reset history");
    if(mode=="probe-discard"){
        hr(w->lists[0]->Close());hr(w->lists[4]->Close());hr(w->lists[0]->Reset(w->alloc[4].Get(),nullptr));
        need(r.snapshot()["failure_reason"]=="reset-before-submission"&&r.retry_discarded_probe(),"Discarded OFF probe is recoverable without any NR work");
        need(!r.snapshot()["terminal_fault"].get<bool>()&&r.enroll(w->lists[0].Get(),true),"Fresh candidate in same host after discard");
    }
    auto host_close=[&](unsigned index){HRESULT result=E_FAIL;
        if(mode=="cross-thread"){std::thread worker([&]{result=w->lists[index]->Close();});worker.join();}
        else result=w->lists[index]->Close();hr(result);};
    need(!r.release_completed(),"Recording return not submission");host_close(0);
    if(mode=="cross-thread")need(r.snapshot()["record_thread"]!=r.snapshot()["close_thread"]&&!r.snapshot()["terminal_fault"].get<bool>(),"Sequential cross-thread Close preserves recording identity");
    w->submit(0);need(r.observed_queue().Get()==w->queue.Get(),"Observe actual submission queue");
    if(mode=="reuse"){
        hr(w->lists[4]->Close());hr(w->lists[0]->Reset(w->alloc[4].Get(),nullptr));hr(w->lists[0]->Close());w->submit(0);
        need(r.observed_queue().Get()==w->queue.Get()&&!r.snapshot()["terminal_fault"].get<bool>(),"New recording before worker polling preserves probe");
        need(!r.enroll(w->lists[2].Get(),false),"Cleared pointer watch does not discard the held enrollment");
    }
    need(r.snapshot()["signals"]==0&&r.release_completed(),"OFF probe does not Signal or inject work");
    w->timeline=std::make_unique<lab::CompletionTimeline>(w->queue.Get());auto& t=*w->timeline;
    std::vector<std::jthread> lifecycle_readers;
    if(mode=="lifecycle-readers")for(unsigned i=0;i<8;++i)lifecycle_readers.emplace_back([&](std::stop_token stop){while(!stop.stop_requested()){(void)r.snapshot();(void)r.observed_queue();}});
    need(r.resolve(w->lists[1].Get()).Get()==w->lists[1].Get(),"Native command identity resolved");
    if(mode=="enrollment-contention"){
        // Read-only metadata polling races enrollment, never the GPU. Keep the
        // first accepted list owned throughout: busy and pending are distinct,
        // and neither may replace it or poison the observer.
        std::atomic<bool> go=false,stop=false;std::atomic<unsigned> readers_ready=0;
        std::vector<std::thread> readers;for(unsigned i=0;i<8;++i)readers.emplace_back([&]{++readers_ready;while(!go.load())YieldProcessor();while(!stop.load()){(void)r.snapshot();(void)r.observed_queue();}});
        while(readers_ready.load()!=8)YieldProcessor();go=true;
        unsigned busy=0,accepted=0,pending=0;
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        do{const auto result=r.try_enroll(w->lists[1].Get(),false);
            if(result==lab::LiveSubmission::Enrollment::busy)++busy;
            else if(result==lab::LiveSubmission::Enrollment::enrolled)++accepted;
            else if(result==lab::LiveSubmission::Enrollment::pending_recording)++pending;
            else{stop=true;for(auto& thread:readers)thread.join();throw std::runtime_error("Unexpected enrollment result during read-only contention");}
        }while(std::chrono::steady_clock::now()<until&&(!busy||!accepted||!pending));
        stop=true;for(auto& thread:readers)thread.join();
        need(busy&&accepted==1&&pending,"Read-only contention is observable and exactly one list acquires the lease");
        need(!r.snapshot()["terminal_fault"].get<bool>()&&r.snapshot()["enrollments"]==2,"Busy/pending enrollments neither poison nor replace the owned list");
    }else need(r.enroll(w->lists[1].Get(),false),"Different next-frame list accepted");
    need(!r.enroll(w->lists[2].Get(),false),"Pending recording cannot be replaced");
    auto ticket=t.reserve(1);need(ticket.generation&&t.recording(ticket),"Reserve/record actual frame");
    // "reset" leaves the recording UNARMED: nothing proves what that list held,
    // so a Reset before submission must stay a fault. "nr-discard" arms it.
    if(mode!="reset"){need(r.arm(t,ticket),"Arm exact frame completion");need(!r.arm(t,ticket),"Cannot arm twice");}
    if(mode=="metadata-close")contend(1,[&]{host_close(1);});else host_close(1);
    if(mode=="reset"){
        // Reset on a DIFFERENT allocator is valid D3D12, but discards the enrolled recording.
        // Unarmed: no ticket ties this list to proven NR work, so it fails closed.
        hr(w->lists[3]->Close());hr(w->lists[1]->Reset(w->alloc[3].Get(),nullptr));hr(w->lists[1]->Close());
        need(r.snapshot()["terminal_fault"]==true&&t.snapshot().signals==0&&r.snapshot()["discarded"]==false,"Unarmed discarded recording cannot certify old work");
        need(r.snapshot()["failure_reason"]=="reset-before-submission","Exact reset rejection survives diagnostics");
        need(!r.retry_discarded_probe()&&!r.release_discarded(),"NR recording is never recovered as an empty probe or released as a discard");
    }else if(mode=="nr-discard"||mode=="nr-discard-contention"){
        // Same D3D12 as "reset", but the recording is ARMED NR work: the Reset
        // proves the game abandoned the frame. Not a fault; the ticket is
        // handed to the timeline as discarded and the list is no longer watched.
        hr(w->lists[3]->Close());
        if(mode=="nr-discard-contention"){
            TimelineHold held(t);hr(w->lists[1]->Reset(w->alloc[3].Get(),nullptr));
            need(!r.snapshot()["terminal_fault"].get<bool>(),"Reset proof survives a busy timeline without a terminal fault");
            for(unsigned i=0;i<128;++i)need(!r.poll_signal(),"Discard proof remains pending while its timeline is busy");
            held.unlock();need(!held.expired&&r.poll_signal(),"Owner publishes the retained discard proof after timeline contention");
        }else hr(w->lists[1]->Reset(w->alloc[3].Get(),nullptr));
        hr(w->lists[1]->Close());
        need(!r.snapshot()["terminal_fault"].get<bool>()&&r.snapshot()["failure_reason"].is_null()&&r.snapshot()["discarded"]==true&&r.snapshot()["discards"]==1,"Armed recording Reset before submission is a discard, not a fault");
        need(t.inspect(ticket)==lab::CompletionTimeline::State::discarded&&t.snapshot().signals==0,"Timeline holds the ticket as discarded without any Signal");
        w->submit(1);need(t.snapshot().signals==0&&!r.snapshot()["terminal_fault"].get<bool>(),"The list's new contents are not certified as the old NR work");
        need(!r.release_discarded()&&!r.enroll(w->lists[2].Get(),false),"Lease held until the owner retires the discarded ticket");
        need(t.retire_discarded(ticket)&&r.release_discarded()&&!r.release_discarded(),"Owner retirement then exactly one lease release");
        need(r.enroll(w->lists[2].Get(),false),"Next frame enrolls after the discard");
        ticket=t.reserve(2);need(t.recording(ticket)&&r.arm(t,ticket),"New frame new ticket after discard");hr(w->lists[2]->Close());w->submit(2);
        lab::Handle event(CreateEventW(nullptr,FALSE,FALSE,nullptr));hr(t.binding_fence()->SetEventOnCompletion(ticket.value,event.value));need(WaitForSingleObject(event.value,5000)==WAIT_OBJECT_0,"Completion after discard");
        need(t.retire(ticket)&&r.release_completed()&&!r.snapshot()["terminal_fault"].get<bool>()&&t.snapshot().signals==1,"Frame after a discard retires normally");
    }else if(mode=="wrong-queue"){
        w->submit(1,w->other.Get());need(r.snapshot()["terminal_fault"]==true&&t.snapshot().signals==0,"Wrong native queue fails without false Signal");
        need(r.snapshot()["failure_reason"]=="unexpected-render-queue","Exact wrong-queue rejection");
    }else{
        hr(w->queue->Wait(w->gate.Get(),1));
        if(mode.starts_with("signal-contention")){
            TimelineHold held(t);w->submit(1);const auto status=r.snapshot();
            need(!held.expired&&!status["terminal_fault"].get<bool>(),"Post-submit timeline lock contention must not terminate NR");
            need(status["submitted"]==true&&status["signals"]==0&&status["completion_signal_pending"]==true,"Actual submission and completion-signal publication are distinct");
            for(unsigned retry=0;retry<128;++retry)need(!r.poll_signal(),"A retained Signal remains pending while the metadata lock is held");
            need(!r.release_completed()&&!r.enroll(w->lists[2].Get(),false),"Busy publication cannot release or replace the GPU lease");
            if(mode=="signal-contention-reuse"){
                hr(w->lists[5]->Close());hr(w->lists[1]->Reset(w->alloc[5].Get(),nullptr));hr(w->lists[1]->Close());w->submit(1);
                need(!r.snapshot()["terminal_fault"].get<bool>()&&r.snapshot()["completion_signal_pending"]==true,"Reset after real submission preserves the old pending Signal and lease");
            }
            held.unlock();need(!held.expired&&r.signal_retries()>=129,"More than the old 64 misses are absorbed without a timeout or fake completion");
            if(mode=="signal-contention-invalidated"){
                t.invalidate();need(!r.poll_signal()&&r.snapshot()["terminal_fault"]==true,"A genuinely invalidated timeline still fails closed");
                need(r.snapshot()["failure_reason"]=="timeline-already-failed"&&t.snapshot().signals==0,"Deferred failure names the true reason without queuing a false Signal");
                need(!r.release_completed()&&!r.enroll(w->lists[2].Get(),false),"Unknown GPU completion retains the lease");
            }else{
                if(mode=="signal-contention-pollers"){
                    std::atomic<bool> go=false;std::atomic<unsigned> ready=0,ok=0;std::vector<std::jthread> pollers;
                    for(unsigned i=0;i<8;++i)pollers.emplace_back([&]{++ready;while(!go.load())YieldProcessor();for(unsigned j=0;j<128;++j)if(r.poll_signal())++ok;});
                    while(ready.load()!=8)YieldProcessor();go=true;for(auto& thread:pollers)thread.join();
                    need(ok>0&&!r.snapshot()["terminal_fault"].get<bool>()&&t.snapshot().signals==1,"Concurrent callback/owner polling publishes exactly one Signal");
                }
                need(r.poll_signal()&&r.poll_signal(),"Publication recovers after contention and repeated polls are idempotent");
                need(!r.snapshot()["completion_signal_pending"].get<bool>()&&!r.snapshot()["terminal_fault"].get<bool>(),"Recovered publication clears only the pending task");
            }
        }else if(mode=="metadata-execute-entry")contend(2,[&]{w->submit(1);});
        else if(mode=="metadata-execute-return")contend(3,[&]{w->submit(1);});
        else w->submit(1);
        if(mode!="signal-contention-invalidated"){
            need(t.snapshot().signals==1&&t.inspect(ticket)==lab::CompletionTimeline::State::submitted,"Signal follows real submission, not CPU completion");
            need(!r.release_completed(),"Blocked GPU retains command lease");
            if(mode=="duplicate"){
                w->submit(1);need(r.snapshot()["terminal_fault"]==true&&t.snapshot().signals==1,"Repeated submission cannot Signal twice");
                need(r.snapshot()["failure_reason"]=="repeated-recording-submission","Exact duplicate rejection");
            }else{
                need(mode=="normal"||mode=="reuse"||mode=="cross-thread"||mode=="probe-discard"||mode=="enrollment-contention"||mode=="signal-contention"||mode=="signal-contention-reuse"||mode=="signal-contention-pollers"||mode.starts_with("metadata-")||mode=="lifecycle-readers","Known fixture mode");
                if(mode=="reuse"||mode=="metadata-reset"){
                    hr(w->lists[5]->Close());
                    if(mode=="metadata-reset")contend(0,[&]{hr(w->lists[1]->Reset(w->alloc[5].Get(),nullptr));});
                    else hr(w->lists[1]->Reset(w->alloc[5].Get(),nullptr));
                    hr(w->lists[1]->Close());w->submit(1);
                    need(!r.release_completed()&&!r.enroll(w->lists[2].Get(),false),"Recycled list still holds original in-flight lease");
                    need(!r.snapshot()["terminal_fault"].get<bool>()&&t.snapshot().signals==1,"New recording is not duplicate execution or a new NR ticket");
                }
                hr(w->gate->Signal(1));
                lab::Handle event(CreateEventW(nullptr,FALSE,FALSE,nullptr));hr(t.binding_fence()->SetEventOnCompletion(ticket.value,event.value));
                need(WaitForSingleObject(event.value,5000)==WAIT_OBJECT_0,"Bounded actual GPU completion");
                need(!r.release_completed(),"Router cannot release before pipeline owner retirement");
                if(mode=="metadata-stale-reset"){
                    hr(w->lists[5]->Close());LifecyclePause pause(r,0);HRESULT reset_hr=E_FAIL;
                    std::jthread resetter([&]{reset_hr=w->lists[1]->Reset(w->alloc[5].Get(),nullptr);});const bool reached=pause.reached();
                    const bool retired=t.retire(ticket)&&r.release_completed();const bool enrolled=r.enroll(w->lists[2].Get(),false);
                    pause.resume();resetter.join();hr(reset_hr);
                    need(reached&&!pause.expired&&retired&&enrolled,"Owner retirement and new enrollment cross a paused old Reset");
                    need(!r.snapshot()["terminal_fault"].get<bool>()&&r.snapshot()["pending"]==true,"Stale watched pointer cannot poison the next recording");
                }else{
                    need(t.retire(ticket)&&r.release_completed(),"After retirement release exactly once");
                    need(r.enroll(w->lists[2].Get(),false),"Third dynamic list enrollment");
                }
                ticket=t.reserve(2);need(t.recording(ticket)&&r.arm(t,ticket),"New frame new ticket");hr(w->lists[2]->Close());w->submit(2);
                hr(t.binding_fence()->SetEventOnCompletion(ticket.value,event.value));need(WaitForSingleObject(event.value,5000)==WAIT_OBJECT_0,"Second completion");
                need(t.retire(ticket)&&r.release_completed()&&!r.snapshot()["terminal_fault"].get<bool>(),"Second frame retired without list reuse assumptions");
            }
        }
    }
    for(auto& reader:lifecycle_readers)reader.request_stop();for(auto& reader:lifecycle_readers)reader.join();
    w->flush();const auto status=r.snapshot();r.uninstall_for_test();w->router=nullptr;
    unsigned errors=0;for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;hr(info->GetMessage(i,nullptr,&bytes));
        std::vector<char> data(bytes);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());hr(info->GetMessage(i,m,&bytes));if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<m->pDescription<<'\n';}}
    need(errors==0,"No D3D12 validation errors");
    std::cout<<lab::json{{"passed",true},{"checks",checks},{"mode",mode},{"router",status},{"raw_files",0},{"nr_executed",false},{"game_control_available",false}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';if(w->gate)w->gate->Signal(99);if(w->router)w->router->stop();(void)w.release();return 1;}}
