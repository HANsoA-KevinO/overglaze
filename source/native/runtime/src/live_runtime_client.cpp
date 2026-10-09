// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The runtime half of the live bridge client, shared by the controller and
// research hosts. Identity gate, the 14 runtime exports, the NR mode/settings
// mailbox and the nr_runtime publication.
//
// Every string and every published key is the same for both hosts, so the
// research status stays byte-identical; the two collector fields come from
// virtual hooks that the base answers with "none".
#include "lab_package_identity.hpp"
#include "lab_live_runtime_client.hpp"
#include "lab_windows_path.hpp"
#include "lab_installation.hpp"
#include "lab_nr_mode.hpp"
#include "lab_game_exposure.hpp"
#include <cmath>
#include <filesystem>
namespace lab {
namespace {
std::wstring env(const wchar_t* name){std::array<wchar_t,32768> value{};auto size=GetEnvironmentVariableW(name,value.data(),static_cast<DWORD>(value.size()));
    if(!size||size>=value.size())throw std::runtime_error("Live runtime environment missing or too long");return {value.data(),size};}
const char* state_name(live::State s){switch(s){case live::State::waiting_frame:return "waiting-frame";case live::State::probing_queue:return "probing-queue";
    case live::State::preparing:return "preparing";case live::State::ready:return "ready";case live::State::failed:return "failed";
    case live::State::draining:return "draining";case live::State::waiting_rebuild_frame:return "waiting-rebuild-frame";default:return "stopped";}}
}

LiveRuntimeClient::LiveRuntimeClient(LiveStatusSink& sink,ID3D12CommandQueue* seed,HMODULE addon,
                                     LiveHostKind kind,const Installation* installation,unsigned required_variant,HMODULE proxy_host)
    :sink_(sink),kind_(kind),proxy_host_(proxy_host!=nullptr){
    if(installation){game_profile_=installation->profile;binding_preservation_=installation->facts.binding_preservation;
        late_attach_=installation->loader.late_host();}
    if(kind==LiveHostKind::native_fixture)game_profile_="synthetic-standalone-fixture";
    if(!installation&&(env(L"OVERGLAZE_OFFLINE_CONFIRMED")!=L"1"||env(L"OVERGLAZE_NO_ANTICHEAT_CONFIRMED")!=L"1"))throw std::runtime_error("Offline single-player scope required");
    // An Xbox app title runs through the OS package layout; compare the real
    // directory, exactly as the installation contract did at startup. Missing
    // this second place let the panel come up and then failed the host the
    // moment NR needed the bridge (A Plague Tale: Resonance).
    const auto exe=module_identity(nullptr);auto game=resolve_package_layout(std::filesystem::path(wide(exe.at("path").get<std::string>()))).parent_path();
    // Without an installation the caller has already admitted the executable
    // itself (the research host's own rule, the fixture identity below), so the
    // directory is the executable's own. Host contract V4: no machine paths here.
    const auto game_path=installation?installation->game_directory:game;
    if(kind==LiveHostKind::native_fixture){
        if(game.parent_path()!=fixture_data_root()||
            (std::filesystem::path(wide(exe.at("path").get<std::string>())).filename()!=L"lab_standalone_harness.exe"&&
             std::filesystem::path(wide(exe.at("path").get<std::string>())).filename()!=L"lab_controller_harness.exe")||
            exe.value("sha256","")!=utf8(env(L"OVERGLAZE_STANDALONE_FIXTURE_SHA256")))throw std::runtime_error("Unverified standalone fixture");
    }else winpath::require_same_file(game,game_path);
    if(GetModuleHandleW(L"renodx-dlss5.addon64")||GetModuleHandleW(L"overglaze_preview.addon64")||GetModuleHandleW(L"dlsslab_preview.addon64")||
       std::filesystem::exists(game/L"renodx-dlss5.addon64")||std::filesystem::exists(game/L"overglaze_preview.addon64")||std::filesystem::exists(game/L"dlsslab_preview.addon64"))
        throw std::runtime_error("Legacy NR and preview must be disabled before game launch");
    // An overlay layer is verified by the add-on that runs under it; a native
    // host must not find one in its process.
    if(!proxy_host&&GetProcAddress(GetModuleHandleW(L"dxgi.dll"),"ReShadeRegisterAddon"))throw std::runtime_error("Standalone host cannot run with ReShade");
    const auto path=std::filesystem::path(wide(module_identity(addon).at("path").get<std::string>())).parent_path()/L"overglaze_nvngx.dll";
    const auto expected=installation?installation->bridge_sha256:utf8(env(L"OVERGLAZE_LIVE_BRIDGE_SHA256"));if(expected.size()!=64||sha256(path)!=expected)throw std::runtime_error("Live bridge hash differs from deployment manifest");
    Handle lock(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));check(lock.valid(),"Lock live bridge");
    module_=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);check(module_!=nullptr,"Load live bridge");
    bridge_=module_identity(module_);if(bridge_.value("sha256","")!=expected)throw std::runtime_error("Loaded live bridge identity mismatch");
    HMODULE pin{};check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(module_),&pin)!=0,"Pin live bridge");
    poll_=live::proc<live::Poll>(module_,"LabNrLivePoll");request_=live::proc<live::Request>(module_,"LabNrLiveRequest");enter_=live::proc<live::Enter>(module_,"LabNrLiveEnter");
    poll_bindings_=reinterpret_cast<live::PollBindings>(GetProcAddress(module_,"LabNrLivePollBindings"));
    configure_=live::proc<live::Configure>(module_,"LabNrLiveConfigure");
    apply_=live::proc<live::Apply>(module_,"LabNrLiveApply");
    // Which variant did we load? A bridge without the export reports variant 0.
    if(auto capabilities=reinterpret_cast<live::GetCapabilities>(GetProcAddress(module_,"LabNrLiveCapabilities")))
        if(!capabilities(&bridge_capabilities_))throw std::runtime_error("Live bridge rejected the capability query");
    if(required_variant&&bridge_capabilities_.variant!=required_variant)
        throw std::runtime_error("Live bridge variant "+std::to_string(bridge_capabilities_.variant)+
            " is not the required variant "+std::to_string(required_variant));
    returned_=live::proc<live::BoundaryReturned>(module_,"LabNrLiveBoundaryReturned");stop_=live::proc<live::Stop>(module_,"LabNrLiveStop");
    reject_=live::proc<live::Reject>(module_,"LabNrLiveReject");
    render_queue_=reinterpret_cast<live::RenderQueue>(GetProcAddress(module_,"LabNrLiveRenderQueue"));
    const auto data=installation?installation->run:std::filesystem::path(env(L"OVERGLAZE_LIVE_DATA_PATH"));
    // V4: the run directory sits directly in the data root -- the one the
    // installation recorded and startup validated, or a fixture's own.
    if(!data.is_absolute()||data.lexically_normal()!=data||!std::filesystem::is_directory(data))throw std::runtime_error("Live data path must be an existing immediate lab run directory");
    const auto allowed=installation?installation->output_root:kind==LiveHostKind::native_fixture?fixture_data_root():data.parent_path();
    if(data.parent_path()!=allowed)throw std::runtime_error("Live data path must be an existing immediate lab run directory");
    // The model sits beside the loader, which for every root-strategy install is
    // the game directory itself. A late-loaded or plugin-hosted controller keeps
    // its whole payload in one subdirectory instead of scattering the model into
    // the game's root. The bridge re-checks the model's identity either way.
    const auto loader_dir=installation?installation->loader_directory:game_path;
    // A fixture reads the user's model in place from the program's models folder,
    // found by the same layout rule as every out-of-game program:
    // <root>\data\<run> and <root>\app\models. No copy, no compiled path.
    const auto model_path=kind==LiveHostKind::native_fixture?allowed.parent_path()/L"app"/L"models"/L"nvngx_dlssnr.dll":loader_dir/L"nvngx_dlssnr.dll";
    if(!live::proc<live::Start>(module_,"LabNrLiveStart")(seed,proxy_host,model_path.c_str(),data.c_str(),&context_))throw std::runtime_error("Live backend start failed");
    std::array<wchar_t,8> preserve_fixture{};
    const bool fixture_preservation=kind==LiveHostKind::native_fixture&&
        GetEnvironmentVariableW(L"OVERGLAZE_FIXTURE_BINDING_PRESERVATION",preserve_fixture.data(),8)==1&&preserve_fixture[0]==L'1';
    binding_preservation_active_=binding_preservation_||fixture_preservation;
    if(binding_preservation_active_ &&
       !live::proc<live::EnableBindingPreservation>(module_,"LabNrLiveEnableBindingPreservation")(context_))
        throw std::runtime_error("Native binding preservation required");
    if(binding_preservation_active_){
        game_entry_=live::proc<live::GameBindingEntry>(module_,"LabNrLiveGameBindingEntryV1");
        game_exit_=live::proc<live::GameBindingExit>(module_,"LabNrLiveGameBindingExitV1");
        // A late-loaded game created its command signatures before we arrived, so
        // their layouts are unknowable; tolerate them there (counted, published)
        // and nowhere else. A fixture may ask for the same policy explicitly.
        std::array<wchar_t,8> tolerate_fixture{};
        const bool fixture_tolerance=kind==LiveHostKind::native_fixture&&
            GetEnvironmentVariableW(L"OVERGLAZE_FIXTURE_UNKNOWN_INDIRECT",tolerate_fixture.data(),8)==1&&tolerate_fixture[0]==L'1';
        if(late_attach_||fixture_tolerance){
            const auto policy=reinterpret_cast<live::BindingPolicy>(GetProcAddress(module_,"LabNrLiveBindingPolicyV1"));
            unknown_indirect_policy_=policy&&policy(context_,1u)?"assume-untouched (late attach)":"strict (bridge has no policy export)";
        }
    }
}

// What this host has actually seen work, folded forward across the process.
//
// Every flag is evidence, not intent: can_set_tone means the NR DLL returned our
// requested tone during an Evaluate on this machine, not that the parameter is
// documented. The ladder L0..L3 is what the capability handshake reads,
// so a controller that has only loaded reports L1 and the panel can say so
// instead of pretending the knobs are live.
//   L0  nothing established yet
//   L1  the live bridge started and reported a state (can_load)
//   L2  NR has executed at least once (can_toggle)
//   L3  the DLL read back Tone, Structure and Style during Evaluate
// None of this says the output is correct, matches the official runtime, or that
// the game's displayed frame was verified. Those stay false elsewhere.
json LiveRuntimeClient::capability_report(){
    discovered_.load=true; // we only get here with the bridge loaded and polled
    if(status_.observed_on!=0){discovered_.toggle=true;discovered_.exposure=true;}
    if(status_.settings_read_mask&1)discovered_.tone=true;
    if(status_.settings_read_mask&2)discovered_.structure=true;
    if(status_.settings_style_read)discovered_.style=true;
    if(status_.retired||status_.waits)discovered_.gpu=true;
    const bool model=discovered_.tone&&discovered_.structure&&discovered_.style;
    return {{"can_load",discovered_.load},{"can_toggle",discovered_.toggle},
        {"can_set_tone",discovered_.tone},{"can_set_structure",discovered_.structure},{"can_set_style",discovered_.style},
        {"can_set_exposure",discovered_.exposure},
        {"can_capture",research_available()},
        {"can_observe_gpu",discovered_.gpu},
        {"evidence_level",model?"L3":discovered_.toggle?"L2":"L1"},
        {"scope","observed in this process and sticky: what the NR DLL returned during Evaluate, not a correctness or official-equivalence claim. can_set_exposure is ours: the Lab colour preparation applies 2^stops, the DLL never reads it. can_capture is live, not sticky."}};
}

void LiveRuntimeClient::poll(const char* blocked){
    const auto prior_rebuild=status_.rebuild_serial;
    live::Status fresh;if(poll_(context_,&fresh))status_=fresh;
    sample_research();
    if(poll_bindings_){live::BindingStatus sample;if(poll_bindings_(context_,&sample))binding_status_=sample;}
    publish_research_diagnostics();
    if(status_.rebuild_serial>prior_rebuild){
        sink_.interrupt_nr_for_rebuild(status_.rebuild_serial,status_.rebuild_frame);
        action_.reset();dispatched_=false;
    }
    if(!stopped()&&status_.state==live::State::ready&&!enabled_){sink_.enable_nr_frame_control(kind_==LiveHostKind::native_fixture?"synthetic-input-real-nr":"game-rr-experimental-nr",true,research_available());enabled_=true;}
    if(enabled_&&!stopped()&&status_.state==live::State::ready){
        if(!action_)action_=sink_.take_nr_mode_request(GetTickCount64());
        if(action_&&!dispatched_){
            if(action_->contains("settings")){const auto& v=action_->at("settings");nr::Settings settings{v.at("tone").get<float>(),v.at("structure").get<float>(),v.at("style").get<unsigned>(),v.value("exposure_stops",0.f),v.value("exposure_auto",0u),v.value("compare_split",0u),
                    v.value("skin",1.f),v.value("automask",0u)};
                dispatched_=apply_(context_,action_->at("revision").get<std::uint64_t>(),nr::mode_value(action_->at("mode").get<std::string>()),&settings);
            }else dispatched_=request_(context_,action_->at("revision").get<std::uint64_t>(),nr::mode_value(action_->at("mode").get<std::string>()));
        }
    }
    dispatch_research_captures();
    const auto capabilities=capability_report();
    // ABI24: where auto exposure took its gain from, the game's raw values and
    // why the meter was used, by name.
    json fallbacks=json::object();
    for(unsigned i=1;i<live::exposure_note_count;++i)if(status_.exposure_notes[i])fallbacks[live::exposure_note_name(i)]=status_.exposure_notes[i];
    const bool game_read=status_.game_exposure_valid!=0;
    const bool game_stops_valid=game_read&&std::isfinite(status_.game_exposure)&&status_.game_exposure>0.f&&
        status_.game_pre_exposure>0.f&&status_.game_exposure_scale>0.f;
    const json game_exposure{{"texture_value",game_read?json(status_.game_exposure):json(nullptr)},
        {"pre_exposure",game_read?json(status_.game_pre_exposure):json(nullptr)},{"exposure_scale",game_read?json(status_.game_exposure_scale):json(nullptr)},
        {"stops",game_stops_valid?json(std::log2(status_.game_exposure*status_.game_exposure_scale/status_.game_pre_exposure)):json(nullptr)},
        {"exposed_log2_luminance",status_.game_exposed_valid?json(status_.game_exposed_log2_luminance):json(nullptr)},
        {"trusted_log2_window",json::array({nr::GameExposureSelector::min_exposed_log2,nr::GameExposureSelector::max_exposed_log2})}};
    sink_.publish_nr_runtime({{"capabilities",capabilities},
        {"profile",game_profile_},{"state",state_name(status_.state)},
        {"host_backend",proxy_host_?"reshade":"standalone-d3d12"},
        {"input_origin",kind_==LiveHostKind::native_fixture?"synthetic":"game"},
        {"error",status_.error},{"binding_blocker",blocked?json(blocked):json(nullptr)},{"bridge",bridge_},
        {"rejected_call",status_.rejection.reason[0]?json{{"call",status_.rejection.call},{"frame",status_.rejection.frame},{"stage",status_.rejection.stage},{"reason",status_.rejection.reason},
            {"disposition",status_.rejection.disposition==RejectedDisposition::constants_missing_before_nr?"bypass-and-rebuild-off":
                status_.rejection.disposition==RejectedDisposition::skipped_before_insertion?"skipped-before-insertion-history-reset":
                status_.rejection.disposition==RejectedDisposition::recording_discarded_by_game_reset?"recording-discarded-by-game-reset-history-reset":"terminal"}}:json(nullptr)},
        {"frames",status_.frames},{"ack_revision",status_.ack_revision},{"ack_frame",status_.ack_frame},
        {"feature_generation",status_.feature_generation},{"feature_releases",status_.feature_releases},{"rebuild_serial",status_.rebuild_serial},{"rebuild_frame",status_.rebuild_frame},
        {"bypass_frame",status_.bypass_frame},{"rebuild_reason",status_.rebuild_reason},{"automatic_on_after_rebuild",false},
        {"skipped_frames",status_.skipped_frames},{"consecutive_skips",status_.consecutive_skips},{"skip_budget",status_.skip_budget},{"history_gaps",status_.history_gaps},
        {"discarded_recordings",status_.discarded_recordings},{"submission_skips",status_.submission_skips},
        {"signal_retries",status_.signal_retries},
        {"discard_scope","recorded frames whose command list the game Reset before any submission (native Reset hook proof): nothing executed, lease released, NR history reset, ON kept; submission_skips are admitted RR frames skipped while the previous recording was still unsubmitted"},
        {"skip_scope","successful RR calls skipped while ON after a transient input-verification miss; nothing recorded on that call; NR history reset on the next admitted frame"},
        {"evaluates",status_.evaluates},{"off_frames",status_.off_frames},{"retired",status_.retired},
        {"execution_mode",nr::mode_name(status_.observed_on)},
        {"game_color_writeback",status_.observed_on==1},{"compute_only_has_overhead",true},
        {"pending_gpu",status_.pending_gpu!=0},{"cpu_wait_count",status_.waits},{"cpu_wait_ms",status_.wait_ms},
        {"binding_preservation",{{"enabled",status_.binding_preservation_enabled!=0},{"ready",status_.binding_preservation_ready!=0},
            {"unknown_indirect_policy",unknown_indirect_policy_},{"unknown_indirect_calls",poll_bindings_?binding_status_.unknown_indirect_calls:0ULL},
            {"restores",status_.binding_restores},{"waiting_frames",status_.binding_wait_frames},
            {"observation",poll_bindings_?json{{"enabled",binding_status_.enabled!=0},{"ready",binding_status_.ready!=0},
                {"frame",binding_status_.frame},{"call",binding_status_.call},{"consecutive_ready",binding_status_.consecutive_ready},
                {"blocked_frames",binding_status_.blocked_frames},{"reason",binding_status_.reason},{"reason_first_frame",binding_status_.reason_first_frame},
                {"heap_order_permutations",binding_status_.heap_order_permutations},{"heap_permuted_insertions",binding_status_.heap_permuted_insertions}}:json(nullptr)}}},
        {"settings",{{"requested",{{"tone",status_.requested_settings.tone},{"structure",status_.requested_settings.structure},{"style",status_.requested_settings.style},{"exposure_stops",status_.requested_settings.exposure_stops},{"exposure_auto",status_.requested_settings.exposure_auto},{"compare_split",status_.requested_settings.compare_split},
                {"skin",status_.requested_settings.skin},{"automask",status_.requested_settings.automask}}},
            {"observed",status_.settings_read_mask==3&&status_.settings_style_read?json{{"tone",status_.observed_settings.tone},{"structure",status_.observed_settings.structure},{"style",status_.observed_settings.style},{"exposure_stops",status_.observed_settings.exposure_stops},{"exposure_auto",status_.observed_settings.exposure_auto},{"compare_split",status_.observed_settings.compare_split},
                {"skin",status_.observed_settings.skin},{"automask",status_.observed_settings.automask}}:json(nullptr)},
            {"skin_read",status_.settings_skin_read==1},{"automask_read",status_.settings_automask_read==1},
            {"applied_exposure_stops",status_.applied_exposure_stops},{"metered_log2_luminance",status_.metered_log2_luminance},{"meter_samples",status_.meter_samples},
            {"exposure_source",live::exposure_source_name(status_.exposure_source)},
            {"exposure_fallback",status_.exposure_source==static_cast<unsigned>(live::ExposureSource::meter)?json(live::exposure_note_name(status_.exposure_note)):json(nullptr)},
            {"game_exposure",game_exposure},
            {"exposure_frames",{{"game",status_.game_exposure_frames},{"meter",status_.meter_exposure_frames},{"source_switches",status_.exposure_source_switches},{"meter_by_reason",fallbacks}}},
            {"auto_exposure_scope","auto: the game's own exposure (log2 of texture value x exposure scale / pre-exposure, read on the GPU with one frame of latency) when the game passes one whose game-exposed log-average lies in the trusted window, with hysteresis; otherwise the host GPU meter (mean log2 luminance of the working RGB -> mid-grey 0.18 gain, smoothed); the user offset on top either way"},
            {"exposure_scope","exposure_stops is applied by the Lab colour preparation (2^stops on working RGB, divided out after composite); the NR DLL does not read it"},
            {"style_read",status_.settings_style_read==1},{"history_reset_requested",status_.settings_reset_requested==1},
            {"requested_revision",status_.settings_requested_revision},{"observed_revision",status_.settings_observed_revision},
            {"frame",status_.settings_frame},{"read_mask",status_.settings_read_mask},
            {"scope","values-returned-to-NR-during-Evaluate-not-internal-network-location"}}},
        {"width",status_.width},{"height",status_.height},{"guide_width",status_.guide_width},{"guide_height",status_.guide_height},{"scratch_bytes",status_.scratch_bytes},
        // ABI23: whether colour/depth are cropped to their regions, and the motion region resampled onto the guide grid.
        {"region_crop",status_.region_crop!=0},{"motion_region",status_.motion_width?json::array({status_.motion_width,status_.motion_height}):json(nullptr)},
        {"color_profile","RR working RGB / identity matrices / exposure 2^exposure_stops (host setting) / pre/post-ratio operator 3 / lab invalid-RGB guard"},
        {"insertion_policy","after-successful-evaluate-before-host-rebind"},
        {"acknowledgement_scope","recorded-at-matched-API-return-not-display-or-state-restoration"},
        {"bridge_variant",bridge_capabilities_.variant==2?"research":bridge_capabilities_.variant==1?"controller":"unknown"},
        {"research_collectors_available",research_available()},
        {"official_runtime_equivalence",false},{"displayed_frame_verified",false},{"p0_gate_open",false},{"raw_texture_files",research_raw_texture_files()},{"event_files",0}});
    // Never acknowledge a new control revision against an older runtime snapshot.
    if(action_&&dispatched_&&status_.ack_revision==action_->at("revision")&&status_.ack_frame){
        sink_.acknowledge_nr_mode(status_.ack_revision,status_.ack_frame,status_.ack_success!=0,status_.ack_evaluated!=0,status_.ack_selected!=0,status_.error);
        action_.reset();dispatched_=false;
    }
}

void LiveRuntimeClient::frame(const live::Frame& f) noexcept {if(!stopped())enter_(context_,&f);}
void LiveRuntimeClient::boundary_returned(std::uint64_t call,bool success) noexcept {if(!stopped())returned_(context_,call,success?1u:0u);}
void LiveRuntimeClient::aborted(std::uint64_t call) noexcept {boundary_returned(call,false);}
void LiveRuntimeClient::rejected(const RejectedCall& why) noexcept {if(!stopped())reject_(context_,&why);}
void LiveRuntimeClient::game_binding_enter(std::uint64_t call,std::uint64_t frame_index,void* command) noexcept {
    if(!stopped()&&game_entry_)game_entry_(context_,call,frame_index,static_cast<ID3D12GraphicsCommandList*>(command));
}
void LiveRuntimeClient::game_binding_exit(std::uint64_t call) noexcept {if(game_exit_)game_exit_(context_,call);}

Microsoft::WRL::ComPtr<ID3D12CommandQueue> LiveRuntimeClient::render_queue() const noexcept {
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if(stopped()||!render_queue_||!context_)return queue;
    ID3D12CommandQueue* raw=nullptr;
    if(render_queue_(context_,&raw)&&raw)queue.Attach(raw); // the export hands over one reference
    return queue;
}

void LiveRuntimeClient::stop() noexcept {
    stopped_=true;
    // Cleanup also runs while handling a rejected status packet. A second
    // publication exception must not escape the host's worker and kill a game.
    // Failure to publish does NOT certify GPU drain or successful release.
    try{stop_(context_);poll(nullptr);}
    catch(...){try{sink_.diagnostic("NR stop requested; final runtime status publication failed (completion not certified)");}catch(...){}}
}
}
