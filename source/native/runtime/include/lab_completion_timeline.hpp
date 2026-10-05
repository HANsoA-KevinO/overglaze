// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <mutex>
#include <cstdint>
#include <stdexcept>
#include <atomic>

namespace lab {
// Owns one queue timeline. Does NOT hook/submit lists, wait, release resources,
// or prove NR internal work. The host calls submitted() only after the last
// use of its scratch/borrowed resources has actually been submitted on queue_.
class CompletionTimeline final {
public:
    enum class State { invalid, reserved, recorded, submitted, complete, busy, failed, discarded };
    // Verdict of discard_before_submission: busy is a try-lock miss to retry, never proof.
    enum class Discard { discarded, busy, refused };
    struct Ticket {unsigned slot=UINT32_MAX;std::uint64_t generation=0,value=0,frame=0;};
    struct Stats {unsigned reserved=0,recorded=0,submitted=0;std::uint64_t signals=0,retired=0,discarded=0;bool failed=false;};
private:
    struct Entry {Ticket ticket;State state=State::invalid;};
    std::array<Entry,8> entries_{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    mutable std::mutex mutex_;
    std::uint64_t generation_=0,next_=0,last_frame_=0,signals_=0,retired_=0,discarded_=0;
    std::atomic<bool> failed_{false};
    Entry* find(Ticket t) noexcept {
        if(t.slot>=entries_.size())return nullptr;auto& e=entries_[t.slot];
        return e.state!=State::invalid && e.ticket.generation==t.generation && e.ticket.value==t.value && e.ticket.frame==t.frame?&e:nullptr;
    }
    State state(Entry& e) noexcept {
        if(failed_)return State::failed;
        const auto done=fence_->GetCompletedValue();
        if(done==UINT64_MAX){failed_=true;return State::failed;}
        // A future/externally advanced fence is NOT proof of submission.
        if(e.state==State::submitted && done>=e.ticket.value)return State::complete;
        return e.state;
    }
public:
    explicit CompletionTimeline(ID3D12CommandQueue* queue):queue_(queue) {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if(!queue || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
           FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_))))throw std::runtime_error("Private completion timeline creation failed");
    }
    ~CompletionTimeline(){ // Only destroy after callers quiesce; retain uncertain GPU dependencies.
        for(auto& e:entries_)if(e.state==State::recorded || e.state==State::submitted){
            if(state(e)!=State::complete){(void)queue_.Detach();(void)fence_.Detach();break;}}
    }
    CompletionTimeline(const CompletionTimeline&)=delete;
    CompletionTimeline& operator=(const CompletionTimeline&)=delete;
    // Borrow for bind_completion only. No outside CPU/queue may Signal it.
    ID3D12Fence* binding_fence() const noexcept {return fence_.Get();}
    Ticket reserve(std::uint64_t frame) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock() || failed_ || !frame || frame<=last_frame_ || next_>=UINT64_MAX-1)return {};
        for(unsigned i=0;i<entries_.size();++i)if(entries_[i].state==State::invalid){
            auto& e=entries_[i];e.ticket={i,++generation_,++next_,frame};e.state=State::reserved;last_frame_=frame;return e.ticket;}
        return {};
    }
    bool cancel_before_recording(Ticket t) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock() || failed_)return false;
        auto* e=find(t);if(!e || e->state!=State::reserved)return false;e->state=State::invalid;return true;
    }
    bool recording(Ticket t) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock() || failed_)return false;
        auto* e=find(t);if(!e || e->state!=State::reserved)return false;e->state=State::recorded;return true;
    }
    // Why a signal did not happen. The whole point of naming these is that they
    // are NOT the same event: `busy` is a try-lock miss against an owner thread
    // that is polling this very timeline, which this class provokes by design
    // (it never blocks a game thread), while the rest are real disagreements.
    // Collapsing them into one bool made an expected contention miss terminate
    // NR -- observed in Alan Wake 2 under rapid ON/OFF toggling.
    enum class Signal { ok, busy, already_failed, wrong_queue, wrong_state, out_of_order, fence_overtaken, signal_failed };
    static const char* signal_name(Signal s) noexcept {
        switch(s){
            case Signal::ok:return "ok";
            case Signal::busy:return "timeline-lock-contention";
            case Signal::already_failed:return "timeline-already-failed";
            case Signal::wrong_queue:return "signal-queue-is-not-the-timeline-queue";
            case Signal::wrong_state:return "ticket-not-in-recorded-state";
            case Signal::out_of_order:return "earlier-ticket-still-unsubmitted";
            case Signal::fence_overtaken:return "fence-already-past-this-ticket";
            case Signal::signal_failed:return "queue-signal-call-failed";
        }
        return "unknown";
    }
    // Must be the same native queue. API return alone is never a GPU completion.
    // Only `busy` is worth retrying; it changes nothing and leaves no state.
    Signal submit_signal(Ticket t,ID3D12CommandQueue* queue) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return Signal::busy;
        if(failed_)return Signal::already_failed;
        if(queue!=queue_.Get())return Signal::wrong_queue;
        auto* e=find(t);if(!e || e->state!=State::recorded)return Signal::wrong_state;
        // A later fence value must not overtake an earlier unsubmitted ticket.
        for(const auto& other:entries_)if((other.state==State::reserved || other.state==State::recorded) && other.ticket.value<t.value)return Signal::out_of_order;
        if(fence_->GetCompletedValue()>=t.value){failed_=true;return Signal::fence_overtaken;}
        if(FAILED(queue_->Signal(fence_.Get(),t.value))){failed_=true;return Signal::signal_failed;}
        e->state=State::submitted;++signals_;return Signal::ok;
    }
    bool submitted(Ticket t,ID3D12CommandQueue* queue) noexcept {return submit_signal(t,queue)==Signal::ok;}
    State inspect(Ticket t) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return State::busy;
        auto* e=find(t);return e?state(*e):State::invalid;
    }
    bool retire(Ticket t) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return false;
        auto* e=find(t);if(!e || state(*e)!=State::complete)return false;e->state=State::invalid;++retired_;return true;
    }
    // The native Reset hook proved the list holding this RECORDED ticket was
    // Reset before any ExecuteCommandLists: D3D12 destroyed the recording, so
    // nothing of it will run. Only a recorded (never submitted) ticket can be
    // discarded; it stops blocking later tickets and never receives a Signal.
    // Called from a game thread; a lock miss is reported, not treated as proof.
    Discard discard_before_submission(Ticket t) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return Discard::busy;
        if(failed_)return Discard::refused;
        auto* e=find(t);if(!e || e->state!=State::recorded)return Discard::refused;
        e->state=State::discarded;++discarded_;return Discard::discarded;
    }
    // Owner acknowledges a discard (no fence involved) and frees the slot.
    bool retire_discarded(Ticket t) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return false;
        auto* e=find(t);if(!e || e->state!=State::discarded)return false;e->state=State::invalid;return true;
    }
    void fail() noexcept {std::lock_guard lock(mutex_);failed_=true;} // Owner/worker shutdown, not render callback.
    // A native submission observer may discover reuse/loss on another thread.
    // Fail closed without waiting for a lock; no future ticket can be retired.
    void invalidate() noexcept {failed_=true;}
    Stats snapshot() const {std::lock_guard lock(mutex_);Stats s;s.signals=signals_;s.retired=retired_;s.discarded=discarded_;s.failed=failed_;
        for(const auto& e:entries_){s.reserved+=e.state==State::reserved;s.recorded+=e.state==State::recorded;s.submitted+=e.state==State::submitted;}return s;}
};
}
