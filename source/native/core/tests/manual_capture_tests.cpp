// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_pipe.hpp"
#include <fstream>
#include <iostream>
#include <thread>

namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F> void until(F check){const auto deadline=GetTickCount64()+5000;while(!check()){
    if(GetTickCount64()>=deadline)throw std::runtime_error("Manual capture acknowledgement timeout");Sleep(10);}}
}
int main(){try{
    auto id=lab::uuid();std::erase_if(id,[](char c){return c=='{' || c=='}' || c=='-';});
    // A fresh directory under the system temporary directory: the capture
    // backend takes its root from the caller, so nothing here names a machine.
    const auto root=std::filesystem::temp_directory_path()/lab::wide("overglaze-manual-capture-test-"+id);
    require(std::filesystem::create_directory(root),"Fresh fixture directory required");
    lab::json results=lab::json::array();
    {
        lab::Controller controller;controller.enable_manual_capture(root,"synthetic");
        lab::PipeServer server(controller);server.start();
        std::atomic<bool> feeding{true};std::jthread producer([&]{while(feeding){controller.observe_present(123);Sleep(1);}});
        struct StopFeed {std::atomic<bool>& running;std::jthread& thread;~StopFeed(){running=false;thread.join();}} stop_feed{feeding,producer};
        auto send=[&](const char* method,const char* client="manual-test"){
            auto state=controller.status();lab::json request={{"protocol","1.0"},{"method",method},{"client_id",client},{"request_id",lab::uuid()},
                {"session_id",state["session_id"]},{"expected_revision",state["revision"]},{"params",lab::json::object()}};
            if(std::string(method)=="StartCapture")request["params"]={{"kind","present_callback_cadence"},{"duration_seconds",1u},
                {"purpose","functional-verification"},{"question","independent manual capture lifecycle and idle-file checks"}};
            return request;
        };
        Sleep(100);require(std::filesystem::is_empty(root),"Idle backend wrote capture files");
        require(controller.status()["observed"]["nr_mode"]=="unknown","Capture fabricated NR state");
        auto invalid=send("StartCapture");invalid["params"]["kind"]="P0";
        require(!lab::pipe_request(GetCurrentProcessId(),invalid)["ok"].get<bool>(),"Unsupported capture kind accepted");
        for(unsigned i=0;i<20;++i){
            auto request=send("StartCapture");const auto first=lab::pipe_request(GetCurrentProcessId(),request);
            require(first["ok"]==true,"Start rejected");require(lab::pipe_request(GetCurrentProcessId(),request)==first,"Idempotent response changed");
            until([&]{return controller.status()["capture"]["state"]=="recording";});
            auto other=send("StopCapture","other-owner");require(lab::pipe_request(GetCurrentProcessId(),other)["error"]["code"]=="control_busy","Other owner changed capture");
            Sleep(25);require(lab::pipe_request(GetCurrentProcessId(),send("StopCapture"))["ok"]==true,"Stop rejected");
            until([&]{return controller.status()["capture"]["state"]=="completed";});
            auto result=controller.status()["capture"]["last_result"];
            require(result["recording_integrity"]=="complete" && result["recorded_callbacks"]>0 && result["dropped"]==0,"Bad captured stream");
            require(result["gpu_timing"]==false && result["p0_game_gate_open"]==false,"Evidence escalation");results.push_back(result);
            const auto file=std::filesystem::path(lab::wide(result["directory"].get<std::string>()))/"callback-events.jsonl";
            const auto hash=lab::sha256(file);Sleep(30);require(lab::sha256(file)==hash,"Stopped stream changed while callbacks continued");
        }
        auto timed=send("StartCapture");require(lab::pipe_request(GetCurrentProcessId(),timed)["ok"]==true,"Timed start refused");
        until([&]{return controller.status()["capture"]["state"]=="completed";});
        require(controller.status()["capture"]["last_result"]["stop_reason"]=="duration_limit","Duration stop missing");
        results.push_back(controller.status()["capture"]["last_result"]);
        auto lease=send("StartCapture");require(lab::pipe_request(GetCurrentProcessId(),lease)["ok"]==true,"Lease start refused");
        until([&]{return controller.status()["capture"]["state"]=="recording";});
        controller.frame_boundary(0,GetTickCount64()+10001);
        until([&]{return controller.status()["capture"]["state"]=="completed";});
        require(controller.status()["capture"]["last_result"]["stop_reason"]=="controller_disconnected","Disconnected capture not stopped");
        results.push_back(controller.status()["capture"]["last_result"]);server.stop();
    }
    lab::json removed=lab::json::array();
    // Only these exact, freshly generated fixture streams; never recursive deletion.
    for(const auto& result:results){
        const auto path=std::filesystem::canonical(std::filesystem::path(lab::wide(result["directory"].get<std::string>()))/"callback-events.jsonl");
        const auto relative=path.lexically_relative(std::filesystem::canonical(root));
        require(!relative.empty() && !relative.native().starts_with(L"..") && path.filename()==L"callback-events.jsonl","Cleanup scope invalid");
        require(lab::sha256(path)==result["stream_sha256"].get<std::string>(),"Functional raw file changed");
        removed.push_back({{"path",lab::utf8(path.wstring())},{"bytes",std::filesystem::file_size(path)},{"sha256",result["stream_sha256"]}});
        require(std::filesystem::remove(path),"Functional raw cleanup failed");
    }
    lab::json report={{"purpose","functional-verification"},{"status","pass"},{"origin","synthetic"},{"game_launched",false},
        {"idle_wrote_files",false},{"manual_cycles",20},{"duration_stop_checked",true},{"lease_stop_checked",true},
        {"results",results},{"removed_raw_files",removed},{"raw_payload_available",false},{"p0_game_gate_open",false},
        {"executable",lab::module_identity(nullptr)}};
    std::ofstream out(root/"acceptance.json");out<<report.dump(2);out.close();require(bool(out),"Acceptance write failed");
    std::cout<<lab::json{{"status","pass"},{"manual_cycles",20},{"raw_streams_cleaned",removed.size()},{"report",lab::utf8((root/"acceptance.json").wstring())}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
