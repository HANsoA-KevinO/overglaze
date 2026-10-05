// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include <iostream>

namespace {
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Fixture {
    lab::Controller control;
    bool display;
    explicit Fixture(bool d):display(d){
        control.enable_nr_preparation(true);control.enable_embedded_control();
        if(display)control.enable_display_capture();
        control.enable_nr_frame_control("game-rr-experimental-nr",true,true);
        control.publish_nr_runtime({{"profile","cyberpunk2077-rr-v1"},{"state","ready"}});
        auto off=control.take_nr_mode_request(1);need(off.has_value(),"Initial OFF intent");
        control.acknowledge_nr_mode(off->at("revision"),1,true,false,false);
        if(!display){need(control.handle_embedded("SetNrMode",{{"mode","on"}},2)["ok"]==true,"ON intent");
            auto on=control.take_nr_mode_request(3);need(on.has_value(),"ON delivery");
            control.publish_nr_runtime({{"profile","cyberpunk2077-rr-v1"},{"state","ready"},
                {"settings",{{"observed_revision",on->at("revision")},{"frame",2u},{"read_mask",3u},{"style_read",true},{"observed",on->at("settings")}}}});
            control.acknowledge_nr_mode(on->at("revision"),2,true,true,true);}
    }
    const char* method()const{return display?"CaptureDisplayPair":"CapturePair";}
    const char* cancel_method()const{return display?"CancelDisplayPair":"CancelCapturePair";}
    lab::json params()const{return display?lab::json{{"scene_confirmed",true},{"fg_disabled_confirmed",true}}:lab::json::object();}
    lab::json begin(std::uint64_t now=100){return control.handle_embedded(method(),params(),now);}
    void cancel(std::uint64_t now=110){need(control.handle_embedded(cancel_method(),lab::json::object(),now)["ok"]==true,"Cancel accepted");}
    lab::json status()const{return control.status()[display?"display_pair":"frame_pair"];}
    void publish(const char* phase,std::uint64_t revision){
        lab::json s={{"state",phase},{"revision",revision},{"files",0u}};
        if(display)control.publish_display_pair(s);else control.publish_pair(s);
    }
    bool dispatch(std::uint64_t now,const std::function<bool(const lab::json&)>& callback){
        return display?control.dispatch_display_capture_request(now,callback):control.dispatch_pair_request(now,callback);
    }
};
unsigned cases=0;
void run(bool display){
    {
        Fixture f(display);unsigned calls=0;
        for(unsigned i=0;i<50;++i)f.dispatch(100+i,[&](const auto&){++calls;return true;});
        need(!calls&&f.status()["state"]=="idle","Idle/startup must not invoke capture");++cases;
    }
    {
        Fixture f(display);unsigned starts=0;need(f.begin()["ok"]==true,"Explicit trigger");
        const auto revision=f.status()["revision"];
        for(unsigned i=0;i<20;++i){
            need(f.begin(101+i)["error"]["code"]=="pending","Repeated click must not queue extra groups");
            need(!f.dispatch(101+i,[&](const auto& a){need(a["revision"]==revision,"One revision throughout contention");return false;}),"Busy is not acceptance");
        }
        need(f.status()["state"]=="requested","Unaccepted request remains cancellable in Controller");
        need(f.dispatch(130,[&](const auto& a){need(a["cancel"]==false,"One start");++starts;return true;}),"Backend handles once");
        for(unsigned i=0;i<50;++i)f.dispatch(131+i,[&](const auto&){++starts;return true;});
        need(starts==1,"No automatic repeat after handled response");++cases;
        f.publish("complete",revision);
        f.publish("pending_gpu",revision);
        need(f.status()["state"]=="complete","Late active receipt cannot resurrect completed request");++cases;
    }
    {
        Fixture f(display);need(f.begin()["ok"]==true,"Begin before cancel");
        f.dispatch(101,[](const auto&){return false;});f.cancel();unsigned calls=0;
        f.dispatch(111,[&](const auto&){++calls;return true;});
        need(!calls&&f.status()["state"]=="cancelled","Cancel during contention prevents delayed start");++cases;
    }
    {
        Fixture f(display);need(f.begin()["ok"]==true,"Begin before timeout");
        need(!f.dispatch(10099,[](const auto&){return false;}),"Busy up to deadline");unsigned calls=0;
        need(!f.dispatch(10100,[&](const auto&){++calls;return true;}),"Exact deadline rejects submission");
        f.dispatch(20000,[&](const auto&){++calls;return true;});
        need(!calls&&f.status()["failure_code"]=="dispatch_timeout"&&f.status()["files"]==0,"Expired request never starts later");++cases;
        const auto old=f.status()["revision"];f.publish("arming",old);
        need(f.status()["state"]=="failed","Timeout cannot be erased by late active receipt");
        need(f.begin(20001)["ok"]==true,"New explicit request allowed after zero-work timeout");
        need(f.status()["revision"]!=old,"New request has new identity");
        f.dispatch(20002,[&](const auto&){++calls;return true;});need(calls==1,"Only explicit rearm starts work");++cases;
    }
    {
        Fixture f(display);f.begin();const auto rev=f.status()["revision"];
        f.dispatch(101,[](const auto&){return true;});f.publish("failed",rev);
        unsigned calls=0;f.dispatch(102,[&](const auto&){++calls;return true;});
        need(!calls&&f.status()["state"]=="failed","Handled rejection/failure is never automatically retried");++cases;
    }
    {
        Fixture f(display);f.begin();const auto rev=f.status()["revision"];
        f.dispatch(101,[](const auto&){return true;});f.publish("pending_gpu",rev);f.cancel();
        f.control.publish_nr_runtime({{"profile","cyberpunk2077-rr-v1"},{"state","draining"}});
        unsigned starts=0,cancels=0;
        need(!f.dispatch(20000,[&](const auto& a){if(a.value("cancel",false))++cancels;else ++starts;return false;}),"Busy cancel remains pending");
        need(f.status()["state"]!="cancelled","No GPU drain invented before worker receipt");
        need(f.dispatch(20001,[&](const auto& a){if(a.value("cancel",false))++cancels;else ++starts;return true;}),"Cancellation delivered even while not ready");
        need(starts==0&&cancels==2&&f.status()["state"]=="cancelling","Retry only unhandled cancel, never old start");
        f.publish("cancelled",rev);need(f.status()["state"]=="cancelled","Worker retirement is separate");++cases;
    }
    {
        Fixture f(display);f.begin();f.dispatch(101,[](const auto&){return false;});
        f.control.interrupt_nr_for_rebuild(1,20);unsigned calls=0;
        f.dispatch(102,[&](const auto&){++calls;return true;});
        need(!calls,"Rebuild removes even a contended delivery");++cases;
    }
    {
        Fixture f(display);
        const auto request=[&](const char* method,lab::json params,const char* id){const auto s=f.control.status();
            return lab::json{{"protocol","1.0"},{"request_id",id},{"client_id","capture-delivery-test"},
                {"session_id",s["session_id"]},{"expected_revision",s["revision"]},{"method",method},{"params",params}};};
        need(f.control.handle(request("TakeControl",lab::json::object(),"take"),100)["ok"]==true,"Explicit external ownership");
        const auto start=request(f.method(),f.params(),"start");const auto first=f.control.handle(start,101);
        need(first["ok"]==true&&f.control.handle(start,102)==first,"Retry same request returns cached receipt");
        f.dispatch(103,[](const auto&){return false;});unsigned calls=0;
        f.dispatch(10103,[&](const auto&){++calls;return true;});
        need(!calls&&f.status()["state"]=="cancelled","External lease expires before any delayed dispatch");++cases;
    }
    {
        Fixture f(display);f.begin();unsigned calls=0;
        // A throwing delivery fails NR closed inside Controller and is not rethrown into the host worker.
        need(!f.dispatch(101,[&](const auto&)->bool{++calls;throw std::runtime_error("test delivery exception");}),"Throwing delivery is not acceptance");
        f.dispatch(102,[&](const auto&){++calls;return true;});
        need(calls==1&&f.control.status()["nr_frame_control"]["state"]=="failed"&&f.status()["state"]=="failed"&&f.status()["failure_code"]=="delivery_exception"&&f.status()["files"]==0,
             "Unknown acceptance fails closed without retry or rethrow");++cases;
    }
    {
        Fixture f(display);f.begin(200);unsigned calls=0;
        f.dispatch(199,[&](const auto&){++calls;return true;});
        need(!calls&&f.status()["state"]=="failed","Regressed clock cannot prolong or admit stale request");++cases;
    }
}
}
int main(){try{
    run(false);run(true);
    lab::Controller waiting;waiting.enable_nr_preparation(true);waiting.enable_embedded_control();waiting.enable_display_capture();
    need(waiting.handle_embedded("CaptureDisplayPair",{{"scene_confirmed",true},{"fg_disabled_confirmed",true}},100)["ok"]==true,"Wait for lazy preparation");
    unsigned calls=0;waiting.dispatch_display_capture_request(10100,[&](const auto&){++calls;return true;});
    need(!calls&&!waiting.take_nr_preparation(10101)&&waiting.status()["display_pair"]["state"]=="failed","Preparation wait expires too; no late capture/prepare");++cases;
    std::cout<<"PASS "<<cases<<" capture delivery scenarios; mock backend, zero GPU/NR/file output\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
