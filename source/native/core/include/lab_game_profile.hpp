// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace lab::profiles {
// Exact executable/module allowlists, not a claim of game runtime acceptance.
struct Game {
    std::string_view id, executable, executable_sha256, capture_origin, settings_file;
    std::uint32_t viewport=0; // Exact observed single-view profile; not a display index.
    bool native_evaluate_host_rebind=false; // Reviewed modern SL public API contract; runtime command must be native.
    bool linear_depth=false; // Exact observed public tag. Observation does not authorize raw NR use.
    // Host colour-preparation default (log2 stops). 007: the saved same-call
    // package showed working-RGB median luminance 0.0045, so exposure 1 fed NR
    // an almost black image; +5 stops (32x) is a starting point for the user
    // slider, not a measured game constant.
    float default_exposure_stops=0.f;
    // Native command-list binding preservation around our insertion (007 needed
    // it against its own state corruption; 2077 runs without). A per-game fact,
    // not a string comparison in the host.
    bool binding_preservation=false;
};
inline constexpr std::array games{
    Game{"cyberpunk2077-rr-v1", "Cyberpunk2077.exe",
        "a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991",
        "cyberpunk2077-controlled-rr-stage", "overlay-cyberpunk2077.json"},
    // Measured live (late load): admitted, cross-thread constants, NR ready;
    // without binding preservation the first NR insertion was followed by an
    // access violation (C0000005) in the display driver (nvwgf2umx.dll) on a
    // game thread that was still RECORDING into the same command list, and
    // that recording never closed or reached ExecuteCommandLists (evaluates 1,
    // retired 0, no discard). No host rebind after the SL Evaluate was ever
    // observed. RE9, like 007, keeps recording on the bindings it had before
    // the Evaluate, so our insertion must restore them: binding preservation on. Viewport 0, hardware depth and no
    // host rebind are the observed values.
    Game{"re9-rr-v1", "re9.exe",
        "9742b40b5a177cee95015714488fe7bdddbe5348892033b4c807d823a20ee168",
        "re9-controlled-rr-stage", "overlay-re9.json",0,false,false,0.f,true},
    // Experimental exact-build entry. Static contract != game runtime acceptance.
    // The hash below is 1.3.0.0 (Authenticode Valid, IO INTERACTIVE A/S). Steam
    // updates have replaced the executable several times; each time the three
    // SL modules (sl.interposer/common/dlss_d), D3D12Core and the NR model were
    // unchanged, so the SL profiles hold: every private RVA is keyed on those
    // SL module hashes, none on the executable, and only this pin moves.
    // Nothing here verifies 1.3.0.0 behaviour in-game.
    Game{"007-first-light-rr-v1", "007FirstLight.exe",
        "9d82479246d2c2fbcbcbb94e24aeb91bc8917241148278a3288031afb5828bf2",
        "007-first-light-controlled-rr-stage", "overlay-007-first-light.json",1,true,true,5.f,true},
    // Alan Wake 2 (Epic, Streamline 2.7.0; the hash below is FileVersion
    // 0.559.302.8, ProductVersion 2026.2.3.0). No Denuvo (7 standard sections).
    // No reviewed interposer, options-setter or begin/end profile exists for
    // this build: it attaches through the public export path and admits on
    // public evidence only. Two facts were MEASURED in the running game, not
    // assumed; every Evaluate was refused until the row matched what the game
    // actually does:
    //   viewport 1      -- 260 calls refused "viewport-unsupported" while the game
    //                      passed 1 and this row carried the default 0.
    //   linear depth    -- 430 calls then refused "fresh-resource-missing-or-revoked"
    //                      because we looked for hardware depth (type 0), which this
    //                      game never tags. Its inline tag set is
    //                      {4,3,1,13,7,8,14,49,42,58}: linear depth (49) is there,
    //                      type 0 is not. Same shape as 007 First Light.
    // Still NOT measured: native_evaluate_host_rebind and the exposure default stay
    // at their defaults, and no reviewed interposer profile exists for this build.
    Game{"alanwake2-rr-v1", "AlanWake2.exe",
        "0e7a7b5fe05ada30ac043d96eeb658e934db96b4783479ade64495ed428b5904",
        "alanwake2-controlled-rr-stage", "overlay-alanwake2.json", 1, false, true}
};
inline const Game* game(std::string_view id) noexcept {
    for(const auto& p:games)if(p.id==id)return &p;return nullptr;
}
inline const Game* executable(std::string_view name,std::string_view hash) noexcept {
    // Profile basenames are ASCII, never locale-dependent. Hashes and profile
    // IDs deliberately remain case-sensitive exact identities.
    auto same=[](std::string_view a,std::string_view b){
        if(a.size()!=b.size())return false;
        auto lower=[](char c){return c>='A'&&c<='Z'?char(c+('a'-'A')):c;};
        for(std::size_t i=0;i<a.size();++i)if(lower(a[i])!=lower(b[i]))return false;
        return true;
    };
    for(const auto& p:games)if(same(p.executable,name)&&p.executable_sha256==hash)return &p;return nullptr;
}
// Runtime game facts. Reviewed games take them from the compiled row above;
// games added through the controller carry them in the V3 installation config,
// which the host accepts only through the adapter package hash chain (see
// load_installation_file). Every consumer reads this struct, never the table.
struct Facts {
    std::string id, executable, executable_sha256, capture_origin, settings_file, route="sl-rr", title, package;
    std::uint32_t viewport=0;
    bool native_evaluate_host_rebind=false, linear_depth=false, binding_preservation=false;
    float default_exposure_stops=0.f;
    std::map<std::string,std::string> modules; // game module pins re-verified at load
    bool reviewed=false; // a compiled row exists and every fact matched it
    static Facts from(const Game& g){
        Facts f;f.id=std::string(g.id);f.executable=std::string(g.executable);f.executable_sha256=std::string(g.executable_sha256);
        f.capture_origin=std::string(g.capture_origin);f.settings_file=std::string(g.settings_file);f.viewport=g.viewport;
        f.native_evaluate_host_rebind=g.native_evaluate_host_rebind;f.linear_depth=g.linear_depth;
        f.binding_preservation=g.binding_preservation;f.default_exposure_stops=g.default_exposure_stops;f.reviewed=true;return f;
    }
};
}
