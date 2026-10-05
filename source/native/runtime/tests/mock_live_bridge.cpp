// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// A fake live bridge: the 14 runtime entry points minus one.
//
// LabNrLiveApply is deliberately absent, and LabNrLiveCapabilities is absent
// too, so this module stands for the two shapes a wrong bridge can take:
//   - a bridge built against an older host, missing an entry the host needs
//   - a bridge with no capability export at all, which the client documents as
//     reporting variant 0
//
// Nothing here does any work. The bodies exist only so the linker emits the
// exports; calling one is a test bug, so each one traps instead of returning a
// plausible value. This module must never be loaded by a host: it is named
// lab_mock_live_bridge.dll, not overglaze_nvngx.dll, so the client's own path
// derivation can never pick it up.
#include <windows.h>

namespace {
[[noreturn]] void never(){RaiseException(0xE0000001,EXCEPTION_NONCONTINUABLE,0,nullptr);__assume(0);}
}

// __cdecl on x64 is undecorated, so these export under exactly these names.
extern "C" {
__declspec(dllexport) void LabNrLiveStart(){never();}
__declspec(dllexport) void LabNrLiveEnableBindingPreservation(){never();}
__declspec(dllexport) void LabNrLiveGameBindingEntryV1(){never();}
__declspec(dllexport) void LabNrLiveGameBindingExitV1(){never();}
__declspec(dllexport) void LabNrLivePoll(){never();}
__declspec(dllexport) void LabNrLivePollBindings(){never();}
__declspec(dllexport) void LabNrLiveRequest(){never();}
// LabNrLiveApply intentionally NOT exported.
__declspec(dllexport) void LabNrLiveConfigure(){never();}
__declspec(dllexport) void LabNrLiveEnter(){never();}
__declspec(dllexport) void LabNrLiveBoundaryReturned(){never();}
__declspec(dllexport) void LabNrLiveStop(){never();}
__declspec(dllexport) void LabNrLiveReject(){never();}
__declspec(dllexport) void LabNrLiveRenderQueue(){never();}
__declspec(dllexport) void LabNrLiveBindingPolicyV1(){never();}
// LabNrLiveCapabilities intentionally NOT exported.
}

BOOL APIENTRY DllMain(HMODULE,DWORD,LPVOID){return TRUE;}
