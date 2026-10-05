// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include "lab_nr_settings.hpp"
namespace lab {
void Controller::enable_nr_preparation(bool integrated){
    std::lock_guard lock(mutex_);
    if(nr_terminal_||synthetic_||nr_preparation_.is_object()||!nr_origin_.empty())throw std::logic_error("NR preparation already configured or unsupported");
    nr_preparation_={{"state","waiting_for_user"},{"scene_confirmation_source","manual-not-runtime-verified"},{"automatically_enables_nr",false}};
    nr_integrated_=integrated;
    if(integrated){nr_preparation_["state"]="standby";nr_preparation_["scene_confirmation_source"]="not-required-for-product-control";
        nr_status_={{"state","standby"},{"requested_mode","off"},{"observed_mode","off"},
            {"observation_scope","NR runtime not started; not displayed-frame evidence"},{"requested_revision",0u},{"applied_revision",nullptr},{"applied_frame",nullptr}};}
}
void Controller::set_default_exposure_stops(float stops){
    std::lock_guard lock(mutex_);
    nr::Settings probe;probe.exposure_stops=stops;
    if(!probe.valid()||settings_revision_||nr_desired_on_)throw std::logic_error("Default exposure must be valid and set before any settings request");
    desired_settings_["exposure_stops"]=stops;
}
void Controller::enable_nr_compute_only(){
    std::lock_guard lock(mutex_);nr_compute_only_=true;
}
bool Controller::request_late_attach_preparation(){
    std::lock_guard lock(mutex_);
    if(nr_terminal_||!nr_integrated_||!nr_origin_.empty()||nr_desired_on_)return false;
    if(nr_preparation_.value("state","")!="standby")return false;
    ++revision_;
    nr_preparation_["state"]="requested";
    nr_preparation_["revision"]=revision_;
    nr_preparation_["request_id"]="late-attach";
    // Say what this actually is. It is NOT the manual scene confirmation that
    // PrepareNr demands from a person; it is the injector's own action, and the
    // runtime still refuses every frame it cannot verify.
    nr_preparation_["scene_confirmation_source"]="late-attach: injected into a game that was already presenting, at the operator's request";
    nr_intent_dirty_=true;
    return true;
}
bool Controller::take_nr_preparation(std::uint64_t now){
    std::lock_guard lock(mutex_);expire(now);
    if(nr_terminal_||!nr_preparation_.is_object()||nr_preparation_.value("state","")!="requested")return false;
    nr_preparation_["state"]="preparing";return true;
}
void Controller::set_nr_intent_locked(unsigned on,const std::string& id){
    nr_desired_on_=on;nr_intent_dirty_=true;
    nr_status_["requested_mode"]=nr::mode_name(on);nr_status_["requested_revision"]=revision_+1;
    nr_pending_.reset(); // Only unsent requests are superseded; in-flight work is retained.
    auto phase=nr_preparation_.value("state","");
    if(nr_origin_.empty()){
        if(on&&phase=="standby"){nr_preparation_["state"]="requested";nr_preparation_["revision"]=revision_+1;nr_preparation_["request_id"]=id;}
        if(!on&&phase=="requested")nr_preparation_["state"]="standby";
        nr_status_["state"]=on?"waiting-input":"standby";
        if(!on&&phase!="preparing"){nr_intent_dirty_=false;nr_status_["observed_mode"]="off";}
    }else if(nr_runtime_.is_object()&&nr_runtime_.value("state","")=="ready"){
        nr_pending_=json{{"mode",nr::mode_name(on)},{"revision",revision_+1},{"request_id",id},{"reason","user-intent"}};
        if(on&&embedded_enabled_)(*nr_pending_)["settings"]=desired_settings_;
        nr_intent_dirty_=false;nr_status_["state"]="requested";
    }
}
void Controller::queue_nr_intent_locked(const char* reason){
    if(!nr_integrated_||nr_terminal_||!nr_intent_dirty_||nr_origin_.empty()||
       !nr_runtime_.is_object()||nr_runtime_.value("state","")!="ready")return;
    ++revision_;
    nr_pending_=json{{"mode",nr::mode_name(nr_desired_on_)},{"revision",revision_},{"reason",reason}};
    if(nr_desired_on_&&embedded_enabled_)(*nr_pending_)["settings"]=desired_settings_;
    nr_intent_dirty_=false;nr_status_["requested_mode"]=nr::mode_name(nr_desired_on_);
    nr_status_["requested_revision"]=revision_;nr_status_["state"]="requested";
}
}
