// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// NGX admission, driven through the REAL evaluate detour against a fake loader
// in this executable (NgxObserver::attach_entries_for_test). WARP resources, no
// NGX, no game, no NR model.
//
// Why it exists (Hellblade 2, frame generation on): the evaluate
// hook receives every NGX feature, and DLSS frame generation (feature 11, on a
// compute list, measured in an NR fault record) was offered to the
// runtime as a named skip. Each one reset NR history and took a call/frame
// number; one landing from another thread while an SR frame sat between its
// recording and its return fell through the runtime's skip rule and ended NR:
// "Live RR rejected while ON: ngx-profile / Not a DLSS super-resolution or
// ray-reconstruction feature". Non-candidates are now forwarded untouched.
//
// The receiver below models the runtime's sequencing rules -- the ones that
// fault was made of -- not the runtime itself:
//   * LabNrLiveReject (runtime/src/nr_live.cpp): a skipped_before_insertion
//     rejection is a skip only when its call is newer than the last one and no
//     frame is between its recording and its matched return; else NR fails;
//   * enter_owned (runtime/src/nr_live_core.hpp): a frame needs a newer call
//     ("Repeated live RR call"), and a frame number that is not last+1 is a
//     history gap; the frame stays "at the boundary" until boundary_returned;
//   * LabNrLiveGameBindingEntryV1: with binding preservation on, a second
//     entry before the first exit is "Overlapping game binding entry"; with it
//     off -- every NGX package today, Hellblade 2 included -- entry and exit do
//     nothing.
#include "lab_ngx_observer.hpp"
#include "lab_ngx_frame.hpp"
#include "lab_rejected_call.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <wrl/client.h>
#include <atomic>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
int checks = 0;
void need(bool ok, const std::string& why) {
    ++checks;
    if (!ok) throw std::runtime_error(why);
}
void hr(HRESULT r, const char* what) {
    if (FAILED(r)) throw std::runtime_error(std::string(what) + " failed");
}

// ---- the fake loader. Distinct, non-inlined bodies so each has its own
// address for MinHook (the target links /OPT:NOICF), called through volatile
// pointers so no call site bypasses the hook.
std::atomic<unsigned> loader_creates{0}, loader_evaluates{0}, loader_releases{0};
std::atomic<std::uintptr_t> next_handle{0x10000};
// An Evaluate on this handle parks inside the "loader" until released, so a
// test can hold one call inside NGX while another thread runs whole frames.
std::atomic<const NVSDK_NGX_Handle*> park_handle{nullptr};
HANDLE parked = nullptr, unpark = nullptr;
std::atomic<const NVSDK_NGX_Handle*> fail_handle{nullptr};

__declspec(noinline) NVSDK_NGX_Result NVSDK_CONV fake_create(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                             NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** out) {
    loader_creates.fetch_add(1);
    if (!out) return NVSDK_NGX_Result_Fail;
    *out = reinterpret_cast<NVSDK_NGX_Handle*>(next_handle.fetch_add(0x40));
    return NVSDK_NGX_Result_Success;
}
__declspec(noinline) NVSDK_NGX_Result NVSDK_CONV fake_evaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle* handle,
                                                               const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
    loader_evaluates.fetch_add(1);
    if (handle && handle == park_handle.load()) {
        SetEvent(parked);
        if (WaitForSingleObject(unpark, 10000) != WAIT_OBJECT_0) return NVSDK_NGX_Result_Fail;
    }
    return handle && handle == fail_handle.load() ? NVSDK_NGX_Result_Fail : NVSDK_NGX_Result_Success;
}
__declspec(noinline) NVSDK_NGX_Result NVSDK_CONV fake_release(NVSDK_NGX_Handle*) {
    loader_releases.fetch_add(1);
    return NVSDK_NGX_Result_Success;
}
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                                 PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

// ---- a parameter block, typed per key the way the observer reads them.
class Parameters final : public NVSDK_NGX_Parameter {
    std::map<std::string, unsigned> uints_;
    std::map<std::string, int> ints_;
    std::map<std::string, float> floats_;
    std::map<std::string, ID3D12Resource*> resources_;
    template <class Map, class T>
    static NVSDK_NGX_Result find(const Map& map, const char* name, T* out) {
        const auto it = map.find(name);
        if (it == map.end() || !out) return NVSDK_NGX_Result_Fail;
        *out = static_cast<T>(it->second);
        return NVSDK_NGX_Result_Success;
    }
public:
    void Set(const char* n, unsigned long long v) override { uints_[n] = static_cast<unsigned>(v); }
    void Set(const char* n, float v) override { floats_[n] = v; }
    void Set(const char* n, double v) override { floats_[n] = static_cast<float>(v); }
    void Set(const char* n, unsigned int v) override { uints_[n] = v; }
    void Set(const char* n, int v) override { ints_[n] = v; }
    void Set(const char*, ID3D11Resource*) override {}
    void Set(const char* n, ID3D12Resource* v) override { resources_[n] = v; }
    void Set(const char*, void*) override {}
    NVSDK_NGX_Result Get(const char* n, unsigned long long* v) const override { return find(uints_, n, v); }
    NVSDK_NGX_Result Get(const char* n, float* v) const override { return find(floats_, n, v); }
    NVSDK_NGX_Result Get(const char* n, double* v) const override { return find(floats_, n, v); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* v) const override { return find(uints_, n, v); }
    NVSDK_NGX_Result Get(const char* n, int* v) const override { return find(ints_, n, v); }
    NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** v) const override { return find(resources_, n, v); }
    NVSDK_NGX_Result Get(const char*, void**) const override { return NVSDK_NGX_Result_Fail; }
    void Reset() override { uints_.clear(); ints_.clear(); floats_.clear(); resources_.clear(); }
    void erase(const char* n) { uints_.erase(n); ints_.erase(n); floats_.erase(n); resources_.erase(n); }
};

// ---- the runtime's sequencing rules (see the header comment).
struct RuntimeModel final : lab::live::IFrameReceiver {
    std::mutex mutex;
    std::uint64_t last_call = 0, last_frame = 0, boundary_call = 0, binding_call = 0;
    bool failed = false, binding_preservation = false;
    std::string failure;
    unsigned frames = 0, skipped = 0, history_gaps = 0, binding_entries = 0, binding_exits = 0, aborted_calls = 0;
    std::vector<std::uint64_t> frame_calls;
    lab::RejectedCall last_rejection{};
    // Runs on the receiving thread after frame() has put the frame at the
    // boundary and BEFORE boundary_returned(): the window that ended NR.
    std::function<void()> inside_frame;

    void fail(const std::string& why) { if (!failed) { failed = true; failure = why; } }
    void frame(const lab::live::Frame& f) noexcept override {
        std::function<void()> hook;
        {
            std::lock_guard lock(mutex);
            if (failed) return;
            if (f.call <= last_call) { fail("Repeated live RR call"); return; }
            if (last_frame && f.frame != last_frame + 1) ++history_gaps;
            last_call = f.call; last_frame = f.frame; boundary_call = f.call;
            ++frames; frame_calls.push_back(f.call);
            hook = std::move(inside_frame); inside_frame = nullptr;
        }
        if (hook) hook();
    }
    void boundary_returned(std::uint64_t call, bool) noexcept override {
        std::lock_guard lock(mutex);
        if (call == boundary_call) boundary_call = 0;
    }
    void aborted(std::uint64_t call) noexcept override {
        { std::lock_guard lock(mutex); ++aborted_calls; }
        boundary_returned(call, false);
    }
    void rejected(const lab::RejectedCall& why) noexcept override {
        std::lock_guard lock(mutex);
        if (failed) return;
        last_rejection = why;
        if (why.disposition == lab::RejectedDisposition::skipped_before_insertion && why.call > last_call && !boundary_call) {
            ++skipped; last_call = why.call;
            if (why.frame > last_frame) last_frame = why.frame;
            return;
        }
        fail(std::string("Live RR rejected while ON: ") + why.stage + " / " + why.reason);
    }
    void game_binding_enter(std::uint64_t call, std::uint64_t, void*) noexcept override {
        std::lock_guard lock(mutex);
        ++binding_entries;
        if (binding_preservation && binding_call) fail("Overlapping game binding entry");
        binding_call = call;
    }
    void game_binding_exit(std::uint64_t call) noexcept override {
        std::lock_guard lock(mutex);
        ++binding_exits;
        if (binding_call == call) binding_call = 0;
    }
};

struct Warp {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator, compute_allocator;
    ComPtr<ID3D12GraphicsCommandList> list, compute;
    Warp() {
        ComPtr<IDXGIFactory4> factory;
        hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter> adapter;
        hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
        hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&compute_allocator)), "compute allocator");
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, compute_allocator.Get(), nullptr, IID_PPV_ARGS(&compute)), "compute list");
    }
    ComPtr<ID3D12Resource> texture(unsigned w, unsigned h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = format; d.SampleDesc.Count = 1; d.Flags = flags;
        ComPtr<ID3D12Resource> r;
        hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)), "texture");
        return r;
    }
};

NVSDK_NGX_Handle* create(NVSDK_NGX_Feature feature, ID3D12GraphicsCommandList* list, Parameters& p) {
    CreateFn volatile entry = &fake_create;
    NVSDK_NGX_Handle* handle = nullptr;
    need(entry(list, feature, &p, &handle) == NVSDK_NGX_Result_Success && handle, "fake create through the hook");
    return handle;
}
NVSDK_NGX_Result evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const Parameters& p) {
    EvaluateFn volatile entry = &fake_evaluate;
    return entry(list, handle, &p, nullptr);
}
void release(NVSDK_NGX_Handle* handle) {
    ReleaseFn volatile entry = &fake_release;
    need(entry(handle) == NVSDK_NGX_Result_Success, "fake release through the hook");
}
const lab::json* feature_record(const lab::json& report, const NVSDK_NGX_Handle* handle) {
    for (const auto& f : report.at("features"))
        if (f.at("handle") == reinterpret_cast<std::uint64_t>(handle)) return &f;
    return nullptr;
}
std::uint64_t admission(const lab::NgxObserver& o, const char* key) { return o.admission().at(key).get<std::uint64_t>(); }
std::uint64_t ignored_fg(const lab::NgxObserver& o) {
    const auto by = o.admission().at("ignored_by_feature");
    return by.contains("11") ? by.at("11").get<std::uint64_t>() : 0;
}
}

int main() try {
    parked = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    unpark = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    need(parked && unpark, "events");
    Warp gpu;
    // Halo's measured shape at WARP size (ngx_frame_tests.cpp): output 2x the guides.
    const unsigned ow = 512, oh = 216, gw = 256, gh = 108;
    auto output = gpu.texture(ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto depth = gpu.texture(gw, gh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    auto motion = gpu.texture(gw, gh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    // Frame generation's own images (DLSSG.* keys only; no Output).
    auto backbuffer = gpu.texture(ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    auto interpolated = gpu.texture(ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    lab::NgxObserver observer;
    need(observer.attach_entries_for_test(reinterpret_cast<void*>(&fake_create), reinterpret_cast<void*>(&fake_evaluate),
                                          reinterpret_cast<void*>(&fake_release)),
         "the real detours install on the fake loader");
    need(observer.attached() && !observer.refused(), "attached, not refused");
    need(!observer.attach_entries_for_test(reinterpret_cast<void*>(&fake_create), reinterpret_cast<void*>(&fake_evaluate),
                                           reinterpret_cast<void*>(&fake_release)),
         "a second install is refused, not stacked");

    // ---- creates, observed: SR (Halo flags 0x43) and frame generation.
    Parameters sr_create;
    sr_create.Set(NVSDK_NGX_Parameter_Width, gw); sr_create.Set(NVSDK_NGX_Parameter_Height, gh);
    sr_create.Set(NVSDK_NGX_Parameter_OutWidth, ow); sr_create.Set(NVSDK_NGX_Parameter_OutHeight, oh);
    sr_create.Set(NVSDK_NGX_Parameter_PerfQualityValue, 0u);
    sr_create.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 0x43u);
    auto* sr = create(NVSDK_NGX_Feature_SuperSampling, gpu.list.Get(), sr_create);
    Parameters fg_create;
    auto* fg = create(NVSDK_NGX_Feature_FrameGeneration, gpu.list.Get(), fg_create);

    Parameters sr_eval;
    sr_eval.Set(NVSDK_NGX_Parameter_Output, output.Get());
    sr_eval.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    sr_eval.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    sr_eval.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f); sr_eval.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    sr_eval.Set(NVSDK_NGX_Parameter_Reset, 0);
    sr_eval.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, static_cast<int>(gw));
    sr_eval.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, static_cast<int>(gh));
    Parameters fg_eval;
    fg_eval.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
    fg_eval.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, motion.Get());
    fg_eval.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
    fg_eval.Set(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, interpolated.Get());

    // ---- with no receiver nothing is offered and nothing is "ignored": the
    // admission counters only mean something while admission is on.
    need(evaluate(gpu.compute.Get(), fg, fg_eval) == NVSDK_NGX_Result_Success, "observation-only FG forwards");
    need(admission(observer, "offered") == 0 && admission(observer, "ignored_other_features") == 0,
         "observation only: nothing offered, nothing counted as ignored");

    RuntimeModel runtime;
    observer.set_receiver(&runtime);

    // ---- 1. steady state: every frame one SR Evaluate and, as under MFG, up to
    // three frame-generation Evaluates on a compute list. Binding preservation
    // ON here, as a package that enables it would run: a candidate whose
    // success path left its scope open fails the next frame.
    runtime.binding_preservation = true;
    unsigned fg_calls = 0;
    for (unsigned i = 0; i < 12; ++i) {
        need(evaluate(gpu.list.Get(), sr, sr_eval) == NVSDK_NGX_Result_Success, "SR forwards");
        for (unsigned g = 0; g <= i % 3; ++g, ++fg_calls)
            need(evaluate(gpu.compute.Get(), fg, fg_eval) == NVSDK_NGX_Result_Success, "FG forwards");
    }
    {
        std::lock_guard lock(runtime.mutex);
        need(!runtime.failed, "NR stays on with frame generation running: " + runtime.failure);
        need(runtime.frames == 12, "every SR call became a frame");
        need(runtime.skipped == 0, "frame generation adds no skip");
        need(runtime.history_gaps == 0, "frame generation leaves no history gap");
        bool consecutive = runtime.frame_calls.size() == 12;
        for (std::size_t i = 0; consecutive && i < runtime.frame_calls.size(); ++i) consecutive = runtime.frame_calls[i] == i + 1;
        need(consecutive, "call/frame numbers advance with candidate calls only (1..12, no holes)");
        need(runtime.binding_entries == 12 && runtime.binding_exits == 12 && !runtime.binding_call,
             "one binding scope per candidate, closed on the success path too");
    }
    need(admission(observer, "offered") == 12 && admission(observer, "admitted") == 12 && admission(observer, "skipped") == 0,
         "only SR was offered, and all of it admitted");
    need(admission(observer, "ignored_other_features") == fg_calls && ignored_fg(observer) == fg_calls,
         "frame generation is counted, under feature 11, and nowhere else");
    need(observer.blocked() == nullptr, "frame generation does not repaint the blocked reason");
    need(observer.report().at("admission") == observer.admission(), "report().admission and admission() are one record");

    // ---- 2. the window that ended NR: frame generation arrives from ANOTHER
    // thread while the SR frame is recorded but not yet returned. Hellblade 2's
    // package runs without binding preservation; offered as a skip, the call
    // ends the model with the game's own message ("Live RR rejected while ON:
    // ngx-profile / Not a DLSS super-resolution or ray-reconstruction feature").
    runtime.binding_preservation = false;
    {
        { std::lock_guard lock(runtime.mutex);
          runtime.inside_frame = [&] {
              std::thread other([&] { (void)evaluate(gpu.compute.Get(), fg, fg_eval); });
              other.join();
          }; }
        need(evaluate(gpu.list.Get(), sr, sr_eval) == NVSDK_NGX_Result_Success, "SR with FG inside its window");
        ++fg_calls;
        std::lock_guard lock(runtime.mutex);
        need(!runtime.failed, "FG inside an SR frame's window no longer ends NR: " + runtime.failure);
        need(runtime.frames == 13 && runtime.skipped == 0 && runtime.history_gaps == 0, "the SR frame landed; no skip, no gap");
    }

    // ---- 3. frame generation enters NGX first and returns after a whole SR
    // frame: its call number would be older than the frame's.
    {
        park_handle = fg;
        std::thread other([&] { (void)evaluate(gpu.compute.Get(), fg, fg_eval); });
        need(WaitForSingleObject(parked, 10000) == WAIT_OBJECT_0, "FG parked inside the loader");
        need(evaluate(gpu.list.Get(), sr, sr_eval) == NVSDK_NGX_Result_Success, "SR while FG is inside NGX");
        SetEvent(unpark);
        other.join();
        park_handle = nullptr;
        ++fg_calls;
        std::lock_guard lock(runtime.mutex);
        need(!runtime.failed, "an FG call returning after a newer SR frame no longer ends NR: " + runtime.failure);
        need(runtime.frames == 14 && runtime.skipped == 0 && runtime.frame_calls.back() == 14, "SR frame 14, consecutive");
    }
    need(admission(observer, "ignored_other_features") == fg_calls, "every FG call counted once");

    // ---- 4. a feature whose create we never saw (late attach) and whose call
    // has no Output cannot become a frame: not offered, counted on its own.
    {
        const auto* unseen = reinterpret_cast<const NVSDK_NGX_Handle*>(0x7f000);
        need(evaluate(gpu.compute.Get(), unseen, fg_eval) == NVSDK_NGX_Result_Success, "unseen FG forwards");
        need(admission(observer, "ignored_unobserved_without_output") == 1, "unobserved without Output: counted");
        need(admission(observer, "offered") == 14, "...and never offered");
        std::lock_guard lock(runtime.mutex);
        need(!runtime.failed && runtime.skipped == 0, "no skip for it");
    }

    // ---- 5. real candidates are still refused BY NAME, as skips that keep ON.
    {
        // Unobserved create WITH an Output: the hint to change a quality setting.
        const auto* late_sr = reinterpret_cast<const NVSDK_NGX_Handle*>(0x7f400);
        need(evaluate(gpu.list.Get(), late_sr, sr_eval) == NVSDK_NGX_Result_Success, "late SR forwards");
        {
            std::lock_guard lock(runtime.mutex);
            need(!runtime.failed && runtime.skipped == 1, "late SR is a named skip");
            need(runtime.last_rejection.disposition == lab::RejectedDisposition::skipped_before_insertion &&
                 std::string(runtime.last_rejection.stage) == "ngx-profile" &&
                 std::string(runtime.last_rejection.reason).find("change any DLSS quality setting") != std::string::npos,
                 "the late-attach refusal still names its remedy");
            need(runtime.last_rejection.call == 15, "a candidate refusal takes the next call number");
        }
        need(observer.blocked() && std::string(observer.blocked()).find("quality setting") != std::string::npos,
             "blocked() names the candidate's refusal");
        // An observed SR call the translation refuses: motion-vector scale gone.
        sr_eval.erase(NVSDK_NGX_Parameter_MV_Scale_X);
        need(evaluate(gpu.list.Get(), sr, sr_eval) == NVSDK_NGX_Result_Success, "SR without MV scale forwards");
        // Frame generation between two refused candidates changes nothing.
        need(evaluate(gpu.compute.Get(), fg, fg_eval) == NVSDK_NGX_Result_Success, "FG forwards");
        ++fg_calls;
        need(observer.blocked() && std::string(observer.blocked()).find("scale") != std::string::npos,
             "FG did not overwrite the candidate's blocked reason");
        sr_eval.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
        need(evaluate(gpu.list.Get(), sr, sr_eval) == NVSDK_NGX_Result_Success, "SR again");
        std::lock_guard lock(runtime.mutex);
        need(!runtime.failed && runtime.skipped == 2, "the scale refusal is a skip, NR still on: " + runtime.failure);
        need(std::string(runtime.last_rejection.reason).find("scale") != std::string::npos, "named: scale");
        need(runtime.frames == 15 && runtime.frame_calls.back() == 17 && runtime.history_gaps == 0,
             "frames resume on the next number; skips advanced the sequence, FG did not");
    }
    need(observer.blocked() == nullptr, "an admitted frame clears blocked()");
    need(admission(observer, "skipped") == 2 && admission(observer, "offered") == 17 &&
         admission(observer, "ignored_other_features") == fg_calls,
         "counters: two named skips, seventeen offers, FG apart");

    // ---- 6. a failed candidate call still closes its binding scope.
    {
        fail_handle = sr;
        need(evaluate(gpu.list.Get(), sr, sr_eval) == NVSDK_NGX_Result_Fail, "the loader's failure passes through");
        fail_handle = nullptr;
        std::lock_guard lock(runtime.mutex);
        need(runtime.aborted_calls == 1 && runtime.binding_entries == runtime.binding_exits && !runtime.binding_call,
             "failure path: aborted, binding scope closed");
    }
    need(admission(observer, "aborted") == 1, "the abort is counted");

    // ---- 7. the record of both features survives in the report.
    {
        const auto report = observer.report();
        bool sr_seen = false, fg_seen = false;
        for (const auto& f : report.at("features")) {
            if (f.at("feature_id") == lab::ngx::super_sampling) sr_seen = true;
            if (f.at("feature_id") == lab::ngx::frame_generation) fg_seen = f.at("command_list") == "compute";
        }
        need(sr_seen && fg_seen, "SR and FG are both still observed (FG on its compute list)");
    }

    // ---- 8. the other lifecycle paths, on the RR route: Halo's shape, NGX-direct
    // RR beside Streamline frame generation. Release goes through the REAL
    // release detour; frame generation is RE-created (a fault record measured two FG
    // creates, the first released without an evaluate) and created BEFORE the RR
    // feature. None of it changes what is a candidate.
    {
        release(sr);
        release(fg);
        Parameters fg2_create;
        auto* fg2 = create(NVSDK_NGX_Feature_FrameGeneration, gpu.list.Get(), fg2_create);  // before RR
        auto* rr = create(NVSDK_NGX_Feature_RayReconstruction, gpu.list.Get(), sr_create);
        const auto offered_before = admission(observer, "offered"), admitted_before = admission(observer, "admitted");
        const auto ignored_before = admission(observer, "ignored_other_features");
        unsigned frames_before = 0, skipped_before = 0, gaps_before = 0;
        std::uint64_t last_before = 0;
        {
            std::lock_guard lock(runtime.mutex);
            frames_before = runtime.frames; skipped_before = runtime.skipped; gaps_before = runtime.history_gaps;
            last_before = runtime.last_call;
        }
        for (unsigned i = 0; i < 6; ++i) {
            need(evaluate(gpu.list.Get(), rr, sr_eval) == NVSDK_NGX_Result_Success, "RR forwards");
            for (unsigned g = 0; g < 2; ++g, ++fg_calls)
                need(evaluate(gpu.compute.Get(), fg2, fg_eval) == NVSDK_NGX_Result_Success, "re-created FG forwards");
        }
        {
            std::lock_guard lock(runtime.mutex);
            need(!runtime.failed, "RR with frame generation keeps NR on: " + runtime.failure);
            need(runtime.frames == frames_before + 6 && runtime.skipped == skipped_before, "every RR call became a frame, no skip");
            // Call 18 was the aborted SR of scenario 6: that one gap is the abort's,
            // and frame generation adds none.
            need(runtime.history_gaps == gaps_before + 1, "one history gap, from the scenario-6 abort, none from FG");
            bool consecutive = true;
            for (unsigned i = 0; i < 6; ++i)
                consecutive = consecutive && runtime.frame_calls[frames_before + i] == last_before + 2 + i;
            need(consecutive, "RR frames take consecutive numbers right after the aborted call");
        }
        need(admission(observer, "offered") == offered_before + 6 && admission(observer, "admitted") == admitted_before + 6,
             "only RR was offered, all admitted");
        need(admission(observer, "ignored_other_features") == ignored_before + 12 && ignored_fg(observer) == fg_calls,
             "the re-created FG feature is counted under 11 like the first");
        need(observer.blocked() == nullptr, "nothing blocked");
        need(loader_releases.load() == 2, "both releases reached the loader through the hook");
        const auto report = observer.report();
        need(report.at("releases") == 2, "the observer counted both releases");
        const auto* sr_record = feature_record(report, sr);
        const auto* fg_record = feature_record(report, fg);
        const auto* fg2_record = feature_record(report, fg2);
        const auto* rr_record = feature_record(report, rr);
        need(sr_record && sr_record->at("released") == true && fg_record && fg_record->at("released") == true,
             "the released SR and FG records say so");
        need(fg2_record && fg2_record->at("feature_id") == lab::ngx::frame_generation && fg2_record->at("evaluates") == 12 &&
             fg2_record->at("command_list") == "compute",
             "the re-created FG feature is observed: id 11, twelve evaluates, compute");
        need(rr_record && rr_record->at("feature_id") == lab::ngx::ray_reconstruction && rr_record->at("evaluates") == 6 &&
             rr_record->at("released") == false,
             "the RR feature is observed: id 13, six evaluates, alive");
    }
    observer.set_receiver(nullptr);
    std::cout << "PASS " << checks << " NGX admission checks through the real detours on a fake loader; WARP, no NGX, no game, no NR\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL after " << checks << " checks: " << e.what() << "\n";
    return 1;
}
