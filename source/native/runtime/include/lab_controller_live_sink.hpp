// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_live_status_sink.hpp"
#include "lab_control.hpp"
namespace lab {
// The one place that binds the live bridge client to Controller. Both hosts use
// it today; it exists so that a host which does not own a Controller can
// substitute its own sink without touching the client.
class ControllerLiveSink final:public LiveStatusSink {
    Controller& controller_;
public:
    explicit ControllerLiveSink(Controller& controller) noexcept:controller_(controller){}
    void publish_nr_runtime(json snapshot) override {controller_.publish_nr_runtime(std::move(snapshot));}
    void interrupt_nr_for_rebuild(std::uint64_t serial,std::uint64_t frame) override {controller_.interrupt_nr_for_rebuild(serial,frame);}
    void enable_nr_frame_control(const std::string& origin,bool settings,bool pair) override {controller_.enable_nr_frame_control(origin,settings,pair);}
    std::optional<json> take_nr_mode_request(std::uint64_t now_ms) override {return controller_.take_nr_mode_request(now_ms);}
    void acknowledge_nr_mode(std::uint64_t revision,std::uint64_t frame,bool success,
                             bool nr_evaluated,bool nr_output_selected,const std::string& error) override {
        controller_.acknowledge_nr_mode(revision,frame,success,nr_evaluated,nr_output_selected,error);
    }
    void diagnostic(const std::string& text) override {controller_.diagnostic(text);}
};
}
