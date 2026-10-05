// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_overlay.hpp"
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <wrl/client.h>
namespace lab::latebind {
// Which swapchain a late-loaded host follows.
//
// A late host never sees a swapchain created: it adopts the first one that
// presents. A game may later release that chain and create another on the same
// window -- what RE9 is believed to do when DLSS frame generation is switched on
// in game (unproven).
// DXGI admits one flip-model chain per HWND, so nothing here may keep the old
// one alive: chains are identities, compared and never dereferenced.
//
// A present from another chain takes over only when the followed chain has not
// presented for `stale_presents` consecutive late presents and the new chain has
// presented `confirm_presents` times. While both keep presenting the first stays
// followed (counted). Take-overs are capped per process.
class ChainTracker final {
public:
    struct Policy {unsigned stale_presents=120,confirm_presents=30,max_adoptions=8;};
    enum class Verdict {followed,other,take_over};
    explicit ChainTracker(Policy policy={}):policy_(policy){}
    void adopt(const void* chain)noexcept;
    Verdict present(const void* chain)noexcept;
    void refuse(const void* chain,const std::string& why);
    const void* followed()const noexcept{return followed_;}
    std::uint64_t adoptions()const noexcept{return adoptions_;}
    json status()const;
private:
    struct Entry {const void* chain=nullptr;std::uint64_t presents=0,last=0;bool refused=false;};
    static constexpr unsigned Tracked=4;
    Entry* entry(const void* chain)noexcept;
    Policy policy_;std::array<Entry,Tracked> entries_{};
    const void* followed_=nullptr;const void* previous_=nullptr;
    std::uint64_t seq_=0,followed_last_=0,adoptions_=0,switches_=0,other_presents_=0,held_=0,over_cap_=0,evictions_=0;
    std::map<std::string,std::uint64_t> refusals_;
};

// The late panel's binding to the followed chain. Driven from the Present
// detour only -- the one place the game's swapchain is touched, through the
// pointer the game handed that very call -- so nothing in the late path holds a
// reference to the chain or its back buffers between presents (the overlay
// acquires the current back buffer per present, lab_overlay.hpp).
//
// The queue the panel submits on comes from Evidence, never from inference. A
// contradiction of that evidence detaches the panel and asks for a fresh,
// bounded decision; past the bound the panel stays withdrawn with a named note.
class LatePanel final {
public:
    struct Answer {Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;std::string source,note;};
    struct Evidence {
        // May a take-over follow this chain? A refusal names itself in `why`.
        std::function<bool(IDXGISwapChain3*,std::string& why)> accept;
        // The queue proven to present this chain, or null and a note.
        std::function<Answer(IDXGISwapChain3*)> resolve;
        // Has the evidence behind `source` ("" while unattached) been contradicted?
        std::function<bool(const std::string& source)> contradicted;
        // A fresh decision under the same rules; false once the bound is spent.
        std::function<bool(std::uint64_t now_ms)> restart;
        // The followed chain changed.
        std::function<void()> rebind;
        // A changed note, for the host's diagnostics. Optional.
        std::function<void(const std::string&)> report;
    };
    LatePanel(GameOverlay& overlay,Evidence evidence,ChainTracker::Policy policy={});
    // The host's first adoption; the host seeds NR from it, this only follows it.
    void adopt(IDXGISwapChain* chain)noexcept;
    // Every late present, first. True when `chain` is the followed chain, after
    // a take-over if this present completed one.
    bool follow(IDXGISwapChain* chain)noexcept;
    // The followed chain's present, before the overlay draws: attach once the
    // evidence names a queue (only while `may_attach`), detach on contradiction.
    void bind(IDXGISwapChain* chain,bool may_attach,std::uint64_t now_ms)noexcept;
    // Show the panel the next time it attaches (the player's Insert started us).
    void open_when_attached()noexcept{open_=true;}
    bool attached()const;
    // "attached", the withdrawal note, the latest note, or waiting-for-render-queue.
    std::string state()const;
    std::string source()const;
    json status()const;
private:
    void note(std::string text);
    void unbind(const char* why);
    GameOverlay& overlay_;Evidence evidence_;ChainTracker chains_;
    mutable std::mutex mutex_;
    bool attached_=false,withdrawn_=false;std::atomic<bool> open_{false};
    std::string source_,withdrawn_note_;std::deque<std::string> notes_;
    std::uint64_t attaches_=0,detaches_=0,redecisions_=0,refused_redecisions_=0,refused_take_overs_=0,detach_failures_=0;
};
}
