// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_nr_frame.hpp"
#include "lab_prepost_ratio_codec.hpp"
#include "lab_color_handoff.hpp"

namespace lab::nr {
struct PipelineCaptureHooks {
    void* context=nullptr;
    void (*before_nr)(void*,const Frame&)=nullptr;
    void (*after_nr)(void*,const Frame&)=nullptr;
    // Runs after the original working RGB is snapshotted (NPSR), before the
    // colour preparation. Host-owned read-only work such as exposure metering.
    void (*after_snapshot)(void*,ID3D12GraphicsCommandList*,ID3D12Resource* original)=nullptr;
};
struct PreparationSelection {
    ID3D12Resource* resource=nullptr;
    bool gpu_recorded=false,nr_recorded=false;
};
// Composed product path, not a game adapter or the official SL bridge. The host
// supplies a linear target, valid guides, exclusive ordered access and a private
// GPU fence. All six scratch textures stay owned by the codec/handoff.
class PrePostFramePipeline final {
    FrameRunner runner_;
    PrePostColorCodec codec_;
    ColorHandoff handoff_;
    PrePostColorInputs inputs_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    std::uint64_t future_=0;
    bool runner_recorded_=false;
    ThreadAccess access_;
    DeviceIdentity device_identity_;
    enum class Stage { idle, recording, recorded, pending, failed } stage_=Stage::idle;
    void owner() const {access_.require();}
    static Microsoft::WRL::ComPtr<IUnknown> identity(IUnknown* p) {
        Microsoft::WRL::ComPtr<IUnknown> id;
        if(!p || FAILED(p->QueryInterface(IID_PPV_ARGS(&id))))throw std::logic_error("Pipeline COM identity missing");
        return id;
    }
public:
    PrePostFramePipeline(Session& session,NVSDK_NGX_Parameter& parameters,ID3D12Device* device,
                     unsigned frame_width,unsigned frame_height,const PrePostColorInputs& inputs,DeviceIdentity policy={},
                     bool target_region_crop=false)
        :runner_(session,parameters,device,frame_width,frame_height,policy),codec_(device,inputs,session.access_gate(),policy),
         handoff_(device,{inputs.original,inputs.composite,inputs.states_and_distinct_allocations_established,target_region_crop},session.access_gate(),policy),inputs_(inputs),device_(device),access_(session.access_gate()),device_identity_(policy) {
        if(inputs.original->GetDesc().Width!=frame_width || inputs.original->GetDesc().Height!=frame_height)
            throw std::logic_error("Pipeline dimensions disagree with scratch");
    }
    ~PrePostFramePipeline(){if(stage_!=Stage::idle){(void)fence_.Detach();(void)device_.Detach();}}
    PrePostFramePipeline(const PrePostFramePipeline&)=delete;
    PrePostFramePipeline& operator=(const PrePostFramePipeline&)=delete;
    // Pure whole-pipeline preflight, also used before adapter guide transitions.
    void validate_on_frame(const Frame& source,D3D12_RESOURCE_STATES target_state,
                           bool target_contract,const PrePostColorConstants& constants) const {
        owner();if(stage_!=Stage::idle)throw std::logic_error("Pipeline pending or failed");
        if(source.color_domain!=ColorDomain::linear_working_rgb)throw std::logic_error("Explicit linear working-RGB source required");
        Frame nr=source;nr.color=inputs_.prepared;nr.output=inputs_.neural;
        nr.color_domain=ColorDomain::prepared_sample_input;
        // No mutations until every component and cross-component alias check
        // succeeds. No hidden color-space inference from a format or flag.
        runner_.validate_on_frame(nr);codec_.validate_prepare(source.commands,constants);
        handoff_.validate_snapshot(source.commands,source.color,target_state,source.token,target_contract);
        const auto target=identity(source.color),depth=identity(source.depth),motion=identity(source.motion);
        for(auto* r:{inputs_.original,inputs_.prepared,inputs_.neural,inputs_.composite,inputs_.working_hdr,inputs_.sdr_linear}) {
            const auto id=identity(r);if(id==target || id==depth || id==motion)throw std::logic_error("Cross-stage pipeline aliases refused");
        }
        if(target==depth || target==motion)throw std::logic_error("Target aliases guide");
    }
    // OFF performs no GPU/resource work; an unknown bypass target may be null.
    Selection record(Mode mode,const Frame& source,D3D12_RESOURCE_STATES target_state,
                     bool target_contract,const PrePostColorConstants& constants,PipelineCaptureHooks capture={}) {
        owner();if(stage_!=Stage::idle)throw std::logic_error("Pipeline pending or failed");
        if(mode==Mode::off)return runner_.record(mode,source);
        if(mode!=Mode::on && mode!=Mode::compute_only)throw std::logic_error("Unknown pipeline mode");
        validate_on_frame(source,target_state,target_contract,constants);
        Frame nr=source;nr.color=inputs_.prepared;nr.output=inputs_.neural;nr.color_domain=ColorDomain::prepared_sample_input;
        stage_=Stage::recording;
        try {
            handoff_.snapshot(source.commands,source.color,target_state,source.token,target_contract);
            if(capture.after_snapshot)capture.after_snapshot(capture.context,source.commands,inputs_.original);
            codec_.prepare(source.commands,constants);
            if(capture.before_nr)capture.before_nr(capture.context,nr);
            auto result=runner_.record(Mode::on,nr);
            if(result.ngx_result!=NVSDK_NGX_Result_Success || !result.resource) {
                stage_=Stage::failed;result.resource=nullptr;return result;
            }
            runner_recorded_=true;
            if(capture.after_nr)capture.after_nr(capture.context,nr);
            codec_.composite(source.commands);
            if(mode==Mode::on)handoff_.return_output(source.commands);
            else handoff_.preserve_target(source.commands);
            stage_=Stage::recorded;result.resource=source.color;return result;
        }catch(...){stage_=Stage::failed;throw;}
    }
    // Not a user-facing NR mode. Uses the same snapshot/prepare implementation
    // and validation as ON, but NEVER calls FrameRunner, composite or copy-back.
    PreparationSelection record_input_preparation(const Frame& source,D3D12_RESOURCE_STATES target_state,
                                                  bool contract,const PrePostColorConstants& constants) {
        validate_on_frame(source,target_state,contract,constants);stage_=Stage::recording;runner_recorded_=false;
        try {
            handoff_.snapshot(source.commands,source.color,target_state,source.token,contract);
            codec_.prepare(source.commands,constants);codec_.finish_prepare_only(source.commands);
            handoff_.preserve_target(source.commands);stage_=Stage::recorded;
            return {source.color,true,false};
        }catch(...){stage_=Stage::failed;throw;}
    }
    void bind_completion(ID3D12Fence* fence,std::uint64_t future) {
        owner();if(stage_!=Stage::recorded || !fence || !future || future==UINT64_MAX || fence->GetCompletedValue()>=future)
            throw std::logic_error("Pipeline requires future completion before submission");
        Microsoft::WRL::ComPtr<ID3D12Device> d;
        if(FAILED(fence->GetDevice(IID_PPV_ARGS(&d))) || !device_identity_.same(d.Get(),device_.Get()))throw std::logic_error("Pipeline fence device mismatch");
        try {codec_.bind_completion(fence,future);handoff_.bind_completion(fence,future);
            fence_=fence;future_=future;stage_=Stage::pending;
        }catch(...){stage_=Stage::failed;throw;}
    }
    bool retire_if_complete() {
        owner();if(stage_!=Stage::pending)throw std::logic_error("No pipeline completion pending");
        const auto done=fence_->GetCompletedValue();
        if(done==UINT64_MAX){stage_=Stage::failed;throw std::runtime_error("Pipeline device removed");}
        if(done<future_)return false;
        try {
            if(!codec_.retire_if_complete() || !handoff_.retire_if_complete())throw std::logic_error("Pipeline completion disagreement");
            if(runner_recorded_)runner_.acknowledge_outer_queue_completion();
            runner_recorded_=false;fence_.Reset();stage_=Stage::idle;return true;
        }catch(...){stage_=Stage::failed;throw;}
    }
    // The command list holding this frame's recording was Reset by its owner
    // before any submission (proven by the submission observer). None of it
    // will execute: no scratch was written, the target was not touched, the
    // Feature saw no Evaluate. Drop the pending completion without claiming
    // one. Never valid for submitted work.
    void discard_recorded() {
        owner();if(stage_!=Stage::pending)throw std::logic_error("No pending pipeline recording to discard");
        try {codec_.discard_recorded();handoff_.discard_recorded();
            if(runner_recorded_)runner_.discard_recorded();
            runner_recorded_=false;fence_.Reset();future_=0;stage_=Stage::idle;
        }catch(...){stage_=Stage::failed;throw;}
    }
    void set_compare_split(bool v){owner();codec_.set_compare_split(v);}
    void set_extrapolation(float factor){owner();codec_.set_extrapolation(factor);}
    float extrapolation() const {owner();return codec_.extrapolation();}
    bool ready() const {owner();return stage_==Stage::idle;}
    std::uint64_t off_frames() const {owner();return runner_.off_frames();}
    std::uint64_t on_frames() const {owner();return runner_.on_frames();}
    std::uint64_t pre_count() const {owner();return codec_.prepare_count();}
    std::uint64_t post_count() const {owner();return codec_.composite_count();}
    std::uint64_t snapshot_count() const {owner();return handoff_.snapshot_count();}
    std::uint64_t return_count() const {owner();return handoff_.return_count();}
};
}
