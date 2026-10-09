// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_exposure_control.hpp"
#include "lab_game_exposure.hpp"
#include "lab_nr_settings.hpp"
#include "lab_control.hpp"
#include <cmath>
#include <iostream>
#include <limits>
namespace {
void need(bool v,const char* m){if(!v)throw std::runtime_error(m);}
bool close_to(float a,float b,float eps=1e-4f){return std::fabs(a-b)<=eps;}
// Game first, meter fallback (Live ABI24): the selector alone, CPU only.
void game_exposure_selection(){
    using lab::nr::GameExposureSelector;using lab::live::ExposureNote;
    constexpr auto none=ExposureNote::none;
    // Alan Wake 2's shape: scene-referred working RGB (meter log2 mean 6.71) and
    // an exposure texture that brings it to about mid-grey.
    const float scene=6.71f,e=std::exp2(-2.5f-scene);
    // The game-exposed log-average a reading of E' would give for this scene.
    const auto value_for=[&](float exposed){return std::exp2(exposed-scene);};
    GameExposureSelector s;
    need(!s.decided&&!s.using_game()&&s.note==ExposureNote::waiting,"Nothing decided before a reading");
    s.observe(true,none,e,1.f,1.f,true,scene);
    need(s.using_game()&&s.note==none&&s.exposed_valid&&close_to(s.exposed_log2,-2.5f),"A plausible first reading is used at once");
    need(close_to(s.stops(1.f,1.f),std::log2(e)),"Gain is log2(E) with pre-exposure and scale 1");
    need(close_to(s.stops(.5f,2.f),std::log2(e*2.f/.5f)),"This frame's pre-exposure and scale are applied");
    need(close_to(s.stops(0.f,std::numeric_limits<float>::quiet_NaN()),std::log2(e)),"Unusable current values fall back to the reading's own");
    // Just outside the window but inside the one-stop margin: still trusted.
    const float edge=value_for(GameExposureSelector::max_exposed_log2+.5f);
    s.observe(true,none,edge,1.f,1.f,true,scene);
    need(s.using_game()&&s.note==none&&s.exposure==edge,"A trusted value is kept within the leave margin");
    // Far outside: held for switch_frames-1 readings with the last good E, then the meter.
    const float bright=value_for(GameExposureSelector::max_exposed_log2+2.f);
    for(unsigned i=1;i<GameExposureSelector::switch_frames;++i){s.observe(true,none,bright,1.f,1.f,true,scene);
        need(s.using_game()&&s.note==ExposureNote::implausible&&s.exposure==edge,"Implausible readings are held through, with the last good value");}
    s.observe(true,none,bright,1.f,1.f,true,scene);
    need(!s.using_game()&&s.note==ExposureNote::implausible&&s.switches==1,"The eighth implausible reading falls back to the meter");
    // Back: the plain window (no margin) and eight consecutive readings; one
    // implausible reading in between starts the count again.
    for(unsigned i=1;i<GameExposureSelector::switch_frames;++i){s.observe(true,none,e,1.f,1.f,true,scene);need(!s.using_game(),"No switch before eight plausible readings");}
    s.observe(true,none,edge,1.f,1.f,true,scene);need(!s.using_game()&&s.note==ExposureNote::implausible,"Outside the plain window counts against entering");
    for(unsigned i=1;i<GameExposureSelector::switch_frames;++i)s.observe(true,none,e,1.f,1.f,true,scene);
    need(!s.using_game(),"The count started again");
    s.observe(true,none,e,1.f,1.f,true,scene);
    need(s.using_game()&&s.note==none&&s.switches==2,"Eight consecutive plausible readings return to the game");
    // A missing reading while trusted is held too, and named.
    for(unsigned i=0;i<3;++i)s.observe(false,ExposureNote::tag_not_fresh,0.f,1.f,1.f,true,scene);
    need(s.using_game()&&s.note==ExposureNote::tag_not_fresh,"A short gap in the game's texture does not flip the source");
    s.observe(true,none,e,1.f,1.f,true,scene);need(s.using_game()&&s.note==none&&s.streak==0,"and the next good reading clears it");
    // Without a meter reading a trusted value continues (and is updated)...
    s.observe(true,none,e*2.f,1.f,1.f,false,0.f);need(s.using_game()&&s.exposure==e*2.f&&!s.exposed_valid,"No meter reading does not end a trusted value");
    // ...but cannot start one.
    GameExposureSelector u;u.observe(true,none,e,1.f,1.f,false,0.f);
    need(!u.using_game()&&u.note==ExposureNote::unverified,"An unverified first reading stays on the meter");
    // 007: no texture at all -> the meter from the first reading, named.
    GameExposureSelector m;for(unsigned i=0;i<20;++i)m.observe(false,ExposureNote::no_texture,0.f,1.f,1.f,true,std::log2(.0045f));
    need(m.decided&&!m.using_game()&&m.note==ExposureNote::no_texture&&m.switches==0,"No exposure texture: the meter, by name");
    // Values the GPU read but that cannot be an exposure.
    for(float bad:{0.f,-1.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
        GameExposureSelector b;b.observe(true,none,bad,1.f,1.f,true,scene);
        need(!b.using_game()&&b.note==ExposureNote::invalid_value,"A non-finite or non-positive texel is not an exposure");}
    {GameExposureSelector b;b.observe(true,none,e,0.f,1.f,true,scene);need(!b.using_game()&&b.note==ExposureNote::invalid_scale,"A frame's invalid pre-exposure is named");}
    {GameExposureSelector b;b.observe(false,none,0.f,1.f,1.f,true,scene);need(b.note==ExposureNote::invalid_value,"A missing reading without a reason is never none");}
    // Pre-exposed colour: E x scale / pre is what counts, not E alone.
    {GameExposureSelector b;const float pre=std::exp2(-9.f);b.observe(true,none,e*pre,pre,1.f,true,scene);
     need(b.using_game()&&close_to(b.exposed_log2,-2.5f)&&close_to(b.stops(pre,1.f),std::log2(e)),"Pre-exposure divides out");}
    s.reset();need(!s.decided&&!s.using_game()&&s.note==ExposureNote::waiting&&s.switches==0&&!s.have_exposure,"Reset forgets everything");
}
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
    game_exposure_selection();
    std::cout<<"PASS: auto-exposure controller math, clamps, reset and protocol flag; game exposure first, meter fallback, hysteresis\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
