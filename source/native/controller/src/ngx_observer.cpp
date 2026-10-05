// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_ngx_observer.hpp"
#include "lab_hook_bank.hpp"
#include "lab_ngx_frame.hpp"
#include "lab_rejected_call.hpp"
#include <cstring>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>

namespace lab {
namespace {

// The keys NVIDIA's own ray-reconstruction evaluate helper populates, taken
// from the SDK header macros rather than retyped, so a spelling cannot drift.
// Presence of a bit means the game handed us that key on the observed call.
// ABSENCE MEANS NOTHING BEYOND THAT: it is not evidence the key is unsupported,
// unused by the feature, or unavailable in the engine. This is our vocabulary,
// not the game's, and a key the game sets that is missing here is invisible.
constexpr const char* guide_keys[]{
    NVSDK_NGX_Parameter_Color,
    NVSDK_NGX_Parameter_Output,
    NVSDK_NGX_Parameter_Depth,
    NVSDK_NGX_Parameter_MotionVectors,
    NVSDK_NGX_Parameter_TransparencyMask,
    NVSDK_NGX_Parameter_ExposureTexture,
    NVSDK_NGX_Parameter_DiffuseAlbedo,
    NVSDK_NGX_Parameter_SpecularAlbedo,
    NVSDK_NGX_Parameter_GBuffer_Normals,
    NVSDK_NGX_Parameter_GBuffer_Roughness,
    NVSDK_NGX_Parameter_GBuffer_Emissive,
    NVSDK_NGX_Parameter_GBuffer_Albedo,
    NVSDK_NGX_Parameter_GBuffer_Specular,
    NVSDK_NGX_Parameter_GBuffer_Metallic,
    NVSDK_NGX_Parameter_GBuffer_Subsurface,
    NVSDK_NGX_Parameter_GBuffer_ShadingModelId,
    NVSDK_NGX_Parameter_GBuffer_MaterialId,
    NVSDK_NGX_Parameter_GBuffer_SpecularMvec,
    NVSDK_NGX_Parameter_MotionVectors3D,
    NVSDK_NGX_Parameter_DepthHighRes,
    NVSDK_NGX_Parameter_Position_ViewSpace,
    NVSDK_NGX_Parameter_RayTracingHitDistance,
    NVSDK_NGX_Parameter_IsParticleMask,
    NVSDK_NGX_Parameter_AnimatedTextureMask,
    NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
    NVSDK_NGX_Parameter_DLSS_DisocclusionMask,
    NVSDK_NGX_Parameter_DLSS_TransparencyLayer,
    NVSDK_NGX_Parameter_DLSS_TransparencyLayerMvecs,
    NVSDK_NGX_Parameter_DLSS_TransparencyLayerOpacity,
    NVSDK_NGX_Parameter_DLSSD_ReflectedAlbedo,
    NVSDK_NGX_Parameter_DLSSD_ResponsivityMask,
    NVSDK_NGX_Parameter_DLSSD_Alpha,
    NVSDK_NGX_Parameter_DLSSD_OutputAlpha,
    NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance,
    NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance,
    NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirection,
    NVSDK_NGX_Parameter_DLSSD_SpecularRayDirection,
    NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirectionHitDistance,
    NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance,
    NVSDK_NGX_Parameter_DLSSD_ScreenSpaceSubsurfaceScatteringGuide,
    NVSDK_NGX_Parameter_DLSSD_ScreenSpaceRefractionGuide,
    NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide,
    NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles,
    NVSDK_NGX_Parameter_DLSSD_ColorAfterParticles,
    NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency,
    NVSDK_NGX_Parameter_DLSSD_ColorAfterTransparency,
    NVSDK_NGX_Parameter_DLSSD_ColorBeforeFog,
    NVSDK_NGX_Parameter_DLSSD_ColorAfterFog,
    NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceSubsurfaceScattering,
    NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceSubsurfaceScattering,
    NVSDK_NGX_Parameter_DLSSD_ColorBeforeScreenSpaceRefraction,
    NVSDK_NGX_Parameter_DLSSD_ColorAfterScreenSpaceRefraction,
    NVSDK_NGX_Parameter_DLSSD_ColorBeforeDepthOfField,
    NVSDK_NGX_Parameter_DLSSD_ColorAfterDepthOfField,
};
constexpr unsigned guide_count = static_cast<unsigned>(std::size(guide_keys));
static_assert(guide_count <= 64, "The presence mask is a single 64-bit word");

// Scalars read alongside a create, so the design step knows the extents and the
// quality tier the game asked for rather than inferring them from the output.
constexpr const char* create_scalars[]{
    NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
    NVSDK_NGX_Parameter_OutWidth, NVSDK_NGX_Parameter_OutHeight,
    NVSDK_NGX_Parameter_PerfQualityValue,
    NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
};
// One preset key PER QUALITY MODE. Reading a single slot was wrong: the first
// run read the DLAA slot while the game ran MaxPerf and got 0x80000002, which is
// not a preset. All six are read at create and reported raw; which one the
// feature uses follows from perf_quality, and a value outside the preset enum
// is reported as read, never reinterpreted.
constexpr const char* preset_keys[]{
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality,
};
constexpr const char* preset_modes[]{"dlaa", "quality", "balanced", "performance",
                                     "ultra_performance", "ultra_quality"};
// Per-frame scalars, read on the sampled evaluates only. Split by TYPE: the
// parameter interface overloads Get per type, and asking for an int as a float
// is not a conversion request, it is a different overload that can simply miss.
constexpr const char* evaluate_ints[]{
    NVSDK_NGX_Parameter_Reset,
    NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
    NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
};
constexpr const char* evaluate_floats[]{
    NVSDK_NGX_Parameter_Sharpness,
    NVSDK_NGX_Parameter_MV_Scale_X, NVSDK_NGX_Parameter_MV_Scale_Y,
    NVSDK_NGX_Parameter_DLSS_Pre_Exposure, NVSDK_NGX_Parameter_DLSS_Exposure_Scale,
};
// The four roles whose RESOURCE DESCRIPTION the admission design actually needs.
// Knowing a key was supplied says nothing about the format and extent we would
// have to accept -- and a format assumption is precisely what crashed the
// display driver on Resident Evil Requiem. GetDesc is a pure
// query on a resource the game already owns: no GPU work, no state change.
constexpr const char* role_keys[]{
    NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Output,
    NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_MotionVectors,
};
constexpr const char* role_names[]{"color", "output", "depth", "motion"};

bool same_path(std::wstring a, std::wstring b) {
    if (a.empty() || b.empty()) return false;
    const auto fold = [](std::wstring& s) {
        for (auto& c : s) {
            if (c == L'/') c = L'\\';
            c = static_cast<wchar_t>(::towlower(c));
        }
    };
    fold(a); fold(b);
    return a == b;
}

// The driver records the directory it installed NGX into. We never derive it
// from the module list alone: a module merely NAMED like the loader is not the
// driver's loader, so the mapped path must sit inside this directory.
std::wstring registry_loader_directory() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
            0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buffer[MAX_PATH * 2]{};
    DWORD bytes = sizeof(buffer) - sizeof(wchar_t), type = 0;
    const auto status = RegQueryValueExW(key, L"FullPath", nullptr, &type,
        reinterpret_cast<LPBYTE>(buffer), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    std::wstring directory(buffer);
    while (!directory.empty() && (directory.back() == L'\\' || directory.back() == L'/'))
        directory.pop_back();
    if (directory.empty()) return {};
    // The value names the directory in every driver package seen so far; if it
    // ever names the file itself, take that file's directory instead.
    if (directory.size() > 9 && same_path(directory.substr(directory.size() - 9), L"nvngx.dll"))
        return std::filesystem::path(directory).parent_path().wstring();
    return directory;
}

// The driver ships TWO: nvngx.dll (488 KiB) is the loader an application links
// against, and _nvngx.dll (1.4 MiB) is the core the current NGX SDK maps
// directly. Halo: Campaign Evolved maps ONLY _nvngx.dll -- no nvngx.dll in its
// module list at all (observed with over two hundred modules mapped) -- so a surface that
// looks for nvngx.dll alone waits forever on a game that is using NGX the
// whole time. Both export the three feature entries; _nvngx.dll is tried first
// because it is the one that is actually mapped.
constexpr const wchar_t* loader_names[]{L"_nvngx.dll", L"nvngx.dll"};

std::wstring module_path(HMODULE module) {
    wchar_t buffer[MAX_PATH * 2]{};
    const auto length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    if (!length || length >= std::size(buffer)) return {};
    return std::wstring(buffer, length);
}

using CreateFn = decltype(&NVSDK_NGX_D3D12_CreateFeature);
using EvaluateFn = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
using ReleaseFn = decltype(&NVSDK_NGX_D3D12_ReleaseFeature);

struct Observed {
    bool used = false;
    // NVSDK_NGX_Handle is an opaque forward declaration in the public SDK, so
    // its fields cannot be read. The handle POINTER is the correlation key: it
    // is valid for that handle's lifetime, and a record is retired on release,
    // so an address the loader later reuses starts a new record instead of
    // silently continuing the old one.
    std::uint64_t handle = 0;
    std::uint32_t feature_id = 0, create_result = 0;
    unsigned width = 0, height = 0, out_width = 0, out_height = 0;
    int perf_quality = 0, create_flags = 0;
    std::array<unsigned, std::size(preset_keys)> presets{};
    std::array<bool, std::size(preset_keys)> presets_present{};
    std::uint64_t guides = 0;          // bitmask over guide_keys
    std::uint64_t evaluates = 0, failed_evaluates = 0;
    std::uint32_t last_evaluate_result = 0;
    bool released = false, sampled = false;
    // False when we attached after the game had already created this
    // feature, which is the NORMAL case for late loading: the create
    // fields below are then unknown rather than measured.
    bool create_observed = false;
    // D3D12_COMMAND_LIST_TYPE of the lists this feature was evaluated on: the
    // first one seen, and whether any later call used another. Measured because
    // the late panel once drew on "the queue RR ran on" and a UE title may run
    // RR on async compute (Halo: GPU crash on the first panel draw).
    int list_type = -1;
    bool list_type_mixed = false;
    std::array<int, std::size(evaluate_ints)> ints{};
    std::array<bool, std::size(evaluate_ints)> ints_present{};
    std::array<float, std::size(evaluate_floats)> floats{};
    std::array<bool, std::size(evaluate_floats)> floats_present{};
    struct Role {
        bool present = false;
        unsigned format = 0, width = 0, height = 0, mips = 0, samples = 0;
        unsigned dimension = 0, flags = 0;
    };
    std::array<Role, std::size(role_keys)> roles{};
};

} // namespace

struct NgxObserver::Impl {
    // One process-wide set of hooks; the detours are free functions and reach
    // the instance through this pointer. A second observer is refused rather
    // than silently sharing the first one's records.
    static inline std::atomic<Impl*> active{nullptr};

    mutable std::mutex mutex;
    HookBank<3, 1> hooks;
    HMODULE loader = nullptr;
    std::wstring registry_path, mapped_path;
    json loader_identity = nullptr;   // path, sha256, version and signature
    std::string refusal;
    bool installed = false;
    std::uint64_t creates = 0, releases = 0, evaluates = 0, overflow = 0, recycled = 0;
    std::array<Observed, 8> features{};
    // Admission (ngx-rr). The receiver is the host's live runtime client; it is
    // never deleted, so a detour that captured it may finish its sequence even
    // if the host has just set it back to null.
    std::atomic<live::IFrameReceiver*> receiver{nullptr};
    std::atomic<std::uint64_t> next_call{0};
    std::atomic<const char*> blocked{nullptr};
    std::uint64_t offered = 0, admitted = 0, skipped = 0, aborted = 0;
    std::string last_skip;
    // Evaluates that were never offered because they are not NR candidates
    // (ngx::candidacy). Counted while a receiver is set, which is the only time
    // "not offered" means anything. By feature id when the create was observed:
    // ids below 32 have their own counter, anything else shares one.
    std::uint64_t ignored_other_features = 0, ignored_unobserved_without_output = 0;
    std::array<std::uint64_t, 32> ignored_by_feature{};
    std::uint64_t ignored_feature_id_out_of_range = 0;

    json admission_locked() const;  // caller holds mutex
    // Becomes the active instance and hooks the three entries; caller holds mutex.
    bool install_locked(void* create, void* evaluate, void* release);

    Observed* slot_for(std::uint64_t handle, bool create) {
        for (auto& f : features)
            if (f.used && f.handle == handle && !f.released) return &f;
        if (!create) return nullptr;
        for (auto& f : features)
            if (!f.used) { f = Observed{}; f.used = true; f.handle = handle; return &f; }
        // All slots taken: reuse one whose feature the game has released. A
        // game that recreates its feature on every quality or resolution
        // change used to run out after eight, and every later create went
        // unobserved -- refused as "created before we attached". The report
        // then keeps the most recent eight.
        for (auto& f : features)
            if (f.used && f.released) { f = Observed{}; f.used = true; f.handle = handle; ++recycled; return &f; }
        ++overflow;
        return nullptr;
    }

    // The interface exposes no const accessors, so reading a parameter the game
    // already populated needs the cast. We call Get only; nothing is written.
    static NVSDK_NGX_Parameter* readable(const NVSDK_NGX_Parameter* parameters) noexcept {
        return const_cast<NVSDK_NGX_Parameter*>(parameters);
    }

    static std::uint64_t scan_guides(NVSDK_NGX_Parameter* parameters) noexcept {
        std::uint64_t mask = 0;
        for (unsigned i = 0; i < guide_count; ++i) {
            ID3D12Resource* resource = nullptr;
            if (parameters->Get(guide_keys[i], &resource) == NVSDK_NGX_Result_Success && resource)
                mask |= std::uint64_t{1} << i;
        }
        return mask;
    }
};

NgxObserver::NgxObserver() : impl_(std::make_unique<Impl>()) {}
NgxObserver::~NgxObserver() {
    // Installed hooks live for the process: a detour may be executing on the
    // game's render thread right now and there is no safe moment to unhook.
    // Detaching the instance pointer first makes every further detour a plain
    // forward with no recording.
    Impl* expected = impl_.get();
    Impl::active.compare_exchange_strong(expected, nullptr);
}

namespace {

NVSDK_NGX_Result NVSDK_CONV create_detour(ID3D12GraphicsCommandList* list,
        NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters,
        NVSDK_NGX_Handle** handle) {
    auto* impl = NgxObserver::Impl::active.load(std::memory_order_acquire);
    // Read what the game asked for BEFORE the call: the loader is free to
    // rewrite the parameter block while creating the feature.
    unsigned scalars[std::size(create_scalars)]{};
    bool present[std::size(create_scalars)]{};
    unsigned preset_values[std::size(preset_keys)]{};
    bool preset_present[std::size(preset_keys)]{};
    if (impl && parameters) {
        for (unsigned i = 0; i < std::size(create_scalars); ++i) {
            present[i] = parameters->Get(create_scalars[i], &scalars[i]) == NVSDK_NGX_Result_Success;
            // A game may store these as int (the flags and quality are signed
            // enums in NGX's own helpers); an unsigned-only read then reported
            // "flags absent" and the call was refused as not HDR.
            if (!present[i]) { int value = 0;
                if (parameters->Get(create_scalars[i], &value) == NVSDK_NGX_Result_Success) {
                    scalars[i] = static_cast<unsigned>(value); present[i] = true; } }
        }
        for (unsigned i = 0; i < std::size(preset_keys); ++i)
            preset_present[i] = parameters->Get(preset_keys[i], &preset_values[i]) == NVSDK_NGX_Result_Success;
    }
    const auto original = impl ? impl->hooks.original<CreateFn>(0, 0) : nullptr;
    if (!original) return NVSDK_NGX_Result_Fail;
    const auto result = original(list, feature, parameters, handle);
    if (!impl) return result;
    try {
        std::lock_guard lock(impl->mutex);
        ++impl->creates;
        const std::uint64_t id = (result == NVSDK_NGX_Result_Success && handle && *handle)
            ? reinterpret_cast<std::uint64_t>(*handle) : 0u;
        if (auto* f = impl->slot_for(id, true)) {
            f->feature_id = static_cast<std::uint32_t>(feature);
            f->create_observed = true;
            f->create_result = static_cast<std::uint32_t>(result);
            f->width = present[0] ? scalars[0] : 0;
            f->height = present[1] ? scalars[1] : 0;
            f->out_width = present[2] ? scalars[2] : 0;
            f->out_height = present[3] ? scalars[3] : 0;
            f->perf_quality = present[4] ? static_cast<int>(scalars[4]) : -1;
            f->create_flags = present[5] ? static_cast<int>(scalars[5]) : -1;
            for (unsigned i = 0; i < std::size(preset_keys); ++i) {
                f->presets[i] = preset_values[i]; f->presets_present[i] = preset_present[i];
            }
        }
    } catch (...) {}
    return result;
}

NVSDK_NGX_Result NVSDK_CONV evaluate_detour(ID3D12GraphicsCommandList* list,
        const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
        PFN_NVSDK_NGX_ProgressCallback callback) {
    auto* impl = NgxObserver::Impl::active.load(std::memory_order_acquire);
    const auto original = impl ? impl->hooks.original<EvaluateFn>(0, 1) : nullptr;
    if (!original) return NVSDK_NGX_Result_Fail;
    // Sampling keeps ~54 virtual reads off every single frame while still
    // catching a guide set that changes after the first frames.
    std::uint64_t guides = 0;
    int ints[std::size(evaluate_ints)]{};
    bool ints_present[std::size(evaluate_ints)]{};
    float floats[std::size(evaluate_floats)]{};
    bool floats_present[std::size(evaluate_floats)]{};
    Observed::Role roles[std::size(role_keys)]{};
    bool sampled = false;
    if (impl && parameters && handle) {
        std::uint64_t seen = 0;
        {
            std::lock_guard lock(impl->mutex);
            auto* f = impl->slot_for(reinterpret_cast<std::uint64_t>(handle), true);
            // Late loading attaches AFTER the game created its features, so
            // the first thing this surface ever sees is an Evaluate on a
            // handle it has no record of. Dropping those would mean a late
            // attach observes nothing at all -- exactly the evidence the
            // NGX-direct route is being adapted for. The record is opened
            // here with create_observed=false.
            if (f) seen = f->evaluates;
        }
        if (seen == 0 || seen % 600 == 0) {
            auto* readable = NgxObserver::Impl::readable(parameters);
            guides = NgxObserver::Impl::scan_guides(readable);
            for (unsigned i = 0; i < std::size(evaluate_ints); ++i)
                ints_present[i] = readable->Get(evaluate_ints[i], &ints[i]) == NVSDK_NGX_Result_Success;
            for (unsigned i = 0; i < std::size(evaluate_floats); ++i)
                floats_present[i] = readable->Get(evaluate_floats[i], &floats[i]) == NVSDK_NGX_Result_Success;
            for (unsigned i = 0; i < std::size(role_keys); ++i) {
                ID3D12Resource* resource = nullptr;
                if (readable->Get(role_keys[i], &resource) != NVSDK_NGX_Result_Success || !resource) continue;
                const auto d = resource->GetDesc();  // pure query; nothing is bound, changed or submitted
                roles[i] = {true, static_cast<unsigned>(d.Format), static_cast<unsigned>(d.Width),
                            d.Height, d.MipLevels, d.SampleDesc.Count,
                            static_cast<unsigned>(d.Dimension), static_cast<unsigned>(d.Flags)};
            }
            sampled = true;
        }
    }
    // ---- admission (ngx-rr): the Streamline route's sequence, on NGX.
    // Everything the translation needs is read from the game's parameter block
    // BEFORE the call, so the frame describes exactly what NGX was handed.
    live::IFrameReceiver* receiver = impl && handle && parameters ? impl->receiver.load(std::memory_order_acquire) : nullptr;
    ngx::Evaluation evaluation;
    auto* const readable = receiver ? NgxObserver::Impl::readable(parameters) : nullptr;
    const auto resource = [&](const char* key) {
        ID3D12Resource* r = nullptr;
        return readable->Get(key, &r) == NVSDK_NGX_Result_Success ? r : nullptr;
    };
    if (receiver) {
        // Read before the candidacy decision: an unobserved create is judged
        // by whether the call carries an Output at all.
        evaluation.output = resource(NVSDK_NGX_Parameter_Output);
        ngx::Candidacy candidacy = ngx::Candidacy::candidate;
        try {
            std::lock_guard lock(impl->mutex);
            if (auto* f = impl->slot_for(reinterpret_cast<std::uint64_t>(handle), true)) {
                evaluation.create_observed = f->create_observed;
                evaluation.feature_id = f->feature_id;
                evaluation.create_flags = f->create_flags;
                evaluation.output_width = f->out_width;
                evaluation.output_height = f->out_height;
                for (const auto& other : impl->features)
                    if (&other != f && other.used && !other.released && other.create_observed &&
                        other.feature_id == ngx::ray_reconstruction)
                        evaluation.rr_alive_elsewhere = true;
            }
            candidacy = ngx::candidacy(evaluation.create_observed, evaluation.feature_id, evaluation.output != nullptr);
            if (candidacy == ngx::Candidacy::other_feature) {
                ++impl->ignored_other_features;
                if (evaluation.feature_id < impl->ignored_by_feature.size()) ++impl->ignored_by_feature[evaluation.feature_id];
                else ++impl->ignored_feature_id_out_of_range;
            } else if (candidacy == ngx::Candidacy::unobserved_without_output) {
                ++impl->ignored_unobserved_without_output;
            }
        } catch (...) {
            // The record lock threw: nothing is known about this call and
            // nothing of ours has run, so it is forwarded, never offered.
            candidacy = ngx::Candidacy::unobserved_without_output;
        }
        // Not a candidate: forwarded untouched. No call/frame number, no binding
        // scope, no offer, no skip, and `blocked` keeps describing the last
        // CANDIDATE -- frame generation must not repaint the panel every frame.
        if (candidacy != ngx::Candidacy::candidate) receiver = nullptr;
    }
    if (receiver) {
        evaluation.command = list;
        evaluation.depth = resource(NVSDK_NGX_Parameter_Depth);
        evaluation.motion = resource(NVSDK_NGX_Parameter_MotionVectors);
        evaluation.mv_scale_present =
            readable->Get(NVSDK_NGX_Parameter_MV_Scale_X, &evaluation.mv_scale_x) == NVSDK_NGX_Result_Success &&
            readable->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &evaluation.mv_scale_y) == NVSDK_NGX_Result_Success;
        if (readable->Get(NVSDK_NGX_Parameter_Reset, &evaluation.reset) != NVSDK_NGX_Result_Success) evaluation.reset = 0;
        int rw = 0, rh = 0;
        evaluation.render_subrect_present =
            readable->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &rw) == NVSDK_NGX_Result_Success &&
            readable->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &rh) == NVSDK_NGX_Result_Success &&
            rw > 0 && rh > 0;
        evaluation.render_width = static_cast<unsigned>(rw);
        evaluation.render_height = static_cast<unsigned>(rh);
        // Every subrect base DLSS accepts. Absent means the origin; a set,
        // nonzero base is a region we do not crop to, so the frame is skipped.
        for (const char* key : {NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
                                NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
                                NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
                                NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y}) {
            unsigned int u = 0; int i = 0;
            if ((readable->Get(key, &u) == NVSDK_NGX_Result_Success && u) || (readable->Get(key, &i) == NVSDK_NGX_Result_Success && i))
                evaluation.subrect_base_offset = true;
        }
        // Call and frame are one monotonic counter: NGX has no frame token,
        // and the runtime requires both to increase.
        evaluation.call = evaluation.frame = impl->next_call.fetch_add(1) + 1;
        receiver->game_binding_enter(evaluation.call, evaluation.frame, list);
    }

    const int list_type = list ? static_cast<int>(list->GetType()) : -1;  // pure query on the game's list
    const auto result = original(list, handle, parameters, callback);

    if (receiver) {
        if (result != NVSDK_NGX_Result_Success) {
            receiver->game_binding_exit(evaluation.call);
            receiver->aborted(evaluation.call);
            try { std::lock_guard lock(impl->mutex); ++impl->offered; ++impl->aborted; } catch (...) {}
        } else {
            // Still inside the detour, after NGX recorded RR and before the
            // game resumes and re-sets its own state: NR lands right after RR
            // on the same command list, as on the Streamline route.
            live::Frame frame;
            const char* why = ngx::translate(evaluation, frame);
            if (why) {
                // The outer call succeeded and nothing of ours was recorded:
                // skip with a history reset and keep the user's ON gate. The
                // default disposition is TERMINAL and would kill NR outright
                // for what is usually just "change a quality setting once".
                RejectedCall rejection;
                rejection.call = evaluation.call;
                rejection.frame = evaluation.frame;
                strcpy_s(rejection.stage, "ngx-profile");
                strncpy_s(rejection.reason, why, _TRUNCATE);
                rejection.disposition = RejectedDisposition::skipped_before_insertion;
                receiver->rejected(rejection);
            } else {
                receiver->frame(frame);
            }
            receiver->boundary_returned(evaluation.call, true);
            // Close the binding scope opened before the game's call, on every
            // exit -- the Streamline route does the same (workbench_adapter.cpp,
            // EndGameBindings). Only the failure path used to: harmless while
            // every NGX package runs without binding preservation, but the
            // second frame of one that enables it would hit "Overlapping game
            // binding entry".
            receiver->game_binding_exit(evaluation.call);
            impl->blocked.store(why, std::memory_order_release);
            try {
                std::lock_guard lock(impl->mutex);
                ++impl->offered;
                if (why) { ++impl->skipped; impl->last_skip = why; } else ++impl->admitted;
            } catch (...) {}
        }
    }

    if (!impl || !handle) return result;
    try {
        std::lock_guard lock(impl->mutex);
        ++impl->evaluates;
        if (auto* f = impl->slot_for(reinterpret_cast<std::uint64_t>(handle), true)) {
            ++f->evaluates;
            if (list_type >= 0) {
                if (f->list_type < 0) f->list_type = list_type;
                else if (f->list_type != list_type) f->list_type_mixed = true;
            }
            f->last_evaluate_result = static_cast<std::uint32_t>(result);
            if (result != NVSDK_NGX_Result_Success) ++f->failed_evaluates;
            if (sampled) {
                f->guides |= guides; // union across samples, never narrowed
                f->sampled = true;
                for (unsigned i = 0; i < std::size(evaluate_ints); ++i)
                    if (ints_present[i]) { f->ints[i] = ints[i]; f->ints_present[i] = true; }
                for (unsigned i = 0; i < std::size(evaluate_floats); ++i)
                    if (floats_present[i]) { f->floats[i] = floats[i]; f->floats_present[i] = true; }
                for (unsigned i = 0; i < std::size(role_keys); ++i)
                    if (roles[i].present) f->roles[i] = roles[i];
            }
        }
    } catch (...) {}
    return result;
}

NVSDK_NGX_Result NVSDK_CONV release_detour(NVSDK_NGX_Handle* handle) {
    auto* impl = NgxObserver::Impl::active.load(std::memory_order_acquire);
    const auto original = impl ? impl->hooks.original<ReleaseFn>(0, 2) : nullptr;
    if (!original) return NVSDK_NGX_Result_Fail;
    const std::uint64_t id = reinterpret_cast<std::uint64_t>(handle);
    const auto result = original(handle);
    if (!impl) return result;
    try {
        std::lock_guard lock(impl->mutex);
        ++impl->releases;
        if (auto* f = impl->slot_for(id, false)) f->released = true;
    } catch (...) {}
    return result;
}

} // namespace

bool NgxObserver::Impl::install_locked(void* create, void* evaluate, void* release) {
    Impl* expected = nullptr;
    if (!active.compare_exchange_strong(expected, this)) {
        refusal = "Another NGX observer already owns the loader hooks";
        return false;
    }
    if (!create || !evaluate || !release) {
        active.store(nullptr);
        refusal = "The mapped NGX loader does not export the three D3D12 feature entries";
        return false;
    }
    const HookBank<3, 1>::Detours detours{HookBank<3, 1>::Entries{
        reinterpret_cast<void*>(&create_detour),
        reinterpret_cast<void*>(&evaluate_detour),
        reinterpret_cast<void*>(&release_detour)}};
    hooks.install(HookBank<3, 1>::Entries{create, evaluate, release}, detours);  // throws; the caller clears `active`
    installed = true;
    return true;
}

bool NgxObserver::attach_entries_for_test(void* create, void* evaluate, void* release) noexcept try {
    std::lock_guard lock(impl_->mutex);
    if (impl_->installed || !impl_->refusal.empty()) return false;
    impl_->mapped_path = L"(test entries; no NGX loader)";
    return impl_->install_locked(create, evaluate, release);
} catch (...) {
    Impl* self = impl_.get();
    Impl::active.compare_exchange_strong(self, nullptr);
    return false;
}

bool NgxObserver::attach() noexcept try {
    std::lock_guard lock(impl_->mutex);
    if (impl_->installed) return true;
    if (!impl_->refusal.empty()) return false;

    if (impl_->registry_path.empty()) impl_->registry_path = registry_loader_directory();
    if (impl_->registry_path.empty()) {
        impl_->refusal = "NGX directory absent from HKLM NGXCore\\FullPath";
        return false;
    }
    // Adopt only what the GAME already mapped. Loading NGX ourselves would
    // change what the process does, which an observation surface must never do
    // -- the same rule the root proxy follows for system DXGI. A module that
    // merely carries the name is refused: its directory must be the one the
    // driver registered.
    HMODULE module = nullptr;
    std::wstring mapped;
    for (const auto* name : loader_names) {
        HMODULE candidate = nullptr;
        if (!GetModuleHandleExW(0, name, &candidate) || !candidate) continue;
        const auto path = module_path(candidate);
        if (same_path(std::filesystem::path(path).parent_path().wstring(), impl_->registry_path)) {
            module = candidate; mapped = path; break;
        }
        // Named like NGX but installed somewhere the driver did not put it.
        FreeLibrary(candidate);
        impl_->mapped_path = path;
        impl_->refusal = "A module named like the NGX core is mapped from a path the driver did not install";
        return false;
    }
    if (!module) return false; // Not mapped yet; the caller polls.
    impl_->mapped_path = mapped;
    const auto entry = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(module, name)); };
    if (!impl_->install_locked(entry("NVSDK_NGX_D3D12_CreateFeature"), entry("NVSDK_NGX_D3D12_EvaluateFeature"),
                               entry("NVSDK_NGX_D3D12_ReleaseFeature"))) {
        FreeLibrary(module);
        return false;
    }
    // The reference is deliberately kept: the trampolines point into this
    // module and the hooks are never removed, so it must not unmap.
    impl_->loader = module;
    // Hash and signature of the loader we hooked, so the report says WHICH
    // loader produced the observation rather than just where it lived.
    try { impl_->loader_identity = module_identity(module); } catch (...) {}
    return true;
} catch (const std::exception& error) {
    Impl::active.store(nullptr);
    try { impl_->refusal = error.what(); } catch (...) {}
    return false;
} catch (...) {
    Impl::active.store(nullptr);
    return false;
}

bool NgxObserver::attached() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->installed;
}

void NgxObserver::set_receiver(live::IFrameReceiver* receiver) noexcept {
    impl_->receiver.store(receiver, std::memory_order_release);
}

const char* NgxObserver::blocked() const noexcept {
    return impl_->blocked.load(std::memory_order_acquire);
}

bool NgxObserver::refused() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return !impl_->refusal.empty();
}

json NgxObserver::report() const {
    std::lock_guard lock(impl_->mutex);
    json features = json::array();
    for (const auto& f : impl_->features) {
        if (!f.used) continue;
        json guides = json::array();
        for (unsigned i = 0; i < guide_count; ++i)
            if (f.guides & (std::uint64_t{1} << i)) guides.push_back(guide_keys[i]);
        json scalars = json::object();
        for (unsigned i = 0; i < std::size(evaluate_ints); ++i)
            if (f.ints_present[i]) scalars[evaluate_ints[i]] = f.ints[i];
        for (unsigned i = 0; i < std::size(evaluate_floats); ++i)
            if (f.floats_present[i]) scalars[evaluate_floats[i]] = f.floats[i];
        json roles = json::object();
        for (unsigned i = 0; i < std::size(role_keys); ++i) {
            const auto& r = f.roles[i];
            if (!r.present) continue;
            roles[role_names[i]] = {{"dxgi_format", r.format}, {"width", r.width}, {"height", r.height},
                {"mips", r.mips}, {"samples", r.samples}, {"dimension", r.dimension}, {"flags", r.flags}};
        }
        features.push_back({
            {"roles", roles},
            {"command_list", f.list_type == 0 ? "direct" : f.list_type == 2 ? "compute" : f.list_type == 1 ? "bundle"
                             : f.list_type == 3 ? "copy" : f.list_type < 0 ? "unobserved" : "other"},
            {"command_list_mixed", f.list_type_mixed},
            {"feature_id", f.create_observed ? json(f.feature_id) : json(nullptr)},
            {"handle", f.handle}, {"create_observed", f.create_observed},
            {"create_result", f.create_result},
            {"render", {f.width, f.height}}, {"output", {f.out_width, f.out_height}},
            {"perf_quality", f.perf_quality}, {"create_flags", f.create_flags},
            {"presets", [&] {
                json out = json::object();
                for (unsigned i = 0; i < std::size(preset_keys); ++i)
                    if (f.presets_present[i]) out[preset_modes[i]] = f.presets[i];
                return out;
            }()},
            {"evaluates", f.evaluates}, {"failed_evaluates", f.failed_evaluates},
            {"last_evaluate_result", f.last_evaluate_result},
            {"released", f.released}, {"sampled", f.sampled},
            {"guides", guides}, {"scalars", scalars}});
    }
    return {
        {"state", impl_->installed ? "attached" : (impl_->refusal.empty() ? "waiting" : "refused")},
        {"refusal", impl_->refusal},
        {"registry_path", utf8(impl_->registry_path)},
        {"mapped_path", utf8(impl_->mapped_path)},
        {"loader", impl_->loader_identity},
        {"creates", impl_->creates}, {"evaluates", impl_->evaluates},
        {"releases", impl_->releases}, {"feature_overflow", impl_->overflow}, {"feature_slots_recycled", impl_->recycled},
        // The payload. It was built and then left out of this object, so the
        // first real observation run recorded over a thousand evaluates and
        // reported none of them. Counters without the records they summarise are
        // not an observation.
        {"features", features},
        {"guide_vocabulary", guide_count},
        {"contract", impl_->receiver.load() == nullptr
            ? "observation only: no frame admitted, no GPU work, no resource or feature of ours, "
              "no NGX call except forwarding the game's own; absent guides are unobserved, not unsupported"
            : "admission on the NGX routes: each super-resolution or ray-reconstruction evaluate is offered to the "
              "live runtime after the game's call returns; refusals are named skips that keep the ON gate; every other "
              "NGX feature (frame generation) is forwarded untouched, never offered, only counted; NR itself defaults OFF"},
        {"admission", impl_->admission_locked()},
        {"hooks", impl_->hooks.snapshot()}};
}

json NgxObserver::Impl::admission_locked() const {
    json by_feature = json::object();
    for (std::size_t id = 0; id < ignored_by_feature.size(); ++id)
        if (ignored_by_feature[id]) by_feature[std::to_string(id)] = ignored_by_feature[id];
    if (ignored_feature_id_out_of_range) by_feature["other"] = ignored_feature_id_out_of_range;
    return {{"receiver", receiver.load() != nullptr},
            {"offered", offered}, {"admitted", admitted},
            {"skipped", skipped}, {"aborted", aborted},
            {"last_skip", last_skip},
            // Never offered: not SR/RR (frame generation is feature 11), or a
            // feature whose create we did not see and whose call has no Output.
            {"ignored_other_features", ignored_other_features},
            {"ignored_by_feature", by_feature},
            {"ignored_unobserved_without_output", ignored_unobserved_without_output}};
}

json NgxObserver::admission() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->admission_locked();
}
}
