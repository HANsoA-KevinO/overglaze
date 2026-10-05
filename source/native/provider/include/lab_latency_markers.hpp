// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// Which game frame each native Present belongs to, in the game's OWN words.
// Shared provider code (needed with frame generation on, e.g. Cyberpunk 2077 with
// DLSS-G): the binding layer
// of BOTH hosts asks it at every Present whether that Present is declared for an
// earlier frame than the tags it would otherwise expire (lab_sl_bindings.hpp
// present_boundary_of_frame), and the research chain capture keeps using it.
//
// Streamline games mark the stages of every frame for Reflex / PC Latency with
// the frame token they fetched for that frame:
//   slPCLSetMarker(sl::PCLMarker, const sl::FrameToken&)        sl_pcl.h, SL >= 2.4, kFeaturePCL (4)
//   slReflexSetMarker(sl::ReflexMarker, const sl::FrameToken&)  sl_reflex.h, older SDKs, kFeatureReflex (3)
// ePresentStart (4) right before the game calls Present, ePresentEnd (5) after
// it; 0..3 are simulation / render-submit, higher values exist and are only
// counted. The local SDK copy has the core headers only (no sl_pcl.h /
// sl_reflex.h), so the function type is declared here: both marker enums are
// 32-bit and travel in the first integer register, so one type serves both.
//
// Resolution is the game's own: the interposer's public slGetFeatureFunction
// (exactly workbench_adapter.cpp public_setter_address -- an image scan can hook
// an sl.dlss_d copy the game never calls), the pointer it
// hands back, the module owning it pinned, its signature checked (fixture:
// address checked at attach instead). The hook is observe-only: the detour
// records, then calls the original with the same arguments and returns its
// result unchanged. One record is a ring slot (one fetch_add, a QPC, relaxed
// stores) plus a token lookup over a fixed table: no allocation, no lock, no COM,
// no file I/O. Installed by either host once its NR bridge exists (the host
// worker polls); the token feed and the Present reading go through the adapter.
//
// The frame index of a marker is the index the host's boundary observer saw
// slGetNewFrameToken return for that token ADDRESS -- the same source the
// binding uses for the anchor call's index. It is never read from the token
// object and never guessed: an address not seen issued is "unknown". Streamline
// reuses its token objects, so a marker set with a token after that address was
// reissued names the newer frame -- what Streamline's own latency plugin reports
// for it, and WRONG for a late ePresentStart of the frame the address carried
// before (that can pair the wrong image with the anchor).
// Presents start in frame order, so a reissue while the previous frame has not
// started yet (no ePresentStart of that frame or a later one recorded) leaves the
// address pending: the next ePresentStart through it is "ambiguous" (frame_known
// 2), never the newer frame.
#include "lab_platform.hpp"
#include "lab_latency_marker_types.hpp"
#include <sl_core_api.h>
#include <array>
#include <atomic>
#include <mutex>
namespace lab::chain {
using MarkerFunction=sl::Result(std::uint32_t marker,const sl::FrameToken& frame);
class LatencyMarkers final {
public:
    static constexpr unsigned kRing=256,kScan=64,kTokens=32,kCounted=20,kGiveUp=60;
    LatencyMarkers() noexcept;
    LatencyMarkers(const LatencyMarkers&)=delete;
    LatencyMarkers& operator=(const LatencyMarkers&)=delete;
    // --- host worker
    bool due(std::uint64_t now_ms) const noexcept {return settled_.load()!=3u&&now_ms>=next_poll_ms_;}
    // resolver: the interposer's verified public slGetFeatureFunction, or the
    // verified fixture's stand-in (fixture=true: the module signature is not
    // required; the address was checked at attach); null: no verified resolver in
    // this process (counted like a refusal). Polls once a second; a function the
    // resolver does not provide in kGiveUp consecutive polls (about a minute) is
    // settled as not provided, so polling stops once both are settled (it never
    // polls forever). Installs at most one hook per function
    // for the life of the process. noexcept: every failure is recorded in snapshot().
    void poll(PFun_slGetFeatureFunction* resolver,bool fixture,std::uint64_t now_ms) noexcept;
    json snapshot() const;
    // The few counters a bounded host status can afford (the controller host):
    // what is hooked, how many ePresentStart markers arrived, and the distrust flags.
    json summary() const;
    unsigned functions() const noexcept {return functions_.load(std::memory_order_acquire);}
    // --- boundary observer (slGetNewFrameToken returned this address for this index)
    void token_returned(const void* token,std::uint32_t index,bool known) noexcept;
    // --- Present thread, at the host's pre-Present (before the panel and the
    // original Present): what the game's markers say about THIS Present.
    PresentMarker at_present(unsigned thread,std::uint64_t now_qpc) const noexcept;
    // --- the detours (and the tests: inject a marker without a hook)
    void record(MarkerSource,std::uint32_t marker,const void* token,unsigned thread,std::uint64_t qpc) noexcept;
    void nested() noexcept {++nested_;}
    // Token address -> frame index as learned; false: unknown (never guessed).
    bool lookup(const void* token,std::uint32_t& index) const noexcept;
private:
    struct Entry {std::uint32_t marker=0,frame=0,known=0,thread=0,source=0;std::uint64_t qpc=0,sequence=0;};
    struct Slot {std::atomic<std::uint64_t> seq{0},qpc{0};std::atomic<std::uint32_t> marker{0},frame{0},known{0},thread{0},source{0};};
    // pending: nonzero (the issue stamp) when this issue replaced a frame of the same
    // address that had not started yet (pending_frame); consumed by one ePresentStart.
    struct TokenSlot {std::atomic<std::uint64_t> version{0},seen{0},pending{0};std::atomic<std::uintptr_t> pointer{0};
        std::atomic<std::uint32_t> index{0},known{0},pending_frame{0};};
    struct Resolved {bool known=false;std::uint32_t index=0,pending_frame=0;std::uint64_t pending=0;unsigned slot=0;};
    struct Function {const char* name="";sl::Feature feature=0;void* target=nullptr;unsigned last_result=UINT32_MAX,misses=0;
        const char* state="unresolved";std::string error;json module;};
    // 1 read, 0 not (yet) written by its writer (another thread, in progress), -1 overwritten.
    int read(std::uint64_t sequence,Entry&) const noexcept;
    void install(unsigned which,void* target,bool fixture);
    bool resolve(const void* token,Resolved&) const noexcept;
    // Presents start in frame order: the latest known frame whose ePresentStart was
    // recorded (bit 32 set once one was); "not started" compares modulo 2^32.
    bool not_started(std::uint32_t frame) const noexcept;
    void started(std::uint32_t frame) noexcept;
    std::array<Slot,kRing> ring_{};
    std::atomic<std::uint64_t> head_{0};
    std::array<TokenSlot,kTokens> tokens_{};
    std::atomic<std::uint64_t> token_stamp_{0},token_updates_{0},last_started_{0};
    std::atomic<bool> token_untrusted_{false};
    std::array<std::atomic<std::uint64_t>,kCounted+1> counts_{};
    std::atomic<std::uint64_t> present_starts_{0},nested_{0},unknown_tokens_{0},aliased_{0},pending_set_{0};
    std::atomic<unsigned> functions_{0},settled_{0}; // bit 0 PCL, bit 1 Reflex: hooked / hooked-or-failed-for-good
    std::uint64_t window_qpc_=0,frequency_=0;
    // Worker only (poll and snapshot run on the host worker); the mutex keeps
    // snapshot() honest if a test calls it elsewhere.
    mutable std::mutex worker_mutex_;
    std::array<Function,2> fn_{};
    std::uint64_t next_poll_ms_=0,attempts_=0;std::string resolver_error_;
    void miss(unsigned which,const char* settled_state) noexcept;
};
}
