// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#ifndef NGX_SNIPPET_BUILD
#error This session uses the verified snippet ABI, not the identically named Core ABI.
#endif
#include <nvsdk_ngx.h>
#include <windows.h>
#include <stdexcept>
#include <thread>
#include <cstdint>
#include "lab_serial_access.hpp"

namespace lab::nr {
// Borrowed verified module/device/parameters. No game loading or implicit cleanup.
// Compile production callers inside the lab-prefixed nvngx research bridge.
struct Api {
    decltype(&NVSDK_NGX_D3D12_Init_Ext) init = nullptr;
    decltype(&NVSDK_NGX_D3D12_CreateFeature) create = nullptr;
    decltype(&NVSDK_NGX_D3D12_EvaluateFeature) evaluate = nullptr;
    decltype(&NVSDK_NGX_D3D12_ReleaseFeature) release = nullptr;
    decltype(&NVSDK_NGX_D3D12_Shutdown1) shutdown = nullptr;
    static Api from_verified_module(HMODULE module) {
        if (!module) throw std::invalid_argument("Missing verified NR module");
        Api api;
#define LAB_NR_EXPORT(member, name) api.member = reinterpret_cast<decltype(api.member)>(GetProcAddress(module, name))
        LAB_NR_EXPORT(init, "NVSDK_NGX_D3D12_Init_Ext");
        LAB_NR_EXPORT(create, "NVSDK_NGX_D3D12_CreateFeature");
        LAB_NR_EXPORT(evaluate, "NVSDK_NGX_D3D12_EvaluateFeature");
        LAB_NR_EXPORT(release, "NVSDK_NGX_D3D12_ReleaseFeature");
        LAB_NR_EXPORT(shutdown, "NVSDK_NGX_D3D12_Shutdown1");
#undef LAB_NR_EXPORT
        return api;
    }
};

class Session final {
    Api api_;
    ThreadAccess access_;
    ID3D12Device* device_ = nullptr;
    NVSDK_NGX_Handle* feature_ = nullptr;
    bool init_attempted_ = false, initialized_ = false, pending_ = false;
    bool operation_failed_ = false, abandoned_ = false;
    std::uint64_t submitted_boundaries_ = 0, successful_evaluates_ = 0, discarded_recordings_ = 0;
    void owner() const {
        access_.require();
    }
    void callable() const {
        owner();
        if (abandoned_ || pending_) throw std::logic_error("NR work completion unknown; no further NGX calls allowed");
    }
public:
    explicit Session(Api api,SerialCallGate* gate=nullptr) : api_(api),access_(gate) {
        if (!api.init || !api.create || !api.evaluate || !api.release || !api.shutdown)
            throw std::invalid_argument("Incomplete verified NR export table");
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Destruction deliberately makes NO NGX call and unloads NO module. The
    // adapter must explicitly finish or isolate/retain an incomplete context.
    ~Session() = default;
    // Immutable configuration only; not a state snapshot or an admission lease.
    SerialCallGate* access_gate() const noexcept {return access_.gate();}

    NVSDK_NGX_Result initialize(ID3D12Device* device, const wchar_t* data_path, const NVSDK_NGX_Parameter* parameters) {
        callable();
        if (init_attempted_ || !device || !data_path || !parameters) throw std::logic_error("Invalid/repeated NR initialization");
        init_attempted_ = true; device_ = device;
        auto result = api_.init(0, data_path, device, NVSDK_NGX_Version_API, parameters);
        initialized_ = result == NVSDK_NGX_Result_Success;
        operation_failed_ = !initialized_;
        return result;
    }
    NVSDK_NGX_Result create(ID3D12GraphicsCommandList* commands, const NVSDK_NGX_Parameter* parameters) {
        callable();
        if (!initialized_ || feature_ || operation_failed_ || !commands || !parameters)
            throw std::logic_error("NR Create state/arguments invalid");
        pending_ = true; // A failed API may still have recorded work.
        auto result = api_.create(commands, static_cast<NVSDK_NGX_Feature>(18), parameters, &feature_);
        operation_failed_ = result != NVSDK_NGX_Result_Success || !feature_;
        return result;
    }
    NVSDK_NGX_Result evaluate(ID3D12GraphicsCommandList* commands, const NVSDK_NGX_Parameter* parameters) {
        callable();
        if (!initialized_ || !feature_ || operation_failed_ || !commands || !parameters)
            throw std::logic_error("NR Evaluate state/arguments invalid");
        pending_ = true;
        auto result = api_.evaluate(commands, feature_, parameters, nullptr);
        operation_failed_ = result != NVSDK_NGX_Result_Success;
        if (!operation_failed_) ++successful_evaluates_;
        return result;
    }
    // Caller attestation of its own submitted command list/queue completion.
    // NOT a claim that all NR internal queues/CUDA or the game's pipeline-order
    // evidence (the p0 gate) are covered.
    void acknowledge_outer_queue_completion() {
        owner();
        if (abandoned_ || !pending_) throw std::logic_error("No pending NR boundary to acknowledge");
        pending_ = false; ++submitted_boundaries_;
    }
    // The command list that received the pending recording was Reset by its
    // owner before any submission: the driver never executed it. NOT a
    // completion acknowledgement; only the pending flag clears so the next
    // Evaluate may record (with a history reset requested by the caller).
    void discard_pending_recording() {
        owner();
        if (abandoned_ || !pending_) throw std::logic_error("No pending NR recording to discard");
        pending_ = false; ++discarded_recordings_;
    }
    NVSDK_NGX_Result release() {
        callable();
        if (!initialized_ || !feature_) throw std::logic_error("No NR Feature to release");
        abandoned_ = true; // Also prevents retry if a foreign call throws.
        auto result = api_.release(feature_);
        feature_ = nullptr; // Never retry an uncertain Release with the same handle.
        abandoned_ = result != NVSDK_NGX_Result_Success;
        return result;
    }
    NVSDK_NGX_Result shutdown() {
        callable();
        if (!initialized_ || feature_) throw std::logic_error("NR shutdown requires released Feature");
        abandoned_ = true;
        auto result = api_.shutdown(device_);
        initialized_ = false;
        abandoned_ = result != NVSDK_NGX_Result_Success;
        return result;
    }
    void abandon() { owner(); abandoned_ = true; }
    bool pending() const { owner(); return pending_; }
    bool initialized() const { owner(); return initialized_; }
    bool has_feature() const { owner(); return feature_ != nullptr; }
    bool abandoned() const { owner(); return abandoned_; }
    bool operation_failed() const { owner(); return operation_failed_; }
    std::uint64_t outer_queue_completions() const { owner(); return submitted_boundaries_; }
    std::uint64_t successful_evaluates() const { owner(); return successful_evaluates_; }
    std::uint64_t discarded_recordings() const { owner(); return discarded_recordings_; }
};
}
