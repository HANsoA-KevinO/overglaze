// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_game_profile.hpp"
#include "lab_installation.hpp"
namespace lab::games {
// PBZ's ordinary proxy captured this build's NGX creation on two fresh launches.
// A name, engine, DRM label or matching module alone is not that evidence.
inline bool verified_early_ngx(const profiles::Facts& f){
    if(f.executable!="PBZ.exe"||f.route!="ngx-rr"||
       f.executable_sha256!="7b4fcd286a350780faf0824d73aa150b21cbf1bccde883e6a96386cbe09973b4")return false;
    const std::pair<const char*,const char*> pins[]{
        {"nvngx_dlss.dll","3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983"},
        {"nvngx_dlssd.dll","caa023181e613e114e0ee037ff022d73c25464f7a79cf720853759417ad3bf50"},
        {"sl.interposer.dll","8c87c9499461da561edd529aa9bf7831d67d7b94ebb1c1a5ed54ef4934e1ea4c"}};
    for(const auto& [name,hash]:pins){const auto i=f.modules.find(name);if(i==f.modules.end()||i->second!=hash)return false;}
    return true;
}
inline bool needs_early_ngx_loading(const Loader& loader,const profiles::Facts& facts){
    return loader.strategy=="late_d3d12"&&verified_early_ngx(facts);
}
}
