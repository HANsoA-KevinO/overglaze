// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_completion_timeline.hpp"
#include <thread>
#include <utility>
#include "lab_serial_access.hpp"

namespace lab::nr {
// Single-access-owner, single-pending coordinator, shared by the real NR pipeline and
// its test double. Timeline outlives this object. The host owns command lists,
// allocators and all borrowed resources until poll() succeeds; no implicit wait.
// This does NOT discover queue submissions or prove hidden NR/CUDA completion.
struct NoBorrowedLease {void abandon() noexcept {} bool held() const noexcept{return false;}};
template<class Pipeline,class Lease=NoBorrowedLease> class FrameRetirement final {
    Pipeline& pipeline_;
    CompletionTimeline& timeline_;
    CompletionTimeline::Ticket ticket_{};
    ThreadAccess access_;
    Lease borrowed_{};
    enum class Stage { idle, recording, bound, submitted, retired_pipeline, failed } stage_=Stage::idle;
    void owner() const {access_.require();}
    template<class Result>static bool recorded_work(const Result& result){
        if constexpr(requires {result.gpu_recorded;})return result.gpu_recorded;
        else return result.nr_recorded;
    }
public:
    FrameRetirement(Pipeline& p,CompletionTimeline& t,SerialCallGate* gate=nullptr):pipeline_(p),timeline_(t),access_(gate){}
    FrameRetirement(const FrameRetirement&)=delete;
    FrameRetirement& operator=(const FrameRetirement&)=delete;
    ~FrameRetirement(){if(stage_!=Stage::idle)borrowed_.abandon();}
    template<class Record> auto record_owned(std::uint64_t frame,Lease lease,Record&& invoke){
        owner();if(stage_!=Stage::idle||!pipeline_.ready()||!lease.held())throw std::logic_error("Borrowed entry lease not ready");
        borrowed_=std::move(lease);
        try{return record(true,frame,std::forward<Record>(invoke));}
        catch(...){if(stage_==Stage::idle)borrowed_={};else borrowed_.abandon();throw;}
    }
    // Whole-frame validation belongs BEFORE this call. Once recording begins,
    // a foreign failure may already have emitted GPU work and cannot roll back.
    template<class Record> auto record(bool on,std::uint64_t frame,Record&& invoke) {
        owner();if(stage_!=Stage::idle || !pipeline_.ready())throw std::logic_error("NR slot still pending or failed");
        if(!on){ // Strict OFF: no ticket/fence, and fail closed on a bad recorder.
            try {auto result=std::forward<Record>(invoke)();
                if(recorded_work(result) || !pipeline_.ready())throw std::logic_error("OFF recorder performed GPU work");
                return result;
            }catch(...){stage_=Stage::failed;throw;}
        }
        ticket_=timeline_.reserve(frame);
        if(!ticket_.generation)throw std::logic_error("No completion ticket available");
        if(!timeline_.recording(ticket_)) {
            if(!timeline_.cancel_before_recording(ticket_))stage_=Stage::failed;
            throw std::logic_error("Cannot enter completion recording");
        }
        stage_=Stage::recording;
        try {
            auto result=std::forward<Record>(invoke)();
            if(!recorded_work(result) || !result.resource)throw std::runtime_error("GPU recorder did not return a recorded result");
            pipeline_.bind_completion(timeline_.binding_fence(),ticket_.value);
            stage_=Stage::bound;return result;
        }catch(...){stage_=Stage::failed;throw;}
    }
    // Call AFTER actual submission of the LAST consumer, on this exact queue.
    // A wrong/repeated notification never queues an extra Signal.
    bool last_use_submitted(std::uint64_t frame,ID3D12CommandQueue* queue) {
        owner();if(stage_!=Stage::bound || ticket_.frame!=frame)return false;
        if(!timeline_.submitted(ticket_,queue))return false;
        stage_=Stage::submitted;return true;
    }
    bool poll() {
        owner();if(stage_==Stage::idle)return true;
        // Native submission callback can signal the Timeline on another thread.
        // Only this owner polls/acknowledges the NR Session; no cross-thread call
        // into Pipeline and no assumption that a mere API return is completion.
        if(stage_==Stage::bound){
            const auto seen=timeline_.inspect(ticket_);
            if(seen==CompletionTimeline::State::submitted || seen==CompletionTimeline::State::complete)stage_=Stage::submitted;
            else if(seen==CompletionTimeline::State::failed || seen==CompletionTimeline::State::invalid){stage_=Stage::failed;return false;}
            else return false;
        }
        if(stage_!=Stage::submitted && stage_!=Stage::retired_pipeline)return false;
        const auto state=timeline_.inspect(ticket_);
        if(state==CompletionTimeline::State::failed || state==CompletionTimeline::State::invalid){stage_=Stage::failed;return false;}
        if(state!=CompletionTimeline::State::complete)return false;
        try {
            if(stage_==Stage::submitted){
                if(!pipeline_.retire_if_complete())throw std::logic_error("Completion/pipeline disagreement");
                stage_=Stage::retired_pipeline;
            }
            // If the try-lock is busy, retry only ticket retirement; NEVER
            // acknowledge the Session twice or record another frame meanwhile.
            if(!timeline_.retire(ticket_))return false;
            stage_=Stage::idle;ticket_={};borrowed_={};return true;
        }catch(...){stage_=Stage::failed;throw;}
    }
    // The submission observer proved the list holding this recording was Reset
    // before any execution (timeline state discarded). Nothing of it will run:
    // retire the ticket without a fence, drop the pipeline's pending completion
    // and RELEASE (not abandon) the borrowed lease. A try-lock miss or a ticket
    // that is not discarded returns false with nothing changed.
    bool discard_unsubmitted() {
        owner();if(stage_!=Stage::bound)return false;
        if(timeline_.inspect(ticket_)!=CompletionTimeline::State::discarded)return false;
        if(!timeline_.retire_discarded(ticket_))return false;
        try {pipeline_.discard_recorded();}catch(...){stage_=Stage::failed;throw;}
        stage_=Stage::idle;ticket_={};borrowed_={};return true;
    }
    bool ready() const {owner();return stage_==Stage::idle && pipeline_.ready();}
    bool failed() const {owner();return stage_==Stage::failed;}
    bool borrowed_held() const {owner();return borrowed_.held();}
    CompletionTimeline::Ticket ticket() const {owner();return ticket_;}
};
}
