// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_control.hpp"
#include "lab_nr_settings.hpp"
#include "lab_game_profile.hpp"

namespace lab {
namespace {
constexpr std::uint64_t capture_dispatch_timeout_ms=10000;
bool pair_active(const json& status){const auto phase=status.is_object()?status.value("state",""):"";return !phase.empty()&&phase!="idle"&&phase!="complete"&&phase!="failed"&&phase!="cancelled";}
bool terminal_pair(const json& status){const auto phase=status.is_object()?status.value("state",""):"";return phase=="complete"||phase=="failed"||phase=="cancelled";}
// A reporting failure (oversize or malformed backend snapshot) must never throw
// into the publishing host worker; it is clipped to a bounded record instead.
// Protected keys carry state/identity/ordering and are never removed; other
// keys are dropped largest-first until the record fits, and their names are
// listed under "clipped". If the protected keys alone exceed the bound the
// record becomes a failure record naming the channel.
bool protected_status_key(const std::string& key){
    for(const char* k:{"state","revision","profile","error","admission_mode","frame","call","rebuild_serial","rebuild_frame","bypass_frame","files","bytes",
                       "failure_code","request_id","settings","clipped","mode","enabled","game_control_available","render_admission","nr_executed","origin","backend"})
        if(key==k)return true;
    return false;
}
json bound_status(json data,std::size_t limit,const char* channel){
    if(!data.is_object())return {{"state","failed"},{"error",std::string(channel)+"-report-shape"},{"clipped",json::array()},{"limit",limit}};
    if(data.dump().size()<=limit)return data;
    json clipped=json::array();
    while(data.dump().size()>limit){
        std::string largest;std::size_t largest_size=0;
        for(const auto& [key,value]:data.items()){if(protected_status_key(key))continue;const auto size=value.dump().size();if(largest.empty()||size>largest_size){largest=key;largest_size=size;}}
        if(largest.empty())break;
        data.erase(largest);clipped.push_back(largest);data["clipped"]=clipped;
    }
    if(data.dump().size()>limit)return {{"state","failed"},{"error",std::string(channel)+"-report-size"},{"clipped",clipped},{"limit",limit}};
    return data;
}
constexpr char bad_settings_message[]="Tone/Structure must be finite 0..2; optional Style must be integer 0, 1 or 2; optional exposure_stops must be finite -12..10; "
    "optional skin must be finite 0..2; optional exposure_auto, compare_split, automask and extrapolate must be a boolean or 0/1; optional extrapolate_factor must be finite 1..4";
bool parse_settings(const json& p,const json& previous,json& result){
    if(!p.is_object()||!p.contains("tone")||!p.contains("structure"))return false;
    const std::size_t expected=2u+(p.contains("style")?1u:0u)+(p.contains("exposure_stops")?1u:0u)+(p.contains("exposure_auto")?1u:0u)+(p.contains("compare_split")?1u:0u)
        +(p.contains("skin")?1u:0u)+(p.contains("automask")?1u:0u)+(p.contains("extrapolate")?1u:0u)+(p.contains("extrapolate_factor")?1u:0u);
    if(p.size()!=expected)return false;
    for(const char* k:{"tone","structure"}){if(!p[k].is_number())return false;const auto v=p[k].get<double>();if(!std::isfinite(v)||v<0||v>nr::Settings::max_tone_structure)return false;}
    unsigned style=previous.value("style",0u);
    if(p.contains("style")){if(!p["style"].is_number_integer()||p["style"]<0||p["style"]>2)return false;style=p["style"].get<unsigned>();}
    // Host exposure (log2 stops) is optional; omitted keeps the current value.
    float exposure_stops=previous.value("exposure_stops",0.f);
    if(p.contains("exposure_stops")){if(!p["exposure_stops"].is_number())return false;const auto v=p["exposure_stops"].get<double>();
        if(!std::isfinite(v)||v<nr::Settings::min_exposure_stops||v>nr::Settings::max_exposure_stops)return false;exposure_stops=static_cast<float>(v);}
    unsigned exposure_auto=previous.value("exposure_auto",0u);
    if(p.contains("exposure_auto")){const auto& a=p["exposure_auto"];
        if(a.is_boolean())exposure_auto=a.get<bool>()?1u:0u;
        else if(a.is_number_integer()&&a>=0&&a<=1)exposure_auto=a.get<unsigned>();
        else return false;}
    unsigned compare_split=previous.value("compare_split",0u);
    if(p.contains("compare_split")){const auto& a=p["compare_split"];
        if(a.is_boolean())compare_split=a.get<bool>()?1u:0u;
        else if(a.is_number_integer()&&a>=0&&a<=1)compare_split=a.get<unsigned>();
        else return false;}
    // ABI22: DLSSNR.SkinStructureStrength and DLSSNR.UseAutoMask, each optional;
    // omitted keeps the current value. The protocol does not couple them; the
    // panel turns the mask on when Skin is moved.
    float skin=previous.value("skin",1.f);
    if(p.contains("skin")){if(!p["skin"].is_number())return false;const auto v=p["skin"].get<double>();
        if(!std::isfinite(v)||v<0||v>nr::Settings::max_skin)return false;skin=static_cast<float>(v);}
    unsigned automask=previous.value("automask",0u);
    if(p.contains("automask")){const auto& a=p["automask"];
        if(a.is_boolean())automask=a.get<bool>()?1u:0u;
        else if(a.is_number_integer()&&a>=0&&a<=1)automask=a.get<unsigned>();
        else return false;}
    // ABI25: edit extrapolation, a composite setting the DLL never reads. Each
    // optional; omitted keeps the current value. The factor is kept while off.
    unsigned extrapolate=previous.value("extrapolate",0u);
    if(p.contains("extrapolate")){const auto& a=p["extrapolate"];
        if(a.is_boolean())extrapolate=a.get<bool>()?1u:0u;
        else if(a.is_number_integer()&&a>=0&&a<=1)extrapolate=a.get<unsigned>();
        else return false;}
    float extrapolate_factor=previous.value("extrapolate_factor",2.f);
    if(p.contains("extrapolate_factor")){if(!p["extrapolate_factor"].is_number())return false;const auto v=p["extrapolate_factor"].get<double>();
        if(!std::isfinite(v)||v<nr::Settings::min_extrapolate_factor||v>nr::Settings::max_extrapolate_factor)return false;extrapolate_factor=static_cast<float>(v);}
    result={{"tone",p["tone"].get<float>()},{"structure",p["structure"].get<float>()},{"style",style},{"exposure_stops",exposure_stops},{"exposure_auto",exposure_auto},{"compare_split",compare_split},
        {"skin",skin},{"automask",automask},{"extrapolate",extrapolate},{"extrapolate_factor",extrapolate_factor}};return true;
}
}
Controller::Controller(bool synthetic) : synthetic_(synthetic), session_(uuid()) {
    if (!synthetic) requested_["nr_mode"] = "unknown";
}

json Controller::status_locked() const {
    const bool rebuilding=nr_runtime_.is_object()&&nr_runtime_.value("rebuild_serial",0ULL)>0&&nr_runtime_.value("state","")!="ready";
    return {{"session_id", session_}, {"pid", GetCurrentProcessId()},
        {"origin", synthetic_ ? "synthetic" : !nr_origin_.empty() ? nr_origin_ : observer_.is_object() ? observer_.value("origin", "unknown") : capture_ ? capture_->snapshot().value("origin","unknown") : "unattached"}, {"state", state_},
        {"revision", revision_}, {"applied_revision", applied_revision_},
        {"applied_frame", applied_frame_}, {"pending", pending_.has_value()},
        {"requested", requested_}, {"observed", observed_}, {"control_owner", owner_},
        {"control",{{"embedded_available",embedded_enabled_},{"explicit_takeover_required",embedded_enabled_},
            {"source",owner_=="@embedded"?"in-game":owner_.empty()?"none":"external"},{"external_lease_ms",10000}}},
        {"nr_settings_request",embedded_enabled_?json{{"values",desired_settings_},{"revision",settings_revision_},
            {"meaning","Requested values only; actual DLL reads remain in nr_runtime.settings"}}:json(nullptr)},
        {"p0_gate_open", false}, {"last_error", last_error_}, {"observer", observer_}, {"host",host_},
        {"nr_preparation",nr_preparation_},
        {"nr_lifecycle",nr_integrated_?json{{"automatic",true},{"desired_mode",nr::mode_name(nr_desired_on_)},
            {"scene_verified",false},{"default_on",false},{"resume_pending",nr_intent_dirty_},
            {"runtime_ready",!nr_terminal_&&nr_runtime_.is_object()&&nr_runtime_.value("state","")=="ready"}}:json(nullptr)},
        {"observer_sampling",observer_sampling_},{"capture",capture_?capture_->snapshot():json(nullptr)},{"nr_frame_control",nr_status_},{"nr_adapter",nr_adapter_},{"nr_runtime",nr_runtime_},
        {"frame_pair",pair_status_},{"display_pair",display_pair_status_},{"post_inspection",post_inspection_},{"nr_access_inspection",access_inspection_},{"binding_restore_probe",binding_probe_},{"input_preparation_probe",preparation_probe_},{"binding_boundaries",binding_boundary_summary_},
        {"capture_policy",{{"trigger","explicit"},{"groups_per_request",1},{"dispatch_timeout_ms",capture_dispatch_timeout_ms},
            {"automatic_recapture",false},{"startup_capture",false},{"applies_to","CapturePair/CaptureDisplayPair; not Nsight"}}},
        {"capabilities", {{"nr_control", !nr_terminal_&&!rebuilding&&nr_origin_=="game-rr-experimental-nr"}, {"capture_pair", !nr_terminal_&&!rebuilding&&nr_pair_enabled_}, {"history_reset", false},
             {"feature_recreate", false}, {"synthetic_control", synthetic_},
             {"queue_observation", observer_.is_object()},
             {"manual_capture",capture_!=nullptr},{"post_binding_inspection",bool(post_inspection_action_)},
             {"nr_access_inspection",bool(access_inspection_action_)},
             {"binding_restore_probe",bool(binding_probe_action_)&&!binding_probe_consumed_&&!nr_terminal_},
             {"input_preparation_probe",bool(preparation_probe_action_)&&!binding_probe_consumed_&&!nr_terminal_},
             {"binding_boundary_inspection",bool(binding_boundary_action_)&&!binding_probe_consumed_&&!nr_terminal_},
             {"capture_display_pair",display_pair_enabled_&&!nr_terminal_},
             {"capture_chain",chain_capture_enabled_&&!nr_terminal_&&!rebuilding},
             {"nr_frame_control",!nr_terminal_&&!rebuilding&&!nr_origin_.empty()},
             {"nr_compute_only",nr_compute_only_&&!nr_terminal_},
             {"nr_settings",!nr_terminal_&&!rebuilding&&nr_settings_enabled_},
             {"nr_style_choices",!nr_terminal_&&(nr_settings_enabled_||embedded_enabled_)?json::array({0,1,2}):json::array()},{"nr_style_abc_mapping_verified",false},
             {"nr_settings_staging",embedded_enabled_&&!nr_terminal_},
             {"nr_intent_control",nr_integrated_&&!nr_terminal_},
             {"nr_preparation",nr_preparation_.is_object()&&nr_preparation_.value("state","")=="waiting_for_user"},
             {"nr_adapter_diagnostics",nr_adapter_.is_object() && nr_adapter_.value("enabled",false)},
             {"observer_sampling_arm",observer_sampling_enabled_ && observer_.is_object() && observer_.value("recording",false) && !observer_sampling_consumed_},
             {"reason", nr_origin_=="game-rr-experimental-nr"?"Experimental RR-stage NR control; P0, color fidelity and stability are not certified":"Isolated NR probe is separate; this backend has no verified game resource/control adapter"}}}};
}
json Controller::status() const { std::lock_guard lock(mutex_); return status_locked(); }
void Controller::enable_binding_probe(std::function<bool(bool)> action){
    std::lock_guard lock(mutex_);
    if(!action||binding_probe_action_||nr_terminal_)throw std::logic_error("Binding probe already attached or backend failed");
    binding_probe_action_=std::move(action);binding_probe_={{"state","idle"},{"nr_evaluates",0},{"raw_files",0}};
}
void Controller::enable_binding_boundary_inspection(std::function<bool(bool)> action){
    std::lock_guard lock(mutex_);
    if(!action||binding_boundary_action_||nr_terminal_)throw std::logic_error("Binding boundary inspection already attached or backend failed");
    binding_boundary_action_=std::move(action);binding_boundaries_={{"state","idle"}};
    binding_boundary_summary_={{"state","idle"},{"row_count",0},{"raw_files",0},{"GPU_commands_by_inspection",0},{"NR_evaluates_by_inspection",0}};
}
void Controller::publish_binding_boundaries(json data){
    // Fixed four-call storage; details travel one call per RPC, never in GetStatus.
    bool valid=data.is_object()&&data.contains("rows")&&data["rows"].is_array()&&data["rows"].size()<=4;
    json summary;
    if(valid){summary=data;summary.erase("rows");summary["row_count"]=data["rows"].size();
        valid=summary.dump().size()<=2048;
        for(const auto& row:data["rows"])valid=valid&&row.dump().size()<=24576;}
    if(!valid){
        // A reporting failure must not break OFF/control or silently truncate evidence.
        data={{"state","failed"},{"error","boundary-report-shape-or-size"},{"rows",json::array()}};
        summary={{"state","failed"},{"error","boundary-report-shape-or-size"},{"row_count",0}};
    }
    std::lock_guard lock(mutex_);binding_boundaries_=std::move(data);binding_boundary_summary_=std::move(summary);
}
void Controller::publish_binding_probe(json data){
    data=bound_status(std::move(data),2048,"binding-probe");
    std::lock_guard lock(mutex_);binding_probe_=std::move(data);
}
void Controller::enable_preparation_probe(std::function<bool(bool)> action){
    std::lock_guard lock(mutex_);
    if(!action||preparation_probe_action_||nr_terminal_)throw std::logic_error("Preparation probe already attached or backend failed");
    preparation_probe_action_=std::move(action);preparation_probe_={{"state","idle"}};
}
void Controller::publish_preparation_probe(json data){
    data=bound_status(std::move(data),2048,"preparation-probe");
    std::lock_guard lock(mutex_);preparation_probe_=std::move(data);
}
void Controller::enable_post_inspection(std::function<bool(bool,std::uint64_t)> action){
    std::lock_guard lock(mutex_);
    if(!action||post_inspection_action_||access_inspection_action_||nr_terminal_||nr_desired_on_||!nr_origin_.empty()||nr_pending_||nr_inflight_)throw std::logic_error("Post diagnostic requires unprepared NR OFF");
    post_inspection_action_=std::move(action);post_inspection_={{"state","awaiting_device"}};
    nr_terminal_=true;nr_terminal_reason_="Post binding diagnostic process; NR disabled until normal restart";
}
void Controller::publish_post_inspection(json data){
    data=bound_status(std::move(data),8192,"post-inspection");
    std::lock_guard lock(mutex_);post_inspection_=std::move(data);
}
void Controller::enable_access_inspection(std::function<bool(bool,std::uint64_t)> action){
    std::lock_guard lock(mutex_);
    if(!action||access_inspection_action_||post_inspection_action_||nr_terminal_||nr_desired_on_||!nr_origin_.empty()||nr_pending_||nr_inflight_)
        throw std::logic_error("Access diagnostic requires unprepared NR OFF");
    access_inspection_action_=std::move(action);access_inspection_={{"state","awaiting_device"}};
    nr_terminal_=true;nr_terminal_reason_="Read-only resource access diagnostic; NR and capture disabled for this process";
}
void Controller::publish_access_inspection(json data){
    data=bound_status(std::move(data),32768,"access-inspection");
    std::lock_guard lock(mutex_);access_inspection_=std::move(data);
}
void Controller::enable_manual_capture(const std::filesystem::path& root,const std::string& origin){
    std::lock_guard lock(mutex_);
    if(capture_ || observer_sampling_enabled_)throw std::runtime_error("Manual capture backend already configured or legacy observer enabled");
    capture_=std::make_shared<ManualCapture>(root,origin);
}
void Controller::observe_present(std::uint64_t swapchain) noexcept {if(capture_)capture_->observe_present(swapchain);}
void Controller::stop_manual_capture(const std::string& reason){if(capture_)capture_->stop(reason);}
void Controller::enable_nr_frame_control(const std::string& origin,bool settings,bool pair){
    std::lock_guard lock(mutex_);
    if(nr_terminal_ || synthetic_ || !nr_origin_.empty() || observer_sampling_enabled_ ||
       (origin!="synthetic-input-real-nr"&&origin!="game-rr-experimental-nr"))
        throw std::logic_error("NR frame backend identity is not supported");
    nr_origin_=origin;nr_settings_enabled_=settings;nr_pair_enabled_=pair;
    if(pair)pair_status_={{"state","idle"},{"revision",0},{"files",0}};
    if(nr_preparation_.is_object())nr_preparation_["state"]="ready";
    nr_status_={{"state","idle"},{"requested_mode",nr::mode_name(nr_integrated_?nr_desired_on_:0u)},{"observed_mode","unknown"},
        {"requested_revision",0},{"applied_revision",nullptr},{"applied_frame",nullptr},
        {"game_control_available",origin=="game-rr-experimental-nr"},{"displayed_frame_verified",false}};
    if(nr_integrated_)nr_intent_dirty_=true;
}
void Controller::fail_nr_locked(const std::string& reason){
    if(!nr_terminal_)nr_terminal_reason_=reason.substr(0,1024);
    nr_terminal_=true;nr_pending_.reset();pair_pending_.reset();display_pair_pending_.reset();
    if(pair_active(display_pair_status_)){display_pair_status_["state"]="failed";display_pair_status_["error"]="Host/NR backend failed; GPU drain not certified";}
    nr_desired_on_=false;nr_intent_dirty_=false;
    if(nr_preparation_.is_object()){nr_preparation_["state"]="failed";nr_preparation_["error"]=nr_terminal_reason_;}
    if(nr_status_.is_object()){
        nr_status_["state"]="failed";nr_status_["observed_mode"]="unknown";nr_status_["game_control_available"]=false;
        nr_status_["error"]=nr_terminal_reason_;
    }
    // Retain an in-flight request for its matching receipt, not for new work.
    // Failure is not evidence that GPU work drained or that OFF was displayed.
}
void Controller::accept_game_profile(const std::string& id){
    std::lock_guard lock(mutex_);
    if(id.empty()||(!accepted_profile_.empty()&&accepted_profile_!=id))throw std::logic_error("Game profile already accepted");
    accepted_profile_=id;
}
void Controller::publish_nr_runtime(json snapshot){
    // A malformed report becomes a bounded failure record (NR fails closed);
    // an unknown profile stays an identity refusal that never mutates status.
    const bool malformed=!snapshot.is_object();
    snapshot=bound_status(std::move(snapshot),8*1024,"nr-runtime");
    const auto profile=snapshot.value("profile",std::string());
    std::lock_guard lock(mutex_);
    // Any compiled game profile, the legacy 2077 id, the synthetic fixture, or
    // the one profile this process's validated installation declared.
    const bool known_profile=profiles::game(profile)!=nullptr||profile=="cyberpunk-rr-experimental-v1"||profile=="synthetic-standalone-fixture"||
        (!accepted_profile_.empty()&&profile==accepted_profile_);
    if(!malformed&&!known_profile)throw std::logic_error("Unknown live NR status profile");
    nr_runtime_=std::move(snapshot);
    if(nr_runtime_.value("state","")=="failed"||nr_runtime_.value("state","")=="stopped")
        fail_nr_locked(nr_runtime_.value("error","Live NR backend stopped"));
    else if(!nr_terminal_&&nr_rebuild_serial_&&nr_runtime_.value("rebuild_serial",0ULL)==nr_rebuild_serial_&&nr_status_.value("state","")=="rebuilding"){
        const auto bypass=nr_runtime_.value("bypass_frame",0ULL),changed=nr_runtime_.value("rebuild_frame",0ULL);
        if(changed&&bypass>=changed){nr_status_["observed_mode"]="off";nr_status_["bypass_frame"]=bypass;}
        if(nr_runtime_.value("state","")=="ready")nr_status_["state"]="idle";
    }
    queue_nr_intent_locked("prepared-or-safe-rebuild");
}
void Controller::interrupt_nr_for_rebuild(std::uint64_t serial,std::uint64_t frame){
    std::lock_guard lock(mutex_);if(nr_terminal_||serial<=nr_rebuild_serial_||!frame)return;
    nr_rebuild_serial_=serial;
    nr_status_["interrupted_requests"]=json::array();
    for(const auto* item:{&nr_inflight_,&nr_pending_})if(*item)nr_status_["interrupted_requests"].push_back({{"revision",(**item).at("revision")},{"reason","resource-rebuild"}});
    nr_inflight_.reset();nr_pending_.reset();
    pair_pending_.reset();
    display_pair_pending_.reset();
    if(pair_active(display_pair_status_)){display_pair_status_["state"]="cancelled";display_pair_status_["error"]="Resource rebuild cancelled display scheduling; runtime drain tracked separately";}
    if(pair_status_.is_object()&&(pair_status_.value("state","")=="requested"||pair_status_.value("state","")=="preparing")){
        pair_status_["state"]="cancelled";pair_status_["error"]="Resource rebuild cancelled capture scheduling";
    }
    ++revision_;nr_status_["state"]="rebuilding";nr_status_["requested_mode"]=nr::mode_name(nr_integrated_?nr_desired_on_:0u);nr_status_["observed_mode"]="unknown";
    if(nr_integrated_)nr_intent_dirty_=true;
    nr_status_["requested_revision"]=revision_;nr_status_["applied_revision"]=nullptr;nr_status_["applied_frame"]=nullptr;
    nr_status_["nr_evaluated"]=false;nr_status_["nr_output_selected"]=false;
    nr_status_["rebuild_serial"]=serial;nr_status_["rebuild_frame"]=frame;
    nr_status_["notice"]=nr_integrated_?"Input unavailable; bypassing until safe preparation. Only the current controller intent can resume NR.":
        "Input lifecycle interrupted; old requests cancelled. NR remains OFF after preparation; enable explicitly.";
}
std::optional<json> Controller::take_nr_mode_request(std::uint64_t now){
    std::lock_guard lock(mutex_);expire(now);
    if(nr_integrated_&&(!nr_runtime_.is_object()||nr_runtime_.value("state","")!="ready"))return std::nullopt;
    if(nr_terminal_ || nr_inflight_ || !nr_pending_)return std::nullopt;
    nr_inflight_=std::move(nr_pending_);nr_pending_.reset();nr_status_["state"]="applying";
    return nr_inflight_;
}
void Controller::acknowledge_nr_mode(std::uint64_t revision,std::uint64_t frame,bool success,
    bool nr_evaluated,bool output_selected,const std::string& error){
    std::lock_guard lock(mutex_);
    if(!nr_inflight_ || nr_inflight_->at("revision")!=revision || !frame)
        throw std::logic_error("No matching NR frame request to acknowledge");
    const auto mode=nr_inflight_->at("mode").get<std::string>();
    if(success && (nr_evaluated!=nr::executes(mode) || output_selected!=(mode=="on")))
        throw std::logic_error("NR mode acknowledgement contradicts executed/selected path");
    if(success&&!nr_terminal_&&nr_inflight_->contains("settings")){
        const auto settings=nr_runtime_.is_object()?nr_runtime_.value("settings",json::object()):json::object();
        if(settings.value("observed_revision",0ULL)!=revision||settings.value("frame",0ULL)!=frame||
           settings.value("read_mask",0u)!=3u||!settings.value("style_read",false)||settings.value("observed",json(nullptr))!=nr_inflight_->at("settings"))
            throw std::logic_error("Settings acknowledgement lacks matching DLL reads and frame");
    }
    if(nr_terminal_){
        nr_status_["ack_after_terminal"]={{"revision",revision},{"frame",frame},{"mode",mode},{"success",success},
            {"nr_evaluated",nr_evaluated},{"nr_output_selected",output_selected},{"error",error.substr(0,1024)}};
        nr_inflight_.reset();return; // Preserve receipt without resurrecting a failed backend.
    }
    if(success){
        nr_status_["observed_mode"]=mode;nr_status_["applied_revision"]=revision;nr_status_["applied_frame"]=frame;
        nr_status_["nr_evaluated"]=nr_evaluated;nr_status_["nr_output_selected"]=output_selected;
        nr_status_["state"]=nr_pending_?"requested":"applied";
    }else{
        fail_nr_locked(error);
    }
    nr_inflight_.reset();
}
void Controller::diagnostic(const std::string& text) { std::lock_guard lock(mutex_); last_error_ = text; }
std::optional<json> Controller::take_pair_request(std::uint64_t now){
    std::lock_guard lock(mutex_);expire(now);if(nr_terminal_||!pair_pending_)return std::nullopt;
    auto request=std::move(pair_pending_);pair_pending_.reset();pair_status_["state"]=request->value("cancel",false)?"cancelling":"preparing";return request;
}
void Controller::publish_pair(json snapshot){
    snapshot=bound_status(std::move(snapshot),4096,"frame-pair");
    std::lock_guard lock(mutex_);if(!nr_pair_enabled_)throw std::logic_error("Pair backend not enabled");
    // Do not replace a queued request with the older worker receipt.
    if(pair_status_.is_object()&&snapshot.value("revision",0ULL)<pair_status_.value("revision",0ULL))return;
    if(terminal_pair(pair_status_)&&snapshot.value("revision",0ULL)==pair_status_.value("revision",0ULL)&&!terminal_pair(snapshot))return;
    pair_status_=std::move(snapshot);
}
void Controller::enable_chain_capture(){std::lock_guard lock(mutex_);
    if(!nr_pair_enabled_||chain_capture_enabled_||synthetic_)throw std::logic_error("Chain capture requires the attached pair backend, once");
    chain_capture_enabled_=true;
}
void Controller::enable_display_capture(){std::lock_guard lock(mutex_);
    if(!nr_integrated_||display_pair_enabled_||synthetic_)throw std::logic_error("Display capture requires integrated host");
    display_pair_enabled_=true;display_pair_status_={{"state","idle"},{"revision",0u},{"files",0u}};
}
std::optional<json> Controller::take_display_capture_request(std::uint64_t now){std::lock_guard lock(mutex_);expire(now);
    if(nr_terminal_||!display_pair_pending_)return std::nullopt;
    if(!display_pair_pending_->value("cancel",false)&&(!nr_runtime_.is_object()||nr_runtime_.value("state","")!="ready"||
       nr_status_.value("observed_mode","")!="off"||nr_pending_||nr_inflight_))return std::nullopt;
    auto action=std::move(display_pair_pending_);display_pair_pending_.reset();display_pair_status_["state"]=action->value("cancel",false)?"cancelling":"preparing";return action;
}
void Controller::publish_display_pair(json snapshot){snapshot=bound_status(std::move(snapshot),4096,"display-pair");
    std::lock_guard lock(mutex_);if(!display_pair_enabled_)throw std::logic_error("Display backend not enabled");
    if(snapshot.value("revision",0ULL)<display_pair_status_.value("revision",0ULL))return;
    if(terminal_pair(display_pair_status_)&&snapshot.value("revision",0ULL)==display_pair_status_.value("revision",0ULL)&&!terminal_pair(snapshot))return;
    display_pair_status_=std::move(snapshot);
}
bool Controller::dispatch_pair_request(std::uint64_t now,const std::function<bool(const json&)>& send){
    std::lock_guard lock(mutex_);expire(now);return dispatch_capture_locked(false,send);
}
bool Controller::dispatch_display_capture_request(std::uint64_t now,const std::function<bool(const json&)>& send){
    std::lock_guard lock(mutex_);expire(now);return dispatch_capture_locked(true,send);
}
bool Controller::dispatch_capture_locked(bool display,const std::function<bool(const json&)>& send){
    auto& pending=display?display_pair_pending_:pair_pending_;auto& status=display?display_pair_status_:pair_status_;
    if(nr_terminal_||!pending)return false;
    const bool cancel=pending->value("cancel",false);
    const auto mode=nr_status_.value("observed_mode","");
    // A scoped chain request may be delivered with NR OFF; the unscoped pair may not.
    const bool scoped=!display&&pending->contains("scope");
    if(!cancel&&(nr_pending_||nr_inflight_||(display?mode!="off":(!nr::executes(mode)&&!scoped))))return false;
    if(!cancel&&nr_integrated_&&(!nr_runtime_.is_object()||nr_runtime_.value("state","")!="ready"))return false;
    // Serialize cancellation/lease expiry with submission. There is no second
    // host-owned queue which can deliver a request after Controller cancels it.
    // A throwing delivery fails NR closed here; it is not rethrown into the
    // host worker, whose other duties (OFF, status, panel) must keep running.
    const auto delivery_failed=[&](const std::string& what){pending.reset();status["state"]="failed";status["failure_code"]="delivery_exception";
        status["error"]="Capture delivery threw: "+what+"; acceptance/GPU drain unknown, no retry";status["files"]=0;status["bytes"]=0;
        fail_nr_locked("Capture delivery threw; acceptance/GPU drain unknown, no retry");return false;};
    try{if(!send(*pending))return false;}
    catch(const std::exception& e){return delivery_failed(e.what());}
    catch(...){return delivery_failed("non-standard exception");}
    pending.reset();status["state"]=cancel?"cancelling":"preparing";return true;
}
void Controller::publish_host(json value){
    value=bound_status(std::move(value),8192,"host");
    std::lock_guard lock(mutex_);host_=std::move(value);
    if(host_.value("state","")=="failed"||host_.value("state","")=="stopped"){
        if(post_inspection_action_){auto cancel=std::move(post_inspection_action_);cancel(true,0);
            post_inspection_["backend_stopped"]=true;}
        if(access_inspection_action_){auto cancel=std::move(access_inspection_action_);cancel(true,0);
            access_inspection_["backend_stopped"]=true;}
        fail_nr_locked(host_.value("error","Host is unavailable"));
    }
}
void Controller::publish_nr_adapter(json snapshot) {
    // The shape check is a capability guard and stays a refusal; only the size is clipped.
    if(!snapshot.is_object() || snapshot.value("mode","")!="admission-only" ||
       !snapshot.contains("game_control_available") || snapshot["game_control_available"]!=false ||
       !snapshot.contains("render_admission") || snapshot["render_admission"]!=false || snapshot.value("nr_executed",true))
        throw std::runtime_error("Only bounded non-rendering NR admission diagnostics may be published");
    snapshot=bound_status(std::move(snapshot),8*1024,"nr-adapter");
    std::lock_guard lock(mutex_);nr_adapter_=std::move(snapshot);
}
void Controller::publish_observer(json snapshot) {
    // Reserve room for protocol/status metadata within the 64 KiB pipe limit.
    snapshot=bound_status(std::move(snapshot),48*1024,"observer");
    std::lock_guard lock(mutex_);
    observer_ = std::move(snapshot);
    if(observer_sampling_enabled_ && observer_.contains("recording") && observer_["recording"]==false){
        observer_sampling_pending_.reset();
        observer_sampling_["state"]=observer_sampling_consumed_?"stopped_after_arm":"stopped_before_scene";
        observer_sampling_["stop_reason"]=observer_.value("stop_reason","unknown");
    }
}
void Controller::enable_observer_sampling(){
    std::lock_guard lock(mutex_);
    if(synthetic_ || observer_sampling_enabled_)throw std::runtime_error("Observer sampling capability cannot be enabled here");
    observer_sampling_enabled_=true;observer_sampling_={{"state","awaiting_scene"},{"NR_control",false}};
}
std::optional<json> Controller::take_observer_sampling_request(std::uint64_t now){
    std::lock_guard lock(mutex_);expire(now);
    if(!observer_sampling_pending_)return std::nullopt;
    auto request=std::move(observer_sampling_pending_);observer_sampling_pending_.reset();observer_sampling_consumed_=true;
    observer_sampling_["state"]="applying";return request;
}
void Controller::acknowledge_observer_sampling(json applied){
    if(!applied.is_object() || applied.dump().size()>4096)throw std::runtime_error("Observer arm acknowledgement too large");
    std::lock_guard lock(mutex_);
    if(!observer_sampling_consumed_ || observer_sampling_.value("state","")!="applying")
        throw std::runtime_error("No consumed scene request to acknowledge");
    observer_sampling_["state"]="warmup";observer_sampling_["backend_acknowledgement"]=std::move(applied);
}
void Controller::release_owner_locked(const char* reason) {
        if(binding_probe_action_)binding_probe_action_(true);
        if(preparation_probe_action_)preparation_probe_action_(true);
        if(binding_boundary_action_)binding_boundary_action_(true);
        if(post_inspection_action_)post_inspection_action_(true,0);
        if(access_inspection_action_)access_inspection_action_(true,0);
        if(display_pair_enabled_&&pair_active(display_pair_status_)){
            if(display_pair_pending_&&!display_pair_pending_->value("cancel",false)){display_pair_pending_.reset();display_pair_status_["state"]="cancelled";}
            else display_pair_pending_=json{{"cancel",true},{"revision",revision_+1}};
        }
        if(nr_integrated_){nr_desired_on_=false;nr_intent_dirty_=!nr_terminal_;
            if(nr_status_.is_object())nr_status_["requested_mode"]="off";
            if(nr_preparation_.value("state","")=="requested"){nr_preparation_["state"]="standby";nr_intent_dirty_=false;nr_status_["state"]="standby";}}
        if(capture_)capture_->stop("controller_disconnected");
        if(nr_pair_enabled_&&!nr_terminal_&&pair_active(pair_status_)){
            if(pair_pending_&&!pair_pending_->value("cancel",false)){pair_pending_.reset();pair_status_["state"]="cancelled";}
            else pair_pending_=json{{"cancel",true},{"revision",revision_+1}};
        }
        if(!nr_terminal_ && !nr_origin_.empty()&&nr_status_.value("state","")!="rebuilding"){
            // Cancel unconsumed ON and enqueue OFF at the next backend boundary.
            // In-flight work is not falsely reported as stopped or released.
            ++revision_;
            nr_pending_=json{{"mode","off"},{"revision",revision_},{"reason",reason}};
            nr_status_["requested_mode"]="off";nr_status_["requested_revision"]=revision_;
            nr_status_["state"]="requested";
        }
        state_ = "cancelled";
        pending_.reset();
        if(nr_preparation_.is_object()&&nr_preparation_.value("state","")=="requested")nr_preparation_["state"]=nr_integrated_?"standby":"waiting_for_user";
        if(observer_sampling_pending_){observer_sampling_pending_.reset();observer_sampling_["state"]="cancelled_before_backend";}
        owner_=embedded_enabled_?"@embedded":"";
        last_error_ = reason;
}
void Controller::expire(std::uint64_t now) {
    if (!owner_.empty() && owner_!="@embedded" && now >= last_seen_ && now - last_seen_ > 10000)
        release_owner_locked("control lease expired; safe OFF requested and no new experiment actions scheduled");
    for(bool display:{false,true}){
        auto& pending=display?display_pair_pending_:pair_pending_;auto& status=display?display_pair_status_:pair_status_;
        if(!pending||pending->value("cancel",false))continue; // Cancellation may need to drain already submitted GPU work.
        const auto start=pending->at("requested_at_ms").get<std::uint64_t>();
        if(now>=start&&now-start<capture_dispatch_timeout_ms)continue;
        pending.reset();status["state"]="failed";status["failure_code"]="dispatch_timeout";
        status["error"]="Capture not accepted within 10 seconds; no copy scheduled, explicit new request required";
        status["files"]=0;status["bytes"]=0;
        if(display)display_pair_release_preparation_locked();
    }
}
void Controller::display_pair_prepare_locked(const json& request_id){
    // Display capture needs a prepared, never Evaluating, bridge. This and the
    // release below are the only writes display capture makes to nr_preparation_.
    if(!display_pair_enabled_||!nr_preparation_.is_object()||!nr_origin_.empty()||nr_preparation_.value("state","")!="standby")return;
    nr_preparation_["state"]="requested";nr_preparation_["revision"]=revision_+1;nr_preparation_["request_id"]=request_id;
    nr_desired_on_=false;nr_intent_dirty_=true; // Prepare the existing bridge, never request Evaluate.
}
void Controller::display_pair_release_preparation_locked(){
    if(!display_pair_enabled_||!nr_preparation_.is_object()||!nr_origin_.empty()||nr_desired_on_||nr_preparation_.value("state","")!="requested")return;
    nr_preparation_["state"]="standby";nr_intent_dirty_=false;
}
void Controller::frame_boundary(std::uint64_t frame, std::uint64_t now) {
    std::lock_guard lock(mutex_);
    expire(now);
    if (pending_ && synthetic_) {
        observed_ = *pending_;
        pending_.reset();
        applied_frame_ = frame;
        applied_revision_ = revision_;
    }
}

void Controller::enable_embedded_control(){
    std::lock_guard lock(mutex_);
    if(embedded_enabled_||!nr_integrated_||!owner_.empty()||nr_terminal_)throw std::logic_error("Embedded control needs an unowned integrated host");
    embedded_enabled_=true;owner_="@embedded";
}
json Controller::handle(const json& request,std::uint64_t now){return handle_impl(request,now,false);}
json Controller::handle_embedded(const std::string& method,const json& params,std::uint64_t now){
    return handle_impl({{"protocol","1.0"},{"method",method},{"params",params}},now,true);
}
json Controller::handle_impl(json request, std::uint64_t now,bool embedded) {
    std::lock_guard lock(mutex_);
    expire(now);
    std::string id;
    auto fail = [&](const char* code, const char* message) {
        return json{{"protocol", "1.0"}, {"request_id", id}, {"ok", false},
                    {"error", {{"code", code}, {"message", message}}}, {"status", status_locked()}};
    };
    try {
        if(embedded){
            if(!embedded_enabled_)return fail("unsupported","Embedded control is not attached");
            request["request_id"]="local-"+std::to_string(++embedded_sequence_);request["client_id"]="@embedded";
            request["session_id"]=session_;request["expected_revision"]=revision_;
        }
        if (!request.is_object() || request.value("protocol", "") != "1.0")
            return fail("bad_protocol", "Expected protocol 1.0 object");
        id = request.at("request_id").get<std::string>();
        auto client = request.at("client_id").get<std::string>();
        auto method = request.at("method").get<std::string>();
        if(!embedded&&client=="@embedded")return fail("reserved_client","Embedded writer is not a pipe client");
        if (id.empty() || id.size() > 128 || client.empty() || client.size() > 128)
            return fail("bad_request", "Invalid request/client id");
        const std::string cache_key = json::array({client, id}).dump();
        const bool read_only = method == "Hello" || method == "GetStatus" || method == "GetCapabilities" || method == "GetBindingBoundaries";
        // Polling does not consume the bounded mutation-id cache.
        if (!read_only&&!embedded) {
            const auto found = responses_.find(cache_key);
            if (found != responses_.end()) {
                if (found->second.first != request.dump()) return fail("id_reused", "Request id reused with different payload");
                return found->second.second;
            }
            if (responses_.size() >= 4096) return fail("session_full", "Restart host to begin a new idempotency session");
        }
        if (client == owner_) last_seen_ = now;
        auto dispatch = [&]() -> json {
            if(method=="GetBindingBoundaries"){
                if(!binding_boundary_action_)return fail("unsupported","Binding inspection is not attached");
                if(request.value("session_id","")!=session_)return fail("stale_session","Boundary details require the current session");
                const auto p=request.value("params",json::object());
                if(!p.is_object()||p.size()!=1||!p.contains("call_index")||!p["call_index"].is_number_integer()||p["call_index"]<0||p["call_index"]>3)
                    return fail("bad_request","Expected call_index in [0,3]");
                const auto phase=binding_boundary_summary_.value("state","");
                if(phase=="idle"||phase=="armed")return fail("not_ready","Read terminal inspection details only");
                const auto index=p["call_index"].get<unsigned>();
                if(!binding_boundaries_.contains("rows")||index>=binding_boundaries_["rows"].size())return fail("missing_call","Requested call was not recorded");
                return {{"protocol","1.0"},{"request_id",id},{"ok",true},{"pid",GetCurrentProcessId()},{"session_id",session_},
                    {"binding_boundaries",binding_boundary_summary_},{"call_index",index},{"row",binding_boundaries_["rows"][index]}};
            }
            if (read_only) return {{"protocol", "1.0"}, {"request_id", id}, {"ok", true}, {"status", status_locked()}};
            const bool capture_method=method=="StartCapture" || method=="StopCapture";
            const bool settings_method=method=="SetNrSettings";
            const bool staged_settings=embedded_enabled_&&settings_method;
            const bool ownership_method=method=="TakeControl"||method=="ReleaseControl";
            const bool nr_method=method=="SetNrMode"||settings_method;
            const bool intent_method=nr_integrated_&&method=="SetNrMode";
            const bool prepare_method=method=="PrepareNr";
            const bool pair_method=method=="CapturePair"||method=="CancelCapturePair";
            const bool display_method=method=="CaptureDisplayPair"||method=="CancelDisplayPair";
            const bool post_method=method=="InspectPostBindings"||method=="CancelPostInspection";
            const bool access_method=method=="InspectNrAccess"||method=="CancelNrAccess";
            const bool preparation_probe_method=method=="ProbeInputPreparation"||method=="CancelInputPreparationProbe";
            const bool boundary_method=method=="InspectBindingBoundaries"||method=="CancelBindingBoundaries";
            const bool binding_method=method=="ProbeBindingRestore"||method=="CancelBindingRestoreProbe"||preparation_probe_method||boundary_method;
            const auto& probe_action=boundary_method?binding_boundary_action_:preparation_probe_method?preparation_probe_action_:binding_probe_action_;
            if(binding_method&&(!probe_action||embedded))return fail("unsupported","Explicit external insertion diagnostic is not attached");
            if(access_method&&!access_inspection_action_)return fail("unsupported","Read-only access diagnostic is not attached");
            if(post_method&&!post_inspection_action_)return fail("unsupported","Post inspection diagnostic is not attached");
            if(display_method&&!display_pair_enabled_)return fail("unsupported","No display-pair backend attached");
            if(pair_method&&!nr_pair_enabled_)return fail("unsupported","No verified same-frame capture backend");
            if(prepare_method&&!nr_preparation_.is_object())return fail("unsupported","No explicit NR preparation backend");
            if(capture_method && !capture_)return fail("unsupported","No manual capture backend attached");
            if(nr_method && nr_origin_.empty()&&!intent_method&&!staged_settings)return fail("unsupported","No real NR frame adapter attached");
            if(ownership_method&&(!embedded_enabled_||embedded))return fail("unsupported","Only external clients explicitly take or release embedded control");
            if (!synthetic_ && method!="ArmObservation" && !capture_method && !nr_method && !prepare_method&&!pair_method&&!ownership_method&&!display_method&&!post_method&&!access_method&&!binding_method) return fail("unsupported", "Observer does not execute NR/game mutations");
            if (request.value("session_id", "") != session_) return fail("stale_session", "Hello required for this host session");
            if (!request.contains("expected_revision") || !request["expected_revision"].is_number_unsigned()
                || request["expected_revision"].get<std::uint64_t>() != revision_)
                return fail("stale_revision", "Refresh status before mutation");
            if(method=="TakeControl"){
                if(owner_!="@embedded"&&owner_!=client)return fail("control_busy","Another external writer owns control");
                if(request.at("params")!=json::object())return fail("bad_request","TakeControl params must be empty");
                owner_=client;last_seen_=now;++revision_;
                return {{"protocol","1.0"},{"request_id",id},{"ok",true},{"status",status_locked()}};
            }
            if (!owner_.empty() && owner_ != client) return fail(embedded_enabled_&&owner_=="@embedded"?"takeover_required":"control_busy", "Explicit TakeControl required, or another controller owns the experiment");
            if(method=="ReleaseControl"){
                if(request.at("params")!=json::object())return fail("bad_request","ReleaseControl params must be empty");
                release_owner_locked("external control released; safe OFF requested");++revision_;
                return {{"protocol","1.0"},{"request_id",id},{"ok",true},{"status",status_locked()}};
            }
            if(nr_terminal_&&(prepare_method||nr_method||pair_method||display_method))return fail("backend_failed","Host/NR backend is unavailable; no new work was scheduled");
            if(binding_method){
                const bool cancel=method=="CancelBindingRestoreProbe"||method=="CancelInputPreparationProbe"||method=="CancelBindingBoundaries";const auto& p=request.at("params");
                if(cancel){if(p!=json::object())return fail("bad_config","Cancel params must be empty");}
                else{
                    if(p!=json{{"scene_confirmed",true},{"fg_disabled_confirmed",true}})return fail("scene_required","This process scene and FG OFF must be confirmed");
                    if(binding_probe_consumed_)return fail("consumed","One insertion diagnostic attempt per process; no retry or mixed phases");
                    if(nr_terminal_||!nr_runtime_.is_object()||nr_runtime_.value("state","")!="ready"||
                       nr_desired_on_||!nr_status_.is_object()||nr_status_.value("observed_mode","")!="off"||
                       nr_pending_||nr_inflight_||nr_runtime_.value("pending_gpu",true)||nr_runtime_.value("evaluates",1ULL)!=0||
                       pair_active(pair_status_)||pair_active(display_pair_status_))return fail("nr_not_off","Fresh prepared OFF, zero NR evaluations and no pending work required");
                }
                if(!probe_action(cancel))return fail("probe_not_ready","Insertion probe not accepted; no automatic retry scheduled");
                if(!cancel)binding_probe_consumed_=true;
                owner_=client;last_seen_=now;++revision_;
                json answer={{"protocol","1.0"},{"request_id",id},{"ok",true},{"status",status_locked()}};
                responses_.emplace(cache_key,std::make_pair(request.dump(),answer));return answer;
            }
            // A diagnostic process stays OFF after its one attempt, including
            // cancellation/expiry. Do not mix it with another NR visual trial.
            if(binding_probe_consumed_&&((method=="SetNrMode"&&request.at("params").value("mode",std::string())!="off")||
                settings_method||pair_method||display_method||capture_method))return fail("diagnostic_session","Insertion diagnostic consumed; NR stays OFF until normal restart");
            if(access_method){
                const bool cancel=method=="CancelNrAccess";const auto& p=request.at("params");
                if(cancel){if(p!=json::object())return fail("bad_config","CancelNrAccess params must be empty");}
                else if(p!=json{{"scene_confirmed",true},{"fg_disabled_confirmed",true}})
                    return fail("scene_required","Current scene and FG OFF must be confirmed; HDR setting is unchanged");
                if(!access_inspection_action_(cancel,now))return fail("access_not_ready","Device unavailable or attempt consumed; no automatic retry");
                owner_=client;last_seen_=now;++revision_;
                return {{"protocol","1.0"},{"request_id",id},{"ok",true},{"status",status_locked()}};
            }
            if(post_method){
                const bool cancel=method=="CancelPostInspection";const auto& p=request.at("params");
                if(cancel){if(p!=json::object())return fail("bad_config","CancelPostInspection params must be empty");}
                else if(p!=json{{"scene_confirmed",true},{"fg_disabled_confirmed",true},{"sdr_confirmed",true}})
                    return fail("scene_required","Current process scene, SDR and FG OFF must be explicitly confirmed");
                if(!post_inspection_action_(cancel,now))return fail("post_not_ready","Device not ready, attempt consumed, or inspection failed; no retry scheduled");
                owner_=client;last_seen_=now;++revision_;
                return {{"protocol","1.0"},{"request_id",id},{"ok",true},{"status",status_locked()}};
            }
            if(method=="SetNrMode"&&nr::executes(request.at("params").value("mode",std::string()))&&pair_active(display_pair_status_))
                return fail("display_capture_active","Display color diagnosis requires NR OFF until capture retires");
            if(((nr_method&&!intent_method&&!staged_settings)||method=="CapturePair")&&nr_status_.is_object()&&nr_status_.value("state","")=="rebuilding")
                return fail("rebuilding","NR is bypassed while resources are rebuilt; wait for ready before a new ON/settings/capture request");
            if (method == "ResetHistory" || method == "RecreateFeature")
                return fail("unsupported", "No dynamically verified NR resource backend; command was not executed");
            if(display_method){
                const auto& p=request.at("params");const bool cancel=method=="CancelDisplayPair";
                if(cancel){if(p!=json::object())return fail("bad_config","CancelDisplayPair params must be empty");
                    if(display_pair_pending_&&!display_pair_pending_->value("cancel",false)){display_pair_pending_.reset();display_pair_status_["state"]="cancelled";
                        display_pair_release_preparation_locked();}
                    else if(pair_active(display_pair_status_))display_pair_pending_=json{{"cancel",true},{"revision",revision_+1}};
                }else{
                    if(!p.is_object()||p.size()!=2||!p.contains("scene_confirmed")||!p.contains("fg_disabled_confirmed")||
                       !p["scene_confirmed"].is_boolean()||p["scene_confirmed"]!=true||!p["fg_disabled_confirmed"].is_boolean()||p["fg_disabled_confirmed"]!=true)
                        return fail("scene_required","Manual scene_confirmed and fg_disabled_confirmed required; not runtime proof");
                    if(pair_active(display_pair_status_)||display_pair_pending_)return fail("pending","Only one display capture may be scheduled");
                    if(nr_desired_on_||nr_status_.value("observed_mode","")!="off"||nr_pending_||nr_inflight_||pair_active(pair_status_))return fail("nr_not_off","Acknowledged NR OFF and no pending NR/capture work required");
                    display_pair_status_={{"state","requested"},{"revision",revision_+1},{"files",0u}};
                    display_pair_pending_=json{{"cancel",false},{"revision",revision_+1},{"request_id",id},{"requested_at_ms",now},{"manual_confirmations",p}};
                    display_pair_prepare_locked(id);
                }
            }else if(staged_settings){
                const auto& params=request.at("params");
                json parsed;if(!parse_settings(params,desired_settings_,parsed))
                    return fail("bad_config",bad_settings_message);
                desired_settings_=std::move(parsed);
                settings_revision_=revision_+1;
                if(nr_desired_on_)set_nr_intent_locked(nr_desired_on_,id);
            }else if(intent_method){
                const auto& params=request.at("params");
                if(!params.is_object()||params.size()!=1||!params.contains("mode")||!params["mode"].is_string()||
                   (params["mode"]!="on"&&params["mode"]!="off"&&params["mode"]!="compute-only"))return fail("bad_config","SetNrMode accepts off, on or compute-only");
                if(params["mode"]=="compute-only"&&!nr_compute_only_)return fail("unsupported","Backend has no compute-only capability");
                set_nr_intent_locked(nr::mode_value(params["mode"].get<std::string>()),id);
            }else if(pair_method){
                const bool cancel=method=="CancelCapturePair";const auto& p=request.at("params");
                // {} keeps the six-file same-call pair. A scope asks for one chain
                // capture: "chain" = pair (NR ON only) + reconstructor
                // input/auxiliary tags + the Presents after the anchor call;
                // "presents" = the Presents alone. fg_declared is
                // the operator's statement, never runtime proof. Paths and frame
                // ids stay outside the client's control.
                std::string scope,fg="unknown";
                if(p!=json::object()){
                    if(cancel)return fail("bad_config","CancelCapturePair params must be empty");
                    if(!p.is_object()||!p.contains("scope")||p.size()>2||(p.size()==2&&!p.contains("fg_declared")))
                        return fail("bad_config","CapturePair accepts {} or {scope, fg_declared?}; capture paths and arbitrary frame ids are not client-controlled");
                    if(!p["scope"].is_string()||(p["scope"]!="chain"&&p["scope"]!="presents"))return fail("bad_config","scope must be chain or presents");
                    if(p.contains("fg_declared")&&(!p["fg_declared"].is_string()||(p["fg_declared"]!="off"&&p["fg_declared"]!="on"&&p["fg_declared"]!="unknown")))
                        return fail("bad_config","fg_declared must be off, on or unknown");
                    if(!chain_capture_enabled_)return fail("unsupported","No chain capture backend attached");
                    scope=p["scope"].get<std::string>();fg=p.value("fg_declared",std::string("unknown"));
                }
                if(!cancel){
                    const auto phase=pair_status_.value("state","");
                    if(pair_pending_||(phase!="idle"&&phase!="complete"&&phase!="cancelled"&&phase!="failed"))return fail("pending","Only one pair may be armed or writing");
                    // The unscoped pair holds NR stages, so NR must execute. A scoped
                    // chain may run with NR OFF (its pair item then records why).
                    if((scope.empty()&&!nr::executes(nr_status_.value("observed_mode","")))||nr_pending_||nr_inflight_)return fail("nr_off","An acknowledged NR evaluation mode and no pending setting are required");
                    pair_status_={{"state","requested"},{"revision",revision_+1},{"files",0}};
                    if(!scope.empty()){pair_status_["scope"]=scope;pair_status_["fg_declared"]=fg;}
                }
                if(cancel&&pair_pending_&&!pair_pending_->value("cancel",false)){pair_pending_.reset();pair_status_["state"]="cancelled";}
                else if(!cancel||pair_active(pair_status_)){pair_pending_=json{{"cancel",cancel},{"revision",revision_+1},{"request_id",id},{"requested_at_ms",now}};
                    if(!scope.empty()){(*pair_pending_)["scope"]=scope;(*pair_pending_)["fg_declared"]=fg;}}
            }else if(prepare_method){
                const bool standby=nr_integrated_&&nr_origin_.empty()&&!nr_desired_on_&&nr_preparation_.value("state","")=="standby";
                if(!standby&&nr_preparation_.value("state","")!="waiting_for_user")return fail("already_prepared","Preparation already scheduled or consumed");
                const auto& params=request.at("params");
                if(!params.is_object()||params.size()!=1||!params.contains("scene_confirmed")||!params["scene_confirmed"].is_boolean()||!params["scene_confirmed"].get<bool>())
                    return fail("scene_required","Explicit scene_confirmed=true required; this is manual confirmation, not runtime proof");
                nr_preparation_["state"]="requested";nr_preparation_["revision"]=revision_+1;nr_preparation_["request_id"]=id;
                if(standby){nr_desired_on_=false;nr_intent_dirty_=true;} // Explicit diagnostics may prepare while remaining OFF.
            }else if(nr_method){
                if(nr_status_.value("state","")=="failed")return fail("backend_failed","NR failure requires a new safe backend session");
                if(nr_pending_ || nr_inflight_)return fail("pending","Previous NR request has not reached an acknowledged frame boundary");
                const auto& params=request.at("params");
                if(settings_method){
                    if(!nr_settings_enabled_)return fail("unsupported","No dynamically verified settings backend");
                    if(!nr::executes(nr_status_.value("observed_mode","")))return fail("nr_off","Enable NR first; settings need an actual Evaluate receipt");
                    json parsed;if(!parse_settings(params,desired_settings_,parsed))
                        return fail("bad_config",bad_settings_message);
                    desired_settings_=parsed;
                    nr_pending_=json{{"mode",nr_status_.at("observed_mode")},{"settings",parsed},
                        {"revision",revision_+1},{"request_id",id},{"request",request}};
                    nr_status_["requested_revision"]=revision_+1;nr_status_["state"]="requested";
                }else{
                if(!params.is_object() || params.size()!=1 || !params.contains("mode") || !params["mode"].is_string())
                    return fail("bad_config","SetNrMode accepts only mode: off or on");
                const auto mode=params["mode"].get<std::string>();
                if(mode!="off" && mode!="on" && mode!="compute-only")return fail("bad_config","Unknown NR mode");
                if(mode=="compute-only"&&!nr_compute_only_)return fail("unsupported","Backend has no compute-only capability");
                nr_pending_=json{{"mode",mode},{"revision",revision_+1},{"request_id",id},{"request",request}};
                nr_status_["requested_mode"]=mode;nr_status_["requested_revision"]=revision_+1;nr_status_["state"]="requested";
                }
            }else if(capture_method){
                try {
                    if(method=="StartCapture")capture_->start(request.at("params"));
                    else {if(request.at("params")!=json::object())return fail("bad_request","StopCapture params must be empty");capture_->stop();}
                }catch(const std::runtime_error& e){return fail("capture_refused",e.what());}
            } else if (method == "ArmObservation") {
                if(synthetic_ || !observer_sampling_enabled_ || !observer_.is_object() || !observer_.value("recording",false))
                    return fail("unsupported","Live observer with explicit scene arming required");
                if(observer_sampling_consumed_ || observer_sampling_pending_)return fail("already_armed","One observation window per host; no duplicate scheduling");
                const auto& params=request.at("params");
                if(!params.is_object() || params.size()!=2 || !params.contains("scene_confirmed") || !params["scene_confirmed"].is_boolean()
                    || !params["scene_confirmed"].get<bool>() || !params.contains("scene_id") || !params["scene_id"].is_string())
                    return fail("scene_required","Explicit scene_confirmed=true and scene_id required; not a runtime settings claim");
                const auto scene=params["scene_id"].get<std::string>();
                if(scene.empty() || scene.size()>256)return fail("bad_scene","Scene description must be 1..256 UTF-8 bytes");
                LARGE_INTEGER qpc{},frequency{};QueryPerformanceCounter(&qpc);QueryPerformanceFrequency(&frequency);
                observer_sampling_pending_=json{{"request_id",id},{"client_id",client},{"session_id",session_},{"requested_revision",revision_+1},
                    {"scene_id",scene},{"operator_scene_confirmed",true},{"runtime_scene_verified",false},
                    {"received_cpu_qpc",qpc.QuadPart},{"qpc_frequency",frequency.QuadPart},{"received_tick_ms",now}};
                observer_sampling_={{"state","requested"},{"request",*observer_sampling_pending_},{"NR_control",false}};
            } else if (method == "ApplyConfig") {
                if (!synthetic_) return fail("unsupported", "Observer does not control the existing NR add-on");
                if (state_ == "running" || state_ == "paused" || state_ == "armed")
                    return fail("busy", "Cancel the current experiment before changing configuration");
                if (pending_) return fail("pending", "Previous configuration has not reached a frame boundary");
                const auto config = request.at("params").at("config");
                if (!config.is_object() || config.size() != 1 || !config.contains("nr_mode")
                    || !config["nr_mode"].is_string()) return fail("bad_config", "Only synthetic nr_mode is accepted");
                const auto mode = config["nr_mode"].get<std::string>();
                if (mode != "off" && mode != "on" && mode != "compute_bypass") return fail("bad_config", "Unknown NR mode");
                requested_ = config;
                pending_ = config;
            } else if (method == "Arm") {
                if (!synthetic_) return fail("unsupported", "Real-game runner awaits verified feature/resource hooks");
                if (state_ == "running" || state_ == "paused" || state_ == "armed" || pending_)
                    return fail("bad_state", "Arm requires idle/cancelled and applied configuration");
                if (!request.at("params").value("scene_confirmed", false))
                    return fail("scene_required", "Operator must confirm the scene");
                state_ = "armed";
            } else if (method == "Start") {
                if (state_ != "armed" && state_ != "paused") return fail("bad_state", "Start requires armed or paused");
                state_ = "running";
            } else if (method == "Pause") {
                if (state_ != "running") return fail("bad_state", "Pause requires running");
                state_ = "paused";
            } else if (method == "Cancel") {
                state_ = "cancelled";
                pending_.reset();
                owner_.clear();
            } else {
                return fail("unknown_method", "Unknown command; no action taken");
            }
            if (method == "StopCapture") owner_.clear();
            else if (method != "Cancel") { owner_ = client; last_seen_ = now; }
            ++revision_;
            return {{"protocol", "1.0"}, {"request_id", id}, {"ok", true}, {"status", status_locked()}};
        };
        auto response = dispatch();
        if (!read_only&&!embedded) responses_.emplace(cache_key, std::make_pair(request.dump(), response));
        return response;
    } catch (const json::exception&) {
        return fail("bad_request", "Missing or incorrectly typed request field");
    }
}
}
