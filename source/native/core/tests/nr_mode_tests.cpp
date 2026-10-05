// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include <iostream>
namespace {
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
lab::json request(lab::Controller& c,const char* mode,const char* client="nr-test"){
    const auto s=c.status();return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id",client},
        {"session_id",s["session_id"]},{"expected_revision",s["revision"]},{"method","SetNrMode"},{"params",{{"mode",mode}}}};
}
template<class F>void refused(F call){bool rejected=false;try{call();}catch(const std::logic_error&){rejected=true;}require(rejected,"Expected internal acknowledgement rejection");}
}
int main(){try{
    lab::Controller bare;
    require(bare.handle(request(bare,"on"),1)["error"]["code"]=="unsupported","Idle game backend accepted NR");
    lab::Controller fake(true);refused([&]{fake.enable_nr_frame_control("synthetic-input-real-nr");});
    refused([&]{bare.enable_nr_frame_control("game");});
    lab::Controller c;c.enable_nr_frame_control("synthetic-input-real-nr");
    require(c.status()["capabilities"]["nr_control"]==false,"Game capability falsely enabled");
    require(c.handle(request(c,"compute_bypass"),1)["error"]["code"]=="bad_config","Unknown mode accepted");
    auto on=request(c,"on");const auto accepted=c.handle(on,2);
    require(accepted["ok"]==true && c.handle(on,3)==accepted,"NR request not idempotent");
    c.frame_boundary(99,4);
    require(c.status()["nr_frame_control"]["observed_mode"]=="unknown","Timer fabricated applied mode");
    require(c.handle(request(c,"off","other"),5)["error"]["code"]=="control_busy","Wrong owner accepted");
    require(c.handle(request(c,"off"),6)["error"]["code"]=="pending","Unacknowledged request overwritten");
    const auto apply=c.take_nr_mode_request(7);require(apply.has_value(),"No NR request");
    require(!c.take_nr_mode_request(8),"NR work consumed twice");
    const auto rev=apply->at("revision").get<std::uint64_t>();
    refused([&]{c.acknowledge_nr_mode(rev+1,10,true,true,true);});
    refused([&]{c.acknowledge_nr_mode(rev,10,true,true,false);});
    c.frame_boundary(100,11000); // Disconnect while ON is in flight.
    require(c.status()["nr_frame_control"]["requested_mode"]=="off","Lease did not request OFF");
    c.acknowledge_nr_mode(rev,10,true,true,true);
    require(c.status()["nr_frame_control"]["observed_mode"]=="on","Actual late ON was hidden");
    require(c.status()["nr_frame_control"]["state"]=="requested","Queued OFF lost");
    const auto off=c.take_nr_mode_request(11001);require(off && off->at("mode")=="off","Disconnected OFF not consumed");
    const auto off_rev=off->at("revision").get<std::uint64_t>();
    refused([&]{c.acknowledge_nr_mode(off_rev,11,true,true,false);});
    c.acknowledge_nr_mode(off_rev,11,true,false,false);
    require(c.status()["nr_frame_control"]["observed_mode"]=="off","OFF acknowledgement missing");
    require(c.status()["nr_frame_control"]["applied_frame"]==11,"Wrong effective frame");
    auto stale=request(c,"on");stale["expected_revision"]=0u;
    require(c.handle(stale,11002)["error"]["code"]=="stale_revision","Stale revision accepted");
    require(c.handle(request(c,"on"),11003)["ok"]==true,"Re-enable refused");
    const auto failed=c.take_nr_mode_request(11004);
    c.acknowledge_nr_mode(failed->at("revision").get<std::uint64_t>(),12,false,true,false,"Evaluate failed");
    require(c.status()["nr_frame_control"]["observed_mode"]=="unknown","Failed output reported usable");
    require(c.handle(request(c,"off"),11005)["error"]["code"]=="backend_failed","Failed backend accepted new scheduling");
    require(!c.status()["p0_gate_open"].get<bool>(),"P0 falsely raised");
    lab::Controller game;game.publish_nr_runtime({{"profile","cyberpunk-rr-experimental-v1"},{"state","ready"}});
    require(game.status()["capabilities"]["nr_control"]==false,"Status JSON alone cannot grant rendering capability");
    game.enable_nr_frame_control("game-rr-experimental-nr");
    require(game.status()["capabilities"]["nr_control"]==true&&game.status()["nr_frame_control"]["observed_mode"]=="unknown","Explicit live backend starts with unknown observed mode");
    auto pending=game.handle(request(game,"on"),1);require(pending["ok"]==true,"Game NR request accepted");
    game.publish_nr_runtime({{"profile","cyberpunk-rr-experimental-v1"},{"state","failed"},{"error","resource mismatch"}});
    require(game.status()["capabilities"]["nr_control"]==false&&game.status()["nr_frame_control"]["observed_mode"]=="unknown","Runtime failure revokes control without false OFF claim");
    require(game.handle(request(game,"on"),2)["error"]["code"]=="backend_failed","Cannot restart terminal game context");
    require(game.status()["p0_gate_open"]==false,"Real controls do not grant P0 evidence");
    std::cout<<"PASS: real-backend mode mailbox, idempotency, applied frames, contradictory acknowledgements, in-flight lease cancellation and fail-closed state\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
