// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_nr_panel.hpp"
#include <iostream>
namespace {void need(bool v,const char* why){if(!v)throw std::runtime_error(why);}}
int main(){try{
    lab::json s={{"control_owner",""},{"capabilities",{{"nr_preparation",true},{"nr_frame_control",false}}},
        {"host",{{"state","awaiting-user-preparation"}}},{"nr_preparation",{{"state","waiting_for_user"}}}};
    const auto panel=[&](bool scene=false,bool busy=false,bool connected=true){return lab::nr_panel(s,connected,busy,"console-test",scene);};
    need(!panel(false,false,false).present,"Disconnected status must not show live controls");
    need(panel().confirmable&&!panel().can_prepare&&!panel().can_toggle,"Unconfirmed startup cannot prepare or toggle");
    need(panel(true).can_prepare&&!panel(true).can_toggle,"Manual scene confirmation only enables preparation");
    need(!panel(true,true).can_prepare,"Pending transport prevents repeat preparation");
    s["control_owner"]="other";need(panel(true).other_owner&&!panel(true).can_prepare,"Other writer cannot be overridden");s["control_owner"]="";
    for(const auto* state:{"requested","preparing"}){s["nr_preparation"]["state"]=state;
        need(panel(true).preparing&&!panel(true).can_prepare&&!panel(true).can_toggle,"Preparing has no ON or repeated Prepare");}
    s["nr_preparation"]["state"]="ready";s["capabilities"]["nr_frame_control"]=true;
    s["nr_frame_control"]={{"state","idle"},{"observed_mode","unknown"},{"requested_mode","off"}};
    need(panel().can_toggle&&panel().observed=="unknown","Ready does not invent an observed OFF frame");
    s["nr_frame_control"]={{"state","applying"},{"observed_mode","off"},{"requested_mode","on"}};
    need(!panel().can_toggle&&panel().observed=="off"&&panel().requested=="on","Pending ON is not displayed as applied ON");
    s["nr_frame_control"]["state"]="applied";s["nr_frame_control"]["observed_mode"]="on";
    need(panel().can_toggle&&panel().observed=="on","Actual acknowledgement enables current controls");
    s["control_owner"]="other";need(!panel().can_toggle,"Single control writer");s["control_owner"]="";
    s["nr_runtime"]={{"state","draining"},{"rebuild_serial",1u}};
    need(panel().rebuilding&&!panel().can_toggle&&!panel().ready&&!panel().failed,"Rebuild blocks stale capability without terminal failure");
    s.erase("nr_runtime");
    for(const auto* field:{"nr_preparation","nr_frame_control","nr_runtime","host"}){
        const auto saved=s.contains(field)?s[field]:lab::json();
        for(const auto* terminal:{"failed","stopped"}){s[field]={{"state",terminal},{"error","test fault"}};
            need(panel(true).present&&panel(true).failed&&!panel(true).can_toggle&&!panel(true).can_prepare&&panel(true).error=="test fault","Terminal state remains visible and overrides stale capabilities");}
        if(saved.is_null())s.erase(field);else s[field]=saved;
    }
    s={{"control_owner",""},{"capabilities",{{"nr_intent_control",true}}},
       {"nr_lifecycle",{{"automatic",true},{"desired_mode","off"}}},
       {"nr_preparation",{{"state","standby"}}}};
    need(panel().automatic&&panel().can_toggle&&!panel().can_prepare&&!panel().confirmable,"Automatic standby exposes one switch without a scene/prepare gate");
    s["nr_preparation"]["state"]="preparing";s["nr_lifecycle"]["desired_mode"]="on";
    need(panel().can_toggle&&panel().requested=="on","OFF stays available while preparation is pending");
    s["nr_runtime"]={{"state","draining"},{"rebuild_serial",1u}};
    need(panel().can_toggle&&panel().rebuilding,"OFF stays available during GPU drain");
    s["control_owner"]="other";need(!panel().can_toggle,"Automatic does not steal another writer");s["control_owner"]="";
    need(!panel(false,true).can_toggle,"An outstanding transport request still prevents a duplicate click");
    s["nr_runtime"]["state"]="failed";need(!panel().can_toggle&&panel().failed,"Automatic never hides terminal errors");
    std::cout<<"PASS NR panel legacy/automatic intent, preparation, writer, requested/observed split and visible failures\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
