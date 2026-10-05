// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <cstdint>
namespace lab {
// Test-only attachment for the controller dxgi.dll. Deliberately NOT the
// research FixtureBindings in lab_standalone.hpp: that contract is pinned by
// size and version in research/src/standalone_host.cpp, and the two fixtures
// must be able to change independently.
//
// The controller admits on PUBLIC evidence only, so a fixture supplies exactly
// the public surface:
//   public_api[0] slGetNewFrameToken   [1] slEvaluateFeature
//   public_api[2] slSetConstants       [3] slSetTag or slSetTagForFrame
//   options            the RR options setter (lab::rr::OptionsV3 signature)
//   sr_options         the SR options setter (lab::rr::SrOptionsPrefix
//                      signature), optional; version 2 added it
//   native_interface   a synthetic slGetNativeInterface; without it the public
//                      command check cannot run and every call is refused.
// There are no private begin/end callbacks and no host-flags address BY
// CONSTRUCTION: WorkbenchAdapterHost::attach_fixture refuses a private pair
// without a research extension, and the controller never attaches one.
struct ControllerFixtureBindings {
    unsigned size=sizeof(ControllerFixtureBindings),version=2;
    void* public_api[4]{};
    void* options=nullptr;
    void* native_interface=nullptr;
    unsigned frame_tagging=0;
    void* sr_options=nullptr;
};
using AttachControllerFixture=bool(__cdecl*)(const ControllerFixtureBindings*);
using ControllerStop=void(__cdecl*)();
}
