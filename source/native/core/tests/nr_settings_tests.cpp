// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include "lab_nr_settings.hpp"
#include <iostream>
#include <limits>
namespace {
void need(bool v,const char* m){if(!v)throw std::runtime_error(m);}
lab::json req(lab::Controller& c,const char* method,lab::json params,const char* client="settings-test"){
    const auto s=c.status();return {{"protocol","1.0"},{"request_id",lab::uuid()},{"client_id",client},
        {"session_id",s["session_id"]},{"expected_revision",s["revision"]},{"method",method},{"params",params}};
}
}
int main(){try{
    need(!lab::nr::Settings{std::numeric_limits<float>::quiet_NaN(),1}.valid(),"NaN");
    lab::Controller c;c.enable_nr_frame_control("synthetic-input-real-nr",true);
    const lab::json values={{"tone",.5f},{"structure",0.f},{"style",2u},{"exposure_stops",0.f},{"exposure_auto",0u},{"compare_split",0u},{"skin",1.f},{"automask",0u},
        {"extrapolate",0u},{"extrapolate_factor",2.f}};
    // ABI22: Tone/Structure 0..2, Skin 0..2, AutoMask 0/1.
    need(lab::nr::Settings{2,2}.valid()&&!lab::nr::Settings{2.01f,1}.valid()&&!lab::nr::Settings{1,2.01f}.valid(),"Tone/Structure range 0..2");
    {lab::nr::Settings s;need(s.skin==1.f&&s.automask==0&&s.valid(),"Default Skin 1, AutoMask off");s.skin=2.f;s.automask=1;need(s.valid(),"Skin 2 with mask");
     s.skin=2.1f;need(!s.valid(),"Skin above 2");s.skin=-.1f;need(!s.valid(),"Negative Skin (the SDK -1 default is not a panel value)");s.skin=1;s.automask=2;need(!s.valid(),"AutoMask 0/1");}
    {lab::nr::Settings want{1,1,0};want.skin=.25f;want.automask=1;lab::nr::SettingsReads r{want,3,true};
     need(r.matches(want),"Unread optional controls never fail the receipt");
     r.skin_observed=r.automask_observed=true;need(r.matches(want),"Read optional controls match");
     r.values.skin=1.f;need(!r.matches(want),"A different Skin read by the DLL fails");r.values.skin=.25f;r.values.automask=0;need(!r.matches(want),"A different AutoMask read fails");}
    need(!lab::nr::Settings{1,1,0,-12.5f}.valid()&&!lab::nr::Settings{1,1,0,10.5f}.valid()&&lab::nr::Settings{1,1,0,5.f}.valid()&&lab::nr::Settings{1,1,0,-9.f}.valid(),"Exposure stops range (-12..10; -9 reaches scene-referred daylight)");
    need(lab::nr::Settings{1,1,0,3.f}.exposure_gain()==8.f,"Exposure gain is 2^stops");
    // ABI25 edit extrapolation: off by default at factor 2; the factor range 1..4
    // holds whether or not it is on; off composites with 1.
    {lab::nr::Settings s;need(!s.extrapolate&&s.extrapolate_factor==2.f&&s.valid()&&s.applied_extrapolation()==1.f,"Extrapolation off by default, factor 2, plain composite");
     s.extrapolate=1;need(s.valid()&&s.applied_extrapolation()==2.f,"On: the factor is applied");
     for(const float f:{1.f,4.f}){s.extrapolate_factor=f;need(s.valid()&&s.applied_extrapolation()==f,"Factor 1 and 4 are in range");}
     for(const float f:{.99f,4.01f,0.f,-1.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
         s.extrapolate_factor=f;need(!s.valid(),"Factor outside finite 1..4");s.extrapolate=0;need(!s.valid(),"Range holds while off too");s.extrapolate=1;}
     s.extrapolate_factor=2;s.extrapolate=2;need(!s.valid(),"Extrapolate 0/1");
     lab::nr::Settings want{1,1,0};want.extrapolate=1;want.extrapolate_factor=3;
     need(lab::nr::SettingsReads{lab::nr::Settings{1,1,0},3,true}.matches(want),"DLL reads never include extrapolation");
     need(want.model_equal(lab::nr::Settings{1,1,0}),"Extrapolation is not a model parameter");}
    need(lab::nr::SettingsReads{lab::nr::Settings{.5f,0.f,2,0.f},3,true}.matches(lab::nr::Settings{.5f,0.f,2,4.f}),"DLL reads never include host exposure");
    need(c.handle(req(c,"SetNrSettings",values),1)["error"]["code"]=="nr_off","OFF parameter request");
    c.handle(req(c,"SetNrMode",{{"mode","on"}}),2);auto on=c.take_nr_mode_request(3);c.acknowledge_nr_mode(on->at("revision"),1,true,true,true);
    for(const auto& bad:{lab::json{{"tone",2.1},{"structure",0}},lab::json{{"tone",2.00000000001},{"structure",0}},lab::json{{"tone",0},{"structure",2.5}},lab::json{{"tone",0},{"structure",1},{"skin",2.5}},lab::json{{"tone",0},{"structure",1},{"skin",-1}},lab::json{{"tone",0},{"structure",1},{"automask",2}},lab::json{{"tone",0},{"structure",1},{"skin","1"}},lab::json{{"tone",0},{"structure",-1e-50}},lab::json{{"tone",0}},lab::json{{"tone",true},{"structure",1}},lab::json{{"tone",0},{"structure",1},{"model","A"}},lab::json{{"tone",0},{"structure",1},{"exposure_stops",11}},lab::json{{"tone",0},{"structure",1},{"exposure_stops","5"}},
        lab::json{{"tone",0},{"structure",1},{"extrapolate",2}},lab::json{{"tone",0},{"structure",1},{"extrapolate","on"}},lab::json{{"tone",0},{"structure",1},{"extrapolate",.5}},
        lab::json{{"tone",0},{"structure",1},{"extrapolate_factor",.99}},lab::json{{"tone",0},{"structure",1},{"extrapolate_factor",4.0000001}},
        lab::json{{"tone",0},{"structure",1},{"extrapolate_factor",0}},lab::json{{"tone",0},{"structure",1},{"extrapolate_factor","2"}},
        lab::json{{"tone",0},{"structure",1},{"extrapolate_factor",true}},lab::json{{"tone",0},{"structure",1},{"extrapolate_factor",std::numeric_limits<double>::quiet_NaN()}},
        lab::json{{"tone",0},{"structure",1},{"extrapolate_factor",std::numeric_limits<double>::infinity()}}})
        need(c.handle(req(c,"SetNrSettings",bad),4)["error"]["code"]=="bad_config","Bad parameter request");
    auto r=req(c,"SetNrSettings",values);const auto accepted=c.handle(r,5);need(accepted["ok"]==true&&c.handle(r,6)==accepted,"Idempotency");
    need(c.handle(req(c,"SetNrMode",{{"mode","off"}}),7)["error"]["code"]=="pending","No concurrent mutation");
    need(c.handle(req(c,"SetNrSettings",values,"other"),7)["error"]["code"]=="control_busy","Writer ownership");
    const auto work=c.take_nr_mode_request(8);need(work&&work->at("settings")==values&&!c.take_nr_mode_request(9),"Single consumption");
    bool refused=false;try{c.acknowledge_nr_mode(work->at("revision"),2,true,true,true);}catch(const lab::json::exception&){refused=true;}catch(const std::logic_error&){refused=true;}
    need(refused,"No receipt fabricated from request");
    c.publish_nr_runtime({{"profile","cyberpunk-rr-experimental-v1"},{"state","ready"},{"settings",{{"observed_revision",work->at("revision")},{"frame",2u},{"read_mask",3u},{"observed",values}}}});
    refused=false;try{c.acknowledge_nr_mode(work->at("revision"),2,true,true,true);}catch(const std::logic_error&){refused=true;}
    need(refused,"Tone/Structure reads do not imply Style read");
    c.publish_nr_runtime({{"profile","cyberpunk-rr-experimental-v1"},{"state","ready"},{"settings",{
        {"observed_revision",work->at("revision")},{"frame",2u},{"read_mask",3u},{"style_read",true},{"observed",values}}}});
    c.acknowledge_nr_mode(work->at("revision"),2,true,true,true);need(c.status()["nr_frame_control"]["applied_frame"]==2,"Exact receipt frame");
    // Edit extrapolation round trip: request -> mailbox -> exact observed receipt;
    // an omitted field keeps the current value.
    {auto ext=c.handle(req(c,"SetNrSettings",{{"tone",.5},{"structure",0},{"extrapolate",true},{"extrapolate_factor",3.5}}),9);
     need(ext["ok"]==true,"Extrapolation accepted");
     auto sent=c.take_nr_mode_request(9);
     need(sent&&sent->at("settings")["extrapolate"]==1&&sent->at("settings")["extrapolate_factor"]==3.5&&sent->at("settings")["style"]==2,"Extrapolation reaches the runtime mailbox; Style kept");
     auto observed=sent->at("settings");
     c.publish_nr_runtime({{"profile","cyberpunk-rr-experimental-v1"},{"state","ready"},{"settings",{
         {"observed_revision",sent->at("revision")},{"frame",3u},{"read_mask",3u},{"style_read",true},{"observed",observed}}}});
     c.acknowledge_nr_mode(sent->at("revision"),3,true,true,true);need(c.status()["nr_frame_control"]["applied_frame"]==3,"Extrapolation receipt");
     need(c.handle(req(c,"SetNrSettings",{{"tone",.5},{"structure",0},{"extrapolate",0}}),9)["ok"]==true,"Extrapolation switched off");
     sent=c.take_nr_mode_request(9);
     need(sent&&sent->at("settings")["extrapolate"]==0&&sent->at("settings")["extrapolate_factor"]==3.5,"Off keeps the factor");
     observed=sent->at("settings");observed["extrapolate_factor"]=2.f;
     c.publish_nr_runtime({{"profile","cyberpunk-rr-experimental-v1"},{"state","ready"},{"settings",{
         {"observed_revision",sent->at("revision")},{"frame",4u},{"read_mask",3u},{"style_read",true},{"observed",observed}}}});
     refused=false;try{c.acknowledge_nr_mode(sent->at("revision"),4,true,true,true);}catch(const std::logic_error&){refused=true;}
     need(refused,"A runtime that reports another extrapolation factor is no receipt");
     observed["extrapolate_factor"]=3.5f;
     c.publish_nr_runtime({{"profile","cyberpunk-rr-experimental-v1"},{"state","ready"},{"settings",{
         {"observed_revision",sent->at("revision")},{"frame",4u},{"read_mask",3u},{"style_read",true},{"observed",observed}}}});
     c.acknowledge_nr_mode(sent->at("revision"),4,true,true,true);}
    auto unconsumed=c.handle(req(c,"SetNrSettings",{{"tone",1.85},{"structure",2},{"skin",.5},{"automask",true}}),10);need(unconsumed["ok"]==true,"Tone/Structure above 1, Skin and AutoMask accepted");
    const auto expired=c.take_nr_mode_request(11011);need(expired&&expired->at("mode")=="off"&&!expired->contains("settings"),"Lease cancels unconsumed settings and requests OFF");
    c.publish_host({{"state","failed"},{"error","test"}});need(c.status()["capabilities"]["nr_settings"]==false,"Failure revokes capability");
    need(c.handle(req(c,"SetNrSettings",values),11012)["error"]["code"]=="backend_failed","No resurrection");
    std::cout<<"PASS: settings range (Tone/Structure 0..2, Skin, AutoMask, extrapolation 1..4), mailbox, idempotency, exact DLL-read receipt, expiry and failure\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
