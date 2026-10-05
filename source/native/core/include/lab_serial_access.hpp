// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <thread>

namespace lab {
// A host explicitly enrolls its whole NR object graph in one domain. This is
// CPU serialization, NOT GPU completion, queue ownership, or proof that an
// arbitrary proprietary DLL supports thread migration. No wait/spin/allocation
// on admission. A busy/reentrant caller must skip before touching the graph.
// The domain outlives all enrolled objects and leases; teardown is host-owned.
class SerialCallGate final {
    std::atomic<DWORD> owner_{0};
public:
    class Lease final {
        SerialCallGate& gate_;
        DWORD thread_=0;
    public:
        explicit Lease(SerialCallGate& gate) noexcept :gate_(gate) {
            DWORD expected=0;
            const DWORD current=GetCurrentThreadId();
            if(gate_.owner_.compare_exchange_strong(expected,current,std::memory_order_acquire,std::memory_order_relaxed))thread_=current;
        }
        Lease(const Lease&)=delete;
        Lease& operator=(const Lease&)=delete;
        Lease(Lease&&)=delete;
        Lease& operator=(Lease&&)=delete;
        ~Lease() {
            if(!thread_)return;
            // Never unlock a live context from the wrong thread. Leases cannot
            // be moved into an asynchronous callback or used as GPU tickets.
            if(thread_!=GetCurrentThreadId() || gate_.owner_.load(std::memory_order_relaxed)!=thread_)std::terminate();
            gate_.owner_.store(0,std::memory_order_release);
        }
        explicit operator bool() const noexcept {return thread_!=0 && thread_==GetCurrentThreadId();}
    };
    SerialCallGate()=default;
    SerialCallGate(const SerialCallGate&)=delete;
    SerialCallGate& operator=(const SerialCallGate&)=delete;
    Lease try_enter() noexcept {return Lease(*this);}
    bool held_by_current_thread() const noexcept {return owner_.load(std::memory_order_relaxed)==GetCurrentThreadId();}
};

// Preserve the original fixed-owner policy unless an explicit shared domain is
// supplied. The pointer is immutable, so an object cannot be rebound to bypass
// an active lease or an unknown pending GPU operation.
class ThreadAccess final {
    const std::thread::id creator_=std::this_thread::get_id();
    SerialCallGate* const gate_;
public:
    explicit ThreadAccess(SerialCallGate* gate=nullptr):gate_(gate) {require();}
    void require() const {
        if(gate_ ? !gate_->held_by_current_thread() : creator_!=std::this_thread::get_id())
            throw std::logic_error("NR access requires its fixed owner or the enrolled serial lease");
    }
    SerialCallGate* gate() const noexcept {return gate_;}
};
}
