// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_pipe.hpp"
#include <iostream>
#include <chrono>
#include <algorithm>

#define REQUIRE(expr) do { if (!(expr)) throw std::runtime_error("FAILED: " #expr); } while (false)

void test_observer_scene_arming(){
    lab::Controller observer;
    auto make=[&](const char* id){return lab::json{{"protocol","1.0"},{"method","ArmObservation"},{"request_id",id},{"client_id","scene-owner"},
        {"session_id",observer.status()["session_id"]},{"expected_revision",observer.status()["revision"]},
        {"params",{{"scene_confirmed",true},{"scene_id","synthetic-scene"}}}};};
    REQUIRE(observer.handle(make("disabled"),1)["ok"]==false);
    observer.enable_observer_sampling();
    REQUIRE(observer.handle(make("no-source"),2)["ok"]==false);
    observer.publish_observer({{"origin","d3d12-harness"},{"recording",true}});
    auto bad=make("not-confirmed");bad["params"]["scene_confirmed"]=false;
    REQUIRE(observer.handle(bad,3)["error"]["code"]=="scene_required");
    auto stale=make("stale");stale["session_id"]="wrong";
    REQUIRE(observer.handle(stale,4)["error"]["code"]=="stale_session");
    auto request=make("arm");auto accepted=observer.handle(request,10);
    REQUIRE(accepted["ok"]==true);
    REQUIRE(observer.handle(request,11)==accepted);
    auto other=make("other");other["client_id"]="other";
    REQUIRE(observer.handle(other,12)["error"]["code"]=="control_busy");
    auto consumed=observer.take_observer_sampling_request(15);
    REQUIRE(consumed && (*consumed)["scene_id"]=="synthetic-scene" && (*consumed)["runtime_scene_verified"]==false);
    REQUIRE(!observer.take_observer_sampling_request(16));
    observer.acknowledge_observer_sampling({{"test_clock",17}});
    REQUIRE(observer.status()["observer_sampling"]["state"]=="warmup");
    REQUIRE(observer.handle(make("duplicate-arm"),18)["error"]["code"]=="already_armed");
    REQUIRE(observer.status()["observed"]["nr_mode"]=="unknown");
    REQUIRE(observer.status()["p0_gate_open"]==false);
    auto nr=make("nr");nr["method"]="ApplyConfig";
    REQUIRE(observer.handle(nr,19)["error"]["code"]=="unsupported");
    observer.publish_observer({{"recording",false},{"stop_reason","duration_limit"}});
    REQUIRE(observer.status()["observer_sampling"]["state"]=="stopped_after_arm");
    REQUIRE(observer.status()["capabilities"]["observer_sampling_arm"]==false);
    lab::Controller expired;expired.enable_observer_sampling();expired.publish_observer({{"recording",true}});
    auto pending=make("expire");pending["session_id"]=expired.status()["session_id"];pending["expected_revision"]=std::uint64_t(0);
    REQUIRE(expired.handle(pending,1)["ok"]==true);
    REQUIRE(!expired.take_observer_sampling_request(10002));
    REQUIRE(expired.status()["observer_sampling"]["state"]=="cancelled_before_backend");
    pending["request_id"]="retry-new";pending["expected_revision"]=expired.status()["revision"];
    REQUIRE(expired.handle(pending,10003)["ok"]==true);
    REQUIRE(expired.take_observer_sampling_request(10004).has_value());
    lab::Controller stopped;stopped.enable_observer_sampling();stopped.publish_observer({{"recording",true}});
    auto stop_request=make("stop-pending");stop_request["session_id"]=stopped.status()["session_id"];stop_request["expected_revision"]=std::uint64_t(0);
    REQUIRE(stopped.handle(stop_request,1)["ok"]==true);
    stopped.publish_observer({{"recording",false},{"stop_reason","scene_ready_timeout"}});
    REQUIRE(stopped.status()["observer_sampling"]["state"]=="stopped_before_scene");
    REQUIRE(stopped.status()["observer_sampling"]["stop_reason"]=="scene_ready_timeout");
    REQUIRE(!stopped.take_observer_sampling_request(2));
    REQUIRE(stopped.status()["capabilities"]["observer_sampling_arm"]==false);
}

int main() {
    try {
        test_observer_scene_arming();
        lab::Controller c(true);
        auto make = [&](const char* method, const char* id) {
            return lab::json{{"protocol", "1.0"}, {"request_id", id}, {"client_id", "test"},
                {"method", method}, {"session_id", c.status()["session_id"]},
                {"expected_revision", c.status()["revision"]}};
        };
        auto req = make("ApplyConfig", "1"); req["params"] = {{"config", {{"nr_mode", "on"}}}};
        auto first = c.handle(req, 100);
        REQUIRE(first["ok"] == true);
        REQUIRE(c.status()["observed"]["nr_mode"] == "unknown");
        REQUIRE(c.handle(req, 101) == first);
        auto changed = req; changed["params"]["config"]["nr_mode"] = "off";
        REQUIRE(c.handle(changed, 102)["error"]["code"] == "id_reused");
        c.frame_boundary(42, 110);
        REQUIRE(c.status()["observed"]["nr_mode"] == "on");
        REQUIRE(c.status()["applied_frame"] == 42);
        auto stale = make("Arm", "2"); stale["expected_revision"] = std::uint64_t(0);
        REQUIRE(c.handle(stale, 120)["error"]["code"] == "stale_revision");
        auto arm = make("Arm", "3"); arm["params"] = {{"scene_confirmed", true}};
        REQUIRE(c.handle(arm, 130)["ok"] == true);
        auto other = make("Start", "4"); other["client_id"] = "other";
        REQUIRE(c.handle(other, 140)["error"]["code"] == "control_busy");
        REQUIRE(c.handle(make("Start", "5"), 150)["ok"] == true);
        REQUIRE(c.handle(make("Pause", "6"), 160)["ok"] == true);
        REQUIRE(c.handle(make("CapturePair", "7"), 170)["error"]["code"] == "unsupported");
        c.frame_boundary(43, 12000);
        REQUIRE(c.status()["state"] == "cancelled");
        REQUIRE(!c.status()["p0_gate_open"].get<bool>());
        lab::Controller observer;
        REQUIRE(observer.status()["origin"] == "unattached");
        REQUIRE(observer.status()["requested"]["nr_mode"] == "unknown");
        REQUIRE(observer.status()["observer"].is_null());
        observer.publish_observer({{"origin", "d3d12-harness"}, {"written", 7}, {"dropped", 1}});
        REQUIRE(observer.status()["observer"]["written"] == 7);
        REQUIRE(observer.status()["capabilities"]["queue_observation"] == true);
        REQUIRE(observer.status()["origin"] == "d3d12-harness");
        REQUIRE(observer.status()["p0_gate_open"] == false);
        // Oversize snapshots are clipped to the bound, never thrown into the publishing worker.
        observer.publish_observer({{"origin", "d3d12-harness"}, {"written", 7}, {"dropped", 1}, {"large", std::string(50 * 1024, 'x')}});
        REQUIRE(observer.status()["observer"]["written"] == 7);
        REQUIRE(observer.status()["observer"]["clipped"] == lab::json::array({"large"}));
        REQUIRE(observer.status().dump().size() < 64 * 1024);
        auto write = req; write["session_id"] = observer.status()["session_id"];
        REQUIRE(observer.handle(write, 1)["error"]["code"] == "unsupported");
        REQUIRE(observer.handle(lab::json::array(), 1)["ok"] == false);
        lab::PipeServer server(observer);
        // A real client may start before the server has created any instance.
        std::jthread delayed_start([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            server.start();
        });
        auto read = make("GetStatus", "ipc");
        auto response = lab::pipe_request(GetCurrentProcessId(), read);
        delayed_start.join();
        REQUIRE(server.ready());
        REQUIRE(response["ok"] == true);
        REQUIRE(response["status"]["capabilities"]["nr_control"] == false);
        REQUIRE(response["status"]["observer"]["written"] == 7);
        REQUIRE(response["status"]["observer"]["dropped"] == 1);
        const auto sessions=lab::discover_lab_processes();
        REQUIRE(std::find(sessions.begin(),sessions.end(),GetCurrentProcessId())!=sessions.end());
        server.stop();
        std::cout << "PASS: revisions, idempotency, ownership, safe boundary, timeout, unsupported NR and real local IPC\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
