// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <memory>
#include <type_traits>
#include <vector>
#include <cassert>
#include <limits>

namespace lab {
// Fixed storage; producers use Windows interlocked SLists, never a mutex or heap.
// One consumer flushes/reverses a batch. Publication order is not GPU order.
// SList alignment/atomicity: https://learn.microsoft.com/en-us/windows/win32/sync/interlocked-singly-linked-lists
template<class T, size_t Capacity> class EventPool {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(Capacity > 0 && Capacity <= (std::numeric_limits<USHORT>::max)());
    struct alignas(MEMORY_ALLOCATION_ALIGNMENT) Node { SLIST_ENTRY entry; T value; };
    static_assert(offsetof(Node, entry) == 0);
    alignas(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER free_{};
    alignas(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER pending_{};
    std::unique_ptr<Node[]> nodes_ = std::make_unique<Node[]>(Capacity);
public:
    EventPool() {
        InitializeSListHead(&free_); InitializeSListHead(&pending_);
        for(size_t i=0;i<Capacity;++i) InterlockedPushEntrySList(&free_,&nodes_[i].entry);
    }
    bool push(const T& value) noexcept {
        return produce([&](T& slot) noexcept {slot=value;});
    }
    // Large bounded payloads are filled directly into a reserved node, not a render-thread stack temporary.
    template<class Fill> bool produce(Fill&& fill) noexcept {
        static_assert(std::is_nothrow_invocable_v<Fill,T&>);
        auto entry=InterlockedPopEntrySList(&free_);
        if(!entry) return false;
        fill(reinterpret_cast<Node*>(entry)->value);
        InterlockedPushEntrySList(&pending_,entry);
        return true;
    }
    // Caller reserves Capacity before producers start; no consumer allocations here.
    void take_all(std::vector<T>& output) noexcept {
        assert(output.empty() && output.capacity() >= Capacity);
        auto stack=InterlockedFlushSList(&pending_);
        PSLIST_ENTRY fifo=nullptr;
        while(stack) { auto next=stack->Next; stack->Next=fifo; fifo=stack; stack=next; }
        while(fifo) {
            auto next=fifo->Next;
            output.push_back(reinterpret_cast<Node*>(fifo)->value);
            InterlockedPushEntrySList(&free_,fifo); fifo=next;
        }
    }
    bool empty() noexcept { return QueryDepthSList(&pending_) == 0; }
};
}
