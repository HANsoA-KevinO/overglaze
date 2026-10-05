// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <dxgi1_4.h>
namespace lab::latebind {
// Defined in late_slots.cpp, which is compiled with CINTERFACE.
std::array<std::size_t,5> swapchain_slots();
// The Present-family method addresses of this process's D3D12 swapchain
// implementation, in the order the controller's Present hook bank installs them:
// Present, Present1, ResizeBuffers, ResizeBuffers1, SetColorSpace1.
//
// Read from a throwaway swapchain of our own, so they are a CANDIDATE for what
// the game uses, never a statement that the game uses them. Confirm with
// matches() against a real swapchain before trusting anything.
struct Discovery {
    std::array<void*,5> methods{};
    bool ready=false;
    bool agility=false; // the game had already loaded D3D12Core.dll (Agility SDK)
};
// Throws with a reason rather than returning a half-filled result. Creates and
// destroys its own device, queue, hidden window and swapchain; touches nothing
// belonging to the game.
Discovery discover_swapchain_methods();
// Does this real swapchain use exactly the implementation we discovered? A false
// here is the skew the caller must report instead of hooking the wrong code.
bool matches(const Discovery&,IDXGISwapChain3*)noexcept;
}
