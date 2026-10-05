// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
namespace lab {
// Presentation only: never grants backend capability or infers a scene from RR.
struct NrPanel {
    bool present=false,failed=false,preparing=false,ready=false,confirmable=false,rebuilding=false;
    bool can_prepare=false,can_toggle=false,other_owner=false;
    bool automatic=false;
    std::string phase,observed,requested,error;
};
inline NrPanel nr_panel(const json& status,bool connected,bool pending,const std::string& client,bool scene_confirmed){
    NrPanel v;if(!connected)return v;
    const auto prep=status.value("nr_preparation",json());
    const auto nr=status.value("nr_frame_control",json());
    const auto runtime=status.value("nr_runtime",json());
    const auto host=status.value("host",json());
    const auto caps=status.value("capabilities",json::object());
    v.present=prep.is_object()||nr.is_object();if(!v.present)return v;
    const auto terminal=[](const json& x){return x.is_object()&&(x.value("state","")=="failed"||x.value("state","")=="stopped");};
    v.failed=terminal(prep)||terminal(nr)||terminal(runtime)||terminal(host);
    for(const auto* x:{&prep,&nr,&runtime,&host})if(v.error.empty()&&terminal(*x))v.error=x->value("error",std::string());
    const auto owner=status.value("control_owner",std::string());v.other_owner=!owner.empty()&&owner!=client;
    const auto p=prep.is_object()?prep.value("state",std::string()):"";
    v.phase=nr.is_object()?nr.value("state",std::string()):"";
    v.observed=nr.is_object()?nr.value("observed_mode",std::string("unknown")):"unknown";
    v.requested=nr.is_object()?nr.value("requested_mode",std::string("unknown")):"unknown";
    v.rebuilding=!v.failed&&((runtime.is_object()&&runtime.value("rebuild_serial",0ULL)>0&&runtime.value("state","")!="ready")||v.phase=="rebuilding");
    v.preparing=!v.failed&&(p=="requested"||p=="preparing");
    v.ready=!v.failed&&!v.rebuilding&&nr.is_object()&&caps.value("nr_frame_control",false);
    v.confirmable=!v.failed&&!v.ready&&p=="waiting_for_user"&&caps.value("nr_preparation",false);
    const bool writer=!pending&&!v.other_owner;
    v.can_prepare=v.confirmable&&writer&&scene_confirmed;
    v.can_toggle=v.ready&&writer&&(v.phase=="idle"||v.phase=="applied");
    const auto lifecycle=status.value("nr_lifecycle",json());v.automatic=lifecycle.is_object()&&lifecycle.value("automatic",false);
    if(v.automatic){v.requested=lifecycle.value("desired_mode",std::string("off"));v.confirmable=v.can_prepare=false;
        v.can_toggle=!v.failed&&writer&&caps.value("nr_intent_control",false);}
    return v;
}
}
