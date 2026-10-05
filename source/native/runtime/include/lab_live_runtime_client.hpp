// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_frame_receiver.hpp"
#include "lab_live_status_sink.hpp"
#include "lab_installation.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <optional>
namespace lab {
// Which host is driving the bridge: a game, or the synthetic fixture.
enum class LiveHostKind {native_game,native_fixture};

namespace live {
// A required export. The name is in the message so a bridge/host ABI mismatch
// is diagnosable from the fault record instead of guessed from a stack.
template<class F> F proc(HMODULE m,const char* name){
    auto p=reinterpret_cast<F>(GetProcAddress(m,name));
    if(!p)throw std::runtime_error(std::string("Live bridge export missing: ")+name);
    return p;
}
}

// The runtime half of the live bridge: load and identity-gate the bridge,
// resolve the 14 runtime exports, hold the NR mode/settings mailbox, and publish
// the nr_runtime status. It knows nothing about any research collector; the
// research subclass adds those through the virtual hooks below, and a controller
// host uses this class directly.
//
// Frames arrive through live::IFrameReceiver, so the Streamline boundary is
// somebody else's problem (provider/src/sl_frame_provider.cpp translates it).
class LiveRuntimeClient:public live::IFrameReceiver {
public:
    // required_variant 0 accepts any bridge; 1 demands the controller bridge,
    // 2 the research bridge. A bridge with no capability export reports 0.
    // proxy_host: the module of an overlay layer that hosts the caller as an
    // add-on, already identity-verified by that caller; the bridge unwraps the
    // layer's command objects through it. Null for every native host, which
    // refuses to run inside such a layer.
    LiveRuntimeClient(LiveStatusSink& sink,ID3D12CommandQueue* seed,HMODULE addon,
                      LiveHostKind kind,const Installation* installation=nullptr,
                      unsigned required_variant=0,HMODULE proxy_host=nullptr);
    // blocked: why the frame provider refused the last RR call, or null.
    void poll(const char* blocked);
    void stop() noexcept;
    // The game's own render queue, once the bridge has observed an RR
    // submission on it; null before that, or on a bridge older than ABI21.
    // Only the late-load host needs it -- see live::RenderQueue.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> render_queue() const noexcept;
    // Whether the runtime would USE an admitted frame now, as of the last
    // poll(): it waits for its first frame (or the first after a rebuild), or
    // NR is requested ON / compute-only, or a mode request is still pending.
    // An OFF frame of a ready runtime is not wanted. Host worker only; the
    // provider copies eOnlyValidNow tags only while this holds.
    bool wants_frames() const noexcept {
        if(stopped_.load())return false;const auto s=status_.state;
        return s==live::State::waiting_frame||s==live::State::waiting_rebuild_frame||
            (s==live::State::ready&&(status_.requested_on!=0||action_.has_value()));}
    // live::IFrameReceiver
    void frame(const live::Frame&) noexcept override;
    void boundary_returned(std::uint64_t call,bool success) noexcept override;
    void aborted(std::uint64_t call) noexcept override;
    void rejected(const RejectedCall&) noexcept override;
    void game_binding_enter(std::uint64_t call,std::uint64_t frame_index,void* command) noexcept override;
    void game_binding_exit(std::uint64_t call) noexcept override;

protected:
    ~LiveRuntimeClient()=default; // hosts never delete; the bridge context stays pinned
    void* context()const noexcept{return context_;}
    HMODULE bridge_module()const noexcept{return module_;}
    const live::Capabilities& capabilities()const noexcept{return bridge_capabilities_;}
    bool binding_preservation_active()const noexcept{return binding_preservation_active_;}
    bool enabled()const noexcept{return enabled_;}
    bool stopped()const noexcept{return stopped_.load();}
    // Research hooks. The base does nothing and reports nothing, which is
    // exactly the controller's behaviour.
    virtual void sample_research(){}
    virtual void publish_research_diagnostics(){}
    virtual void dispatch_research_captures(){}
    virtual bool research_available()const noexcept{return false;}
    virtual std::uint64_t research_raw_texture_files()const noexcept{return 0;}

private:
    LiveStatusSink& sink_;
    void* context_=nullptr;
    HMODULE module_=nullptr;
    live::Poll poll_=nullptr;live::Request request_=nullptr;live::Enter enter_=nullptr;
    live::PollBindings poll_bindings_=nullptr;live::BindingStatus binding_status_;
    live::GameBindingEntry game_entry_=nullptr;live::GameBindingExit game_exit_=nullptr;
    live::Configure configure_=nullptr;live::Apply apply_=nullptr;
    live::BoundaryReturned returned_=nullptr;live::Stop stop_=nullptr;live::Reject reject_=nullptr;
    live::RenderQueue render_queue_=nullptr; // optional: absent on a pre-ABI21 bridge
    live::Status status_;
    live::Capabilities bridge_capabilities_{};
    json bridge_=nullptr;
    // Capability discovery is monotonic within this process: once the NR DLL has
    // returned one of our knobs during Evaluate, that knob is proven for this
    // game and this build, and turning NR off later does not un-prove it. The
    // live per-frame read-back stays in settings.observed, which is not sticky.
    // Nothing here is a claim about official runtime equivalence.
    struct Discovered {bool load=false,toggle=false,tone=false,structure=false,style=false,exposure=false,gpu=false;};
    Discovered discovered_;
    // nr_runtime.capabilities: what this host has actually seen work, plus the
    // evidence level (L1/L2/L3) the capability handshake reads. Call once per poll, after status_:
    // it folds this poll's evidence into discovered_ and then reports it.
    json capability_report();
    LiveHostKind kind_=LiveHostKind::native_game;
    bool proxy_host_=false; // started under a caller-verified overlay layer
    std::string game_profile_="cyberpunk2077-rr-v1";
    bool binding_preservation_=false,binding_preservation_active_=false;
    // Late attach (loader late_d3d12 or root_proxy_on_insert) and the unknown-indirect policy actually in force.
    bool late_attach_=false;std::string unknown_indirect_policy_="strict";
    bool enabled_=false;
    std::atomic<bool> stopped_{false};
    std::optional<json> action_;bool dispatched_=false;
};
}
