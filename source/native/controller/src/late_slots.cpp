// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Swapchain vtable slot indices from the installed SDK, never counted by hand.
// Its own translation unit because CINTERFACE replaces the C++ interface
// definitions for the whole file, which breaks WRL and IID_PPV_ARGS elsewhere.
#define CINTERFACE
#include <d3d12.h>
#include <dxgi1_4.h>
#include <array>
#include <cstddef>
namespace lab::latebind {
// Present, Present1, ResizeBuffers, ResizeBuffers1, SetColorSpace1 — the same
// five the controller's Present hook bank installs, in that order.
std::array<std::size_t,5> swapchain_slots(){
#define SLOT(name) offsetof(IDXGISwapChain3Vtbl,name)/sizeof(void*)
    return {SLOT(Present),SLOT(Present1),SLOT(ResizeBuffers),SLOT(ResizeBuffers1),SLOT(SetColorSpace1)};
#undef SLOT
}
// The present-queue witness's four hooks (lab_present_queue_witness.hpp).
std::array<std::size_t,4> witness_slots(){
    return {offsetof(ID3D12GraphicsCommandListVtbl,ResourceBarrier)/sizeof(void*),
            offsetof(ID3D12GraphicsCommandList7Vtbl,Barrier)/sizeof(void*),
            offsetof(ID3D12CommandQueueVtbl,ExecuteCommandLists)/sizeof(void*),
            offsetof(ID3D12CommandQueueVtbl,Wait)/sizeof(void*)};
}
}
