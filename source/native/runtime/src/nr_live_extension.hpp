// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// nr::Frame reaches the seam through the verified snippet ABI, the same one the
// bridge itself is built against; declaring it here keeps every extension
// translation unit on that ABI whatever it includes first.
#ifndef NGX_SNIPPET_BUILD
#define NGX_SNIPPET_BUILD
#endif
#include "lab_nr_live_api.hpp"
#include "lab_nr_frame.hpp"
#include <string>
namespace lab::live {
struct Live;
// Link-time seam between the shared NR runtime core (nr_live_core.hpp) and the
// research-only collectors: frame capture, display pair capture, the binding
// and input-preparation probes, and the boundary audit. Exactly one definition
// of create_live_extension() is linked into a bridge:
//   controller bridge -> runtime/src/nr_live_extension_none.cpp  (returns nullptr)
//   research bridge   -> research/src/nr_live_research.cpp
// so the controller bridge's link closure contains none of those objects.
//
// The extension sees the whole Live core by reference; the core knows nothing
// about capture types. Every hook runs under the serial call gate the core
// already holds. Hooks marked noexcept also run from game callbacks after a
// failure and must not throw; the others run on the worker, where a throw is
// caught and becomes the context's first fault.
// What the bridge knows about the frame it has just recorded; the research
// collector merges it with the SL-side context the host published separately.
struct RecordedFrameFacts {
    std::uint64_t frame=0,call=0;
    ID3D12GraphicsCommandList* command=nullptr;
    unsigned nr_mode=0,history_reset=0,host_reset_intervention=0;
    unsigned style_observed=0,settings_read_mask=0;
    std::uint64_t settings_revision=0;
};
class LiveExtension {
public:
    virtual ~LiveExtension()=default;
    // NR ON is refused while a diagnostic or a display capture owns the frame.
    virtual bool blocks_nr_on()noexcept{return false;}
    // Deadline/cancellation upkeep for armed diagnostics.
    virtual void expire(Live&)noexcept{}
    virtual void on_fail()noexcept{}
    // After the Feature is created and the core's colour constants are final.
    virtual void on_prepared(Live&){}
    // Stop and join collectors; throws if a completion cannot be certified.
    virtual void on_finish(Live&){}
    virtual void on_rebuild_begin()noexcept{}
    // Stop requested: cancel collectors without claiming their GPU work drained.
    virtual void on_stopping()noexcept{}
    // false: a collector still holds GPU work, so the context must not finish yet.
    virtual bool stop_drained()noexcept{return true;}
    // false: the extension still holds GPU dependencies (the caller returns and
    // retries on the next poll); it demands its own certainty before saying so.
    virtual bool rebuild_drained(){return true;}
    virtual void on_rebuild_release(Live&){}
    // true: that recording/completion belonged to the extension, not to NR, so
    // the core must not count it as a discarded NR frame or an NR retirement.
    virtual bool on_recording_discarded()noexcept{return false;}
    virtual bool on_retired()noexcept{return false;}
    virtual void rr_admitted(std::uint64_t call)noexcept{}
    // The whole OFF-frame branch: probes, their recorded work, and the display
    // capture. The core has already reset history and taken the boundary.
    virtual void off_frame(Live&,const Frame&){}
    virtual void boundary_returned(std::uint64_t call,bool ok)noexcept{}
    virtual void poll(Live&)noexcept{}
    // Collector hooks inside the ON recording lambda.
    virtual bool begin_inputs(Live&,const nr::Frame&){return false;}
    virtual void record_output(Live&,const nr::Frame&){}
    virtual void record_result(Live&,const RecordedFrameFacts&,const nr::Settings& recorded,bool compare_split,std::string getters){}
};
// Both defined once per bridge by the linked extension translation unit.
LiveExtension* create_live_extension();
void describe_live_variant(Capabilities&)noexcept;
}
