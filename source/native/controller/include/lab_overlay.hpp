// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_control.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <memory>
#include <span>
namespace lab {
// One admitted HWND/swapchain. No injection service, game menu automation, or
// NR execution is performed by this renderer. Controls call Controller only.
class GameOverlay final {
    struct Impl;std::unique_ptr<Impl> p_;
public:
    // data_root: the installation's recorded data root (host contract V4); the
    // preferences file must sit in one of its direct subdirectories.
    explicit GameOverlay(Controller&,std::filesystem::path preferences={},std::filesystem::path data_root={});
    ~GameOverlay();
    // Holds the chain's identity and the queue only: the current back buffer is
    // acquired per present and released after recording, so no reference of
    // ours keeps the game's chain alive (DXGI admits one flip chain per HWND).
    void attach(IDXGISwapChain3*,ID3D12CommandQueue*);
    // Unbind from the chain: hide, wait for our own submitted work, release the
    // GPU resources. A later attach() binds anew (late path: another chain, or
    // the same chain on a re-decided queue). False when our completion is
    // unknown; the resources are then kept and attach() is refused.
    bool detach() noexcept;
    // Bound to a chain by attach() and not detached or stopped since.
    bool bound() const noexcept;
    bool visible() const noexcept;
    void color_space(IDXGISwapChain*,DXGI_COLOR_SPACE_TYPE) noexcept;
    // Returns this submission's id, or zero if no work was submitted. The late
    // path requires a per-call permit and checks its queue under the draw lock.
    std::uint64_t present(IDXGISwapChain*,bool permitted=true,ID3D12CommandQueue* expected_queue=nullptr) noexcept;
    // In late batch mode Present polls input only; drawing occurs atomically
    // with the game's own named back-buffer submission, preserving its Execute boundary. No swapchain is touched
    // from the Execute hook and no back-buffer reference survives the call.
    void use_batch_submission(bool enabled)noexcept;
    void submit_batch(ID3D12CommandQueue*,ID3D12Resource*,const std::function<void(ID3D12CommandList*)>&)noexcept;
    // Original Present's result for that submission. S_OK is not display proof.
    void presented(std::uint64_t submission,HRESULT result,UINT flags) noexcept;
    bool presentation_source(IDXGISwapChain*,ID3D12CommandQueue**,DXGI_COLOR_SPACE_TYPE*) noexcept;
    bool before_resize(IDXGISwapChain*) noexcept;
    // After successful ResizeBuffers1 only. Unresolved/rotating queues disable
    // drawing instead of continuing to submit work on an obsolete queue.
    void resized_queues(IDXGISwapChain3*,std::span<ID3D12CommandQueue* const>) noexcept;
    void resized_default_queue(IDXGISwapChain3*) noexcept;
    void stop() noexcept;
    // Open the panel as if the hotkey had been pressed. For the root proxy's
    // on-Insert start: that press already happened, before the host existed.
    void show() noexcept;
    json snapshot() const;
};
}
