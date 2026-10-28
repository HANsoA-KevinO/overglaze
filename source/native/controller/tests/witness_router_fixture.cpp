// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Test-only second DLL: the NR bridge's real Reset/Close/Execute and binding
// observers, with its own static MinHook instance. No model or NR execution.
#include "lab_live_submission.hpp"
namespace {lab::LiveSubmission* router=nullptr;lab::InsertionBindings* bindings=nullptr;}
extern "C" __declspec(dllexport) BOOL WitnessRouterStart(ID3D12CommandQueue* queue){
    try{router=lab::LiveSubmission::install(queue,nullptr);bindings=router->enable_binding_preservation();return TRUE;}catch(...){return FALSE;}
}
extern "C" __declspec(dllexport) BOOL WitnessRouterObserved(){
    try{return router&&bindings&&!router->snapshot().value("terminal_fault",true)&&bindings->status().value("recording_begins",0ULL)>0;}catch(...){return FALSE;}
}
extern "C" __declspec(dllexport) void WitnessRouterStop(){if(router){router->uninstall_for_test();router=nullptr;bindings=nullptr;}}
