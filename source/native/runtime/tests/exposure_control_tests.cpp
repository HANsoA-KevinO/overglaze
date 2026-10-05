// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_exposure_control.hpp"
#include "lab_nr_settings.hpp"
#include "lab_control.hpp"
#include <cmath>
#include <iostream>
#include <limits>
namespace {
void need(bool v,const char* m){if(!v)throw std::runtime_error(m);}
bool close_to(float a,float b,float eps=1e-4f){return std::fabs(a-b)<=eps;}
}
int main(){try{
    using lab::nr::ExposureController;
    ExposureController c;
    need(!c.valid&&close_to(c.gain_stops(),ExposureController::seed_stops),"Seed before any reading");
    // 007 First Light capture statistics: median working luminance 0.0045 -> log2 = -7.796; target 0.18 -> gain ~ +5.32 stops.
    const float dark=std::log2(0.0045f);
    need(close_to(c.update(dark,false),ExposureController::target_log2_luminance-dark,1e-4f)&&c.valid,"First reading snaps to target");
    const float before=c.gain_stops();
    c.update(dark+3.f,false); // scene became 8x brighter: target drops by 3 stops, blended 10%
    need(close_to(c.gain_stops(),before-0.3f,1e-4f),"Smoothing moves ten percent per frame");
    c.update(dark+3.f,true);need(close_to(c.gain_stops(),before-3.f,1e-4f),"Snap jumps to target");
    need(close_to(c.update(50.f,true),ExposureController::min_auto_stops),"Very bright clamps low");
    // Alan Wake 2 daylight, measured: log2 mean 6.71 must reach mid-grey.
    need(close_to(c.update(6.714f,true),ExposureController::target_log2_luminance-6.714f),"Scene-referred daylight is reached, not clamped");
    need(close_to(c.update(-50.f,true),ExposureController::max_auto_stops),"Very dark clamps high");
    const float held=c.gain_stops();need(close_to(c.update(std::numeric_limits<float>::quiet_NaN(),true),held),"Nonfinite reading ignored");
    c.reset();need(!c.valid&&close_to(c.gain_stops(),ExposureController::seed_stops),"Reset returns to seed");
    need(close_to(ExposureController::clamp_applied(20.f),ExposureController::max_applied_stops)&&close_to(ExposureController::clamp_applied(-20.f),ExposureController::min_applied_stops),"Applied clamp");
    lab::nr::Settings s;s.exposure_auto=1;need(s.valid(),"Auto flag valid");s.exposure_auto=2;need(!s.valid(),"Auto flag range");
    lab::Controller ctl;ctl.enable_nr_preparation(true);ctl.enable_embedded_control();
    need(ctl.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"exposure_auto",true}},1)["ok"]==true,"Boolean auto flag accepted");
    need(ctl.status()["nr_settings_request"]["values"]["exposure_auto"]==1,"Auto flag staged");
    need(ctl.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"exposure_stops",1.5}},1)["ok"]==true&&ctl.status()["nr_settings_request"]["values"]["exposure_auto"]==1,"Omitted flag keeps auto");
    need(ctl.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"exposure_auto",2}},1)["error"]["code"]=="bad_config","Out-of-range flag rejected");
    need(ctl.handle_embedded("SetNrSettings",{{"tone",1},{"structure",1},{"exposure_auto",0}},1)["ok"]==true&&ctl.status()["nr_settings_request"]["values"]["exposure_auto"]==0,"Integer flag accepted");
    std::cout<<"PASS: auto-exposure controller math, clamps, reset and protocol flag\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
