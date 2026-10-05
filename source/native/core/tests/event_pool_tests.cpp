// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_event_pool.hpp"
#include <atomic>
#include <thread>
#include <iostream>
#include <array>
#include <stdexcept>
#define REQUIRE(expr) do { if (!(expr)) throw std::runtime_error("FAILED: " #expr); } while (false)
struct Event { unsigned producer, number; std::uint64_t check; };
int main() {
    try {
        lab::EventPool<Event,64> small;
        for(unsigned i=0;i<64;++i) REQUIRE(small.push({0,i,i*17ULL}));
        REQUIRE(!small.push({0,64,0}));
        std::vector<Event> batch; batch.reserve(64); small.take_all(batch);
        REQUIRE(batch.size()==64);
        for(unsigned i=0;i<64;++i) REQUIRE(batch[i].number==i);
        REQUIRE(small.empty()); REQUIRE(small.push({1,0,42}));
        batch.clear(); small.take_all(batch); REQUIRE(batch[0].check==42);
        constexpr unsigned producers=8, per_thread=50000;
        lab::EventPool<Event,8192> pool;
        std::atomic<unsigned> finished{0};
        std::array<std::jthread,producers> workers;
        for(unsigned p=0;p<producers;++p) workers[p]=std::jthread([&,p](std::stop_token stop) {
            for(unsigned n=0;n<per_thread;++n) {
                if(stop.stop_requested()) return;
                Event e{p,n,static_cast<std::uint64_t>(p)*per_thread+n};
                // Test back-pressure only; production never waits for space.
                while(!pool.push(e)) {
                    if(stop.stop_requested()) return;
                    std::this_thread::yield();
                }
            }
            ++finished;
        });
        std::array<unsigned,producers> received{};
        std::vector<Event> events; events.reserve(8192);
        while(finished.load()!=producers || !pool.empty()) {
            events.clear(); pool.take_all(events);
            for(const auto& e:events) {
                REQUIRE(e.producer<producers); REQUIRE(e.number==received[e.producer]++);
                REQUIRE(e.check==static_cast<std::uint64_t>(e.producer)*per_thread+e.number);
            }
            if(events.empty()) std::this_thread::yield();
        }
        for(auto count:received) REQUIRE(count==per_thread);
        std::cout<<"PASS: aligned fixed pool, overflow, reuse, 8 producers / 400000 unique intact ordered-per-thread events\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
