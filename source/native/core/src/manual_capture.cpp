// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_manual_capture.hpp"
#include "lab_event_pool.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

namespace lab {
namespace {
std::uint64_t capture_qpc(){LARGE_INTEGER q{};QueryPerformanceCounter(&q);return q.QuadPart;}
constexpr std::uint64_t reserve_bytes=30ULL*1024*1024*1024,event_limit=65536,byte_limit=16ULL*1024*1024;
void require_capture(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
void new_json(const std::filesystem::path& path,const json& data){
    require_capture(!std::filesystem::exists(path),"Capture artifact already exists");
    std::ofstream file(path,std::ios::binary);file<<data.dump(2);file.close();require_capture(bool(file),"Capture artifact write failed");
}
}
struct ManualCapture::Impl {
    struct Event {std::uint64_t qpc,chain;DWORD thread;};
    struct Session {
        EventPool<Event,4096> pool;
        std::atomic<std::uint64_t> accepted{0},dropped{0};
        std::filesystem::path directory;
        std::ofstream stream;
        std::uint64_t begin=0,tick=0,written=0,bytes=0,first_chain=0;
        bool mixed_chains=false;
        std::vector<std::uint64_t> clocks;
    };
    const std::filesystem::path root;
    const std::string origin;
    std::uint64_t frequency=0;
    mutable std::mutex mutex;
    json state={{"state","idle"},{"kind","present_callback_cadence"},{"idle_event_logging",false},
        {"nr_control",false},{"gpu_timing",false},{"image_capture",false},{"p0_evidence",false},
        {"pre_capture_history_available",false},{"max_seconds",120},{"max_events",event_limit},{"max_bytes",byte_limit},
        {"completed_runs",0},{"last_result",nullptr}};
    std::optional<json> pending;
    std::string stop_reason;
    std::atomic<Session*> active{nullptr};
    std::atomic<unsigned> readers{0};
    std::unique_ptr<Session> session;
    std::jthread worker;
    std::atomic<bool> poisoned{false};
    Impl(std::filesystem::path path,std::string source):root(std::move(path)),origin(std::move(source)) {
        require_capture(root.is_absolute() && std::filesystem::is_directory(root),"Existing absolute capture root required");
        require_capture(origin=="game" || origin=="d3d12-harness" || origin=="synthetic","Invalid source origin");
        // No directory enumeration, module hashing, log allocation or file creation here.
        LARGE_INTEGER f{};QueryPerformanceFrequency(&f);frequency=f.QuadPart;
        state["origin"]=origin;state["scope"]="ReShade-present-callback-CPU-arrivals; not-native-Present-return-or-scanout";
        worker=std::jthread([this](std::stop_token token){run(token);});
    }
    ~Impl(){worker.request_stop();worker.join();
        // Production owners are process-pinned. If a callback failed to drain,
        // never free memory that it could still touch.
        if(poisoned)session.release();
    }
    void finish(const std::string& reason) {
        active=nullptr;
        const auto deadline=GetTickCount64()+50;
        while(readers.load() && GetTickCount64()<deadline)std::this_thread::yield();
        if(readers.load()){poisoned=true;std::lock_guard lock(mutex);state["state"]="failed";
            state["error"]="callback drain timed out; no restart in this host";return;}
        drain();session->stream.flush();session->stream.close();
        require_capture(bool(session->stream),"Capture stream close failed");
        auto& clocks=session->clocks;std::sort(clocks.begin(),clocks.end());std::vector<double> intervals;
        if(!session->mixed_chains)for(size_t i=1;i<clocks.size();++i)intervals.push_back(double(clocks[i]-clocks[i-1])*1000/frequency);
        std::sort(intervals.begin(),intervals.end());
        auto percentile=[&](double p)->json {return intervals.empty()?json(nullptr):json(intervals[static_cast<size_t>((intervals.size()-1)*p)]);};
        const bool clean=session->dropped==0 && session->accepted==session->written;
        json result={{"schema_version","1.0"},{"origin",origin},{"kind","present_callback_cadence"},{"stop_reason",reason},
            {"recording_integrity",clean?"complete":"incomplete"},{"begin_cpu_qpc",session->begin},{"end_cpu_qpc",capture_qpc()},
            {"qpc_frequency",frequency},{"recorded_callbacks",session->written},{"dropped",session->dropped.load()},
            {"stream_bytes",session->bytes},{"stream_sha256",sha256(session->directory/"callback-events.jsonl")},
            {"multiple_swapchain_addresses",session->mixed_chains},{"cpu_callback_interval_ms",{{"median",percentile(.5)},{"p95",percentile(.95)},{"p99",percentile(.99)}}},
            {"gpu_timing",false},{"real_frames_classified",false},{"displayed_frames_verified",false},
            {"p0_game_gate_open",false},{"pre_capture_history_available",false},
            {"warning","CPU callback intervals only; not GPU stage time, PresentMon display data, or classified real frames."},
            {"directory",utf8(session->directory.wstring())}};
        new_json(session->directory/"result.json",result);
        const auto elapsed=GetTickCount64()-session->tick;
        session.reset();
        std::lock_guard lock(mutex);state["state"]=clean?"completed":"failed";state["last_result"]=result;
        state["recorded_callbacks"]=result["recorded_callbacks"];state["stored_bytes"]=result["stream_bytes"];state["elapsed_ms"]=elapsed;
        state["completed_runs"]=state["completed_runs"].get<unsigned>()+1;stop_reason.clear();
    }
    void drain(){
        std::vector<Event> batch;batch.reserve(4096);session->pool.take_all(batch);
        for(const auto& e:batch){
            if(session->written>=event_limit){++session->dropped;continue;}
            const auto line=json{{"seq",session->written},{"cpu_qpc",e.qpc},{"thread_id",e.thread},{"swapchain_address",std::to_string(e.chain)}}.dump()+"\n";
            if(session->bytes+line.size()>byte_limit-65536){++session->dropped;continue;}
            session->stream.write(line.data(),static_cast<std::streamsize>(line.size()));require_capture(bool(session->stream),"Capture stream write failed");
            session->bytes+=line.size();++session->written;session->clocks.push_back(e.qpc);
            if(!session->first_chain)session->first_chain=e.chain;else if(session->first_chain!=e.chain)session->mixed_chains=true;
        }
    }
    void run(std::stop_token token) {
        unsigned seconds=0;
        while(!token.stop_requested() || session){
            try {
                std::optional<json> request;std::string reason;
                {std::lock_guard lock(mutex);reason=stop_reason;
                    if(pending && !token.stop_requested()){request=std::move(pending);pending.reset();}}
                if(request && !session){
                    if(!reason.empty()){std::lock_guard lock(mutex);state["state"]="cancelled";stop_reason.clear();continue;}
                    require_capture(std::filesystem::space(root).available>=reserve_bytes+byte_limit,"Insufficient 30 GiB reserve");
                    session=std::make_unique<Session>();session->clocks.reserve(event_limit);
                    auto id=uuid();std::erase_if(id,[](char c){return c=='{' || c=='}' || c=='-';});
                    session->directory=root/wide("manual-capture-"+id);
                    require_capture(std::filesystem::create_directory(session->directory),"Capture directory exists");
                    seconds=(*request)["duration_seconds"].get<unsigned>();
                    new_json(session->directory/"manifest.json",{{"schema_version","1.0"},{"purpose",(*request)["purpose"]},
                        {"question",(*request)["question"]},{"origin",origin},{"kind","present_callback_cadence"},
                        {"duration_limit_seconds",seconds},{"max_bytes",byte_limit},{"max_events",event_limit},
                        {"pre_capture_history_available",false},{"p0_game_gate_open",false},{"application",module_identity(nullptr)}});
                    session->stream.open(session->directory/"callback-events.jsonl",std::ios::binary);
                    require_capture(bool(session->stream),"Cannot open capture stream");
                    session->begin=capture_qpc();session->tick=GetTickCount64();active=session.get();
                    std::lock_guard lock(mutex);state["state"]="recording";state["directory"]=utf8(session->directory.wstring());
                    state["duration_seconds"]=seconds;state["begin_cpu_qpc"]=session->begin;state.erase("error");
                }
                if(session && !poisoned){
                    drain();
                    if(token.stop_requested())reason="host_stopped";
                    else if(session->dropped)reason="event_loss";
                    else if(session->accepted>=event_limit)reason="event_limit";
                    else if(GetTickCount64()-session->tick>=seconds*1000ULL)reason="duration_limit";
                    else if(std::filesystem::space(root).available<reserve_bytes)reason="disk_reserve";
                    if(!reason.empty())finish(reason);
                    else {std::lock_guard lock(mutex);state["recorded_callbacks"]=session->written;
                        state["stored_bytes"]=session->bytes;state["elapsed_ms"]=GetTickCount64()-session->tick;}
                }
            }catch(const std::exception& e){
                active=nullptr;
                // Keep failed-session storage/metadata alive rather than pretending it closed cleanly.
                poisoned=true;std::lock_guard lock(mutex);state["state"]="failed";state["error"]=e.what();
            }
            if(poisoned)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
};
ManualCapture::ManualCapture(std::filesystem::path root,std::string origin):impl_(std::make_unique<Impl>(std::move(root),std::move(origin))){}
ManualCapture::~ManualCapture()=default;
void ManualCapture::start(const json& p){
    require_capture(p.is_object() && p.size()==4 && p.value("kind","")=="present_callback_cadence","Only CPU callback cadence is implemented");
    require_capture(p.contains("duration_seconds") && p["duration_seconds"].is_number_unsigned()
        && p["duration_seconds"]>=1 && p["duration_seconds"]<=120,"Capture duration must be 1..120 seconds");
    require_capture(p.value("purpose","")=="functional-verification" || p.value("purpose","")=="research-evidence","Explicit capture purpose required");
    // Four user fields: kind, duration_seconds, purpose, question.
    require_capture(p.contains("question") && p["question"].is_string() && !p["question"].get<std::string>().empty()
        && p["question"].get<std::string>().size()<=256,"Bounded nonempty capture question required");
    std::lock_guard lock(impl_->mutex);const auto state=impl_->state["state"].get<std::string>();
    require_capture(!impl_->poisoned && state!="recording" && state!="starting" && state!="stopping","Capture busy or failed; no new capture started");
    impl_->pending=p;impl_->stop_reason.clear();impl_->state["state"]="starting";
    impl_->state["recorded_callbacks"]=0;impl_->state["stored_bytes"]=0;impl_->state["elapsed_ms"]=0;
}
void ManualCapture::stop(const std::string& reason){std::lock_guard lock(impl_->mutex);
    const auto s=impl_->state["state"].get<std::string>();
    if(s=="starting" || s=="recording" || s=="stopping"){impl_->stop_reason=reason;impl_->state["state"]="stopping";}
}
void ManualCapture::observe_present(std::uint64_t chain) noexcept {
    ++impl_->readers;auto* session=impl_->active.load();
    if(session){const Impl::Event e{capture_qpc(),chain,GetCurrentThreadId()};
        if(session->pool.push(e))++session->accepted;else ++session->dropped;}
    --impl_->readers;
}
json ManualCapture::snapshot() const {std::lock_guard lock(impl_->mutex);auto state=impl_->state;
    state["can_start"]=!impl_->poisoned && state["state"]!="starting" && state["state"]!="recording" && state["state"]!="stopping";return state;}
bool ManualCapture::busy() const {const auto s=snapshot()["state"].get<std::string>();return s=="starting" || s=="recording" || s=="stopping";}
}
