// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <optional>
#include <string>
namespace lab {
// Where the live bridge client publishes, and where it takes its mode requests
// from. Exactly the six Controller entry points the runtime client needs, and
// nothing else.
//
// It inverts the dependency: the runtime client never names Controller, so a
// controller host can supply its own sink without carrying any research
// status key.
struct LiveStatusSink {
    virtual void publish_nr_runtime(json snapshot)=0;
    virtual void interrupt_nr_for_rebuild(std::uint64_t serial,std::uint64_t frame)=0;
    virtual void enable_nr_frame_control(const std::string& origin,bool settings,bool pair)=0;
    virtual std::optional<json> take_nr_mode_request(std::uint64_t now_ms)=0;
    virtual void acknowledge_nr_mode(std::uint64_t revision,std::uint64_t frame,bool success,
                                     bool nr_evaluated,bool nr_output_selected,const std::string& error)=0;
    virtual void diagnostic(const std::string& text)=0;
protected:~LiveStatusSink()=default;
};
}
