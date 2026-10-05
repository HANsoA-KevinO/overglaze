// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_sl_observer.hpp"
#include "lab_sl_boundary.hpp"
namespace lab {
// Public API metadata only. No private layout reads. No FrameToken virtual call
// either, unless set_streamline_token_index_through_interface(true) was chosen.
bool install_streamline_observer(const std::array<void*,4>&,slobserve::Sink*,json&);
// Mutually exclusive with the observer install. No QueueLog/CommandLog is
// required or created. Installer must verify actual module identity first.
// Fourth target is either slSetTag (legacy) or slSetTagForFrame (2.8).
// Public 2.8 signature from NVIDIA Streamline; see THIRD_PARTY_NOTICES.md.
using SetTagForFrame=sl::Result(const sl::FrameToken&,const sl::ViewportHandle&,const sl::ResourceTag*,std::uint32_t,sl::CommandBuffer*);
bool install_streamline_boundary(const std::array<void*,4>&,slboundary::Sink*,json&,bool frame_tagging=false);
// New adapters may observe BOTH public tagging ABIs. Slot order: token,
// evaluate, constants, legacy tags, frame tags. The caller must validate all
// five executable targets in the admitted module before installing. This does
// not infer the game's preference flags or grant NR rendering admission.
bool install_streamline_dual_tag_boundary(const std::array<void*,5>&,slboundary::Sink*,json&);
// Detach stops new callbacks, but the borrowed sink must remain alive until
// all in-flight public calls have quiesced. Installed code stays process-pinned.
void detach_streamline_boundary();
// Controller (self-configuring), before attach: when a game fetches its frame
// token WITHOUT a frame index (Streamline then numbers frames itself), read the
// index through the token's own public interface, sl::FrameToken's
// `operator uint32_t`, under an exception guard. Without it such a game's
// every Evaluate is "explicit-token-generation-missing". Research: off.
void set_streamline_token_index_through_interface(bool) noexcept;
std::uint64_t current_streamline_evaluate() noexcept;
// Only delivers within the current thread's live bounded RR Evaluate. Neither
// delivery nor begin/end pairing proves that the game's restore branch is used.
bool notify_streamline_inner(slboundary::InnerCall) noexcept;
bool notify_streamline_restore(slboundary::RestoreCall) noexcept;
bool notify_streamline_restore_entry(slboundary::RestoreCall) noexcept;
json streamline_snapshot();
// Public-interface mode: the boundary is resolved from the interposer's
// PUBLIC export table instead of a per-version RVA table. The gate is the
// module's own Authenticode identity plus the requirement that each export
// address lies inside THAT module's committed, executable, non-guard image.
// No pinned RVA is required, so any signed Streamline build works.
namespace slpublic {
void* verified_export(HMODULE,const char* name) noexcept;
// Public entry points this build exposes. Streamline 2.7 has only slSetTag;
// 2.8 added slSetTagForFrame; 2.12 exports both. We observe whichever exist
// rather than guessing which one the game uses.
struct PublicApi {
    void* token=nullptr;void* evaluate=nullptr;void* constants=nullptr;
    void* legacy_tag=nullptr;void* frame_tag=nullptr;
    void* native_interface=nullptr;void* feature_function=nullptr;
    bool complete() const noexcept {return token&&evaluate&&constants&&native_interface&&(legacy_tag||frame_tag);}
    // Slot 4 of the install target array, plus the optional fifth.
    bool frame_tagging() const noexcept {return !legacy_tag&&frame_tag;}
    void* extra_frame_tag() const noexcept {return legacy_tag&&frame_tag?frame_tag:nullptr;}
    std::array<void*,4> targets() const noexcept {return {token,evaluate,constants,legacy_tag?legacy_tag:frame_tag};}
    const char* tag_abi() const noexcept {return legacy_tag&&frame_tag?"both-public-abis":legacy_tag?"legacy-tags-only":frame_tag?"frame-tags-only":"none";}
};
PublicApi resolve_public_api(HMODULE) noexcept;
json public_api_report(const PublicApi&);
}
void detach_streamline_observer();
// Independent tests only, after all callers quiesce. Game hooks remain pinned.
void uninstall_streamline_observer();
}
