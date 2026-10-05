// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <sl_core_api.h>
#include <array>
#include <cstdint>

namespace lab::slboundary {
// Borrowed API metadata, valid only inside the callback. No COM queries/refs,
// resource-state inference, frame-token vtable calls, caches or file output.
enum Issue : unsigned {
    fault=1, bound=2, duplicate=4, version=8, cycle=16, invalid=32,
    extension=64, unknown_input=128
};
struct Tag {
    sl::BufferType type{};
    sl::ResourceLifecycle lifecycle{};
    sl::Extent extent{};
    void* native=nullptr;
    sl::ResourceType resource_type=sl::ResourceType::eTex2d;
    std::uint32_t state=UINT32_MAX;
    bool null_resource=false;
    unsigned issues=0;
    // Never set by the decoder. A legacy tag the game declared eOnlyValidNow,
    // for one of NR's INPUT roles, copied at its own slSetTag call into a
    // Lab-owned texture (lab_sl_volatile_copy.hpp): native and state then name
    // that copy, lifecycle stays the game's word, lab_copy is the copy's serial.
    // copy_refusal: the CopyOutcome that stopped such a copy (0: none tried).
    // game_native / game_state: for a Lab copy, the resource and state the game
    // itself tagged, kept so no record presents the copy's as the game's word.
    std::uint64_t lab_copy=0;
    std::uint8_t copy_refusal=0;
    void* game_native=nullptr;
    std::uint32_t game_state=UINT32_MAX;
    void* tagged_native() const noexcept {return lab_copy?game_native:native;}
    std::uint32_t tagged_state() const noexcept {return lab_copy?game_state:state;}
};
struct Inputs {
    // issues: every problem found, OR'd over the call and each tag (strict).
    // call_issues: only those that concern the call as a whole -- its viewport,
    // an element that is not a readable ResourceTag, a bound, an extension --
    // never a problem confined to one tag, which stays in that Tag::issues.
    unsigned issues=0, call_issues=0, tag_count=0;
    bool viewport_present=false, constants_present=false;
    std::uint32_t viewport=UINT32_MAX;
    std::array<Tag,16> tags{};
    sl::Constants constants{};
};
// Strict public version checks; unknown structures are never silently ignored
// when this packet is used to decide whether an adapter may render.
Inputs decode_inputs(const sl::BaseStructure** inputs,std::uint32_t count) noexcept;
Inputs decode_tags(const sl::ViewportHandle&,const sl::ResourceTag*,std::uint32_t count) noexcept;
Inputs decode_constants(const sl::ViewportHandle&,const sl::Constants&) noexcept;
bool read_pointer(void*& out,const void* source) noexcept;

enum class Api { token, constants, tags, evaluate };
struct Call {
    Api api=Api::evaluate;
    std::uint64_t id=0,parent=0;
    void* token=nullptr; // Address identity only; never a durable frame ID.
    void* command=nullptr;
    sl::Feature feature=0;
    sl::Result result=sl::Result::eErrorInvalidParameter;
    std::uint32_t thread=0,frame_index=0;
    bool frame_index_known=false,concurrent=false;
    // Most recent other boundary entry seen while this call was active (Api
    // value and thread). Diagnostic identity only; UINT32_MAX when unknown.
    std::uint32_t overlapped_api=UINT32_MAX,overlapped_thread=0;
    // Concurrent, but every boundary call that overlapped it was a
    // slGetNewFrameToken. A Plague Tale: Resonance frame fetches its token about
    // ten times (latency markers) from other threads, so this is the normal
    // case there, not a race on tags or constants.
    bool token_only_overlap=false;
    bool frame_scoped_tags=false; // slSetTagForFrame, never legacy last-seen tags.
    Inputs inputs{};
};
// Internal RR callback ABI is separately pinned by module hash. This packet is
// borrowed only during the live outer call, before common's conditional restore.
struct InnerCall {
    enum class Phase { begin, end } phase=Phase::begin;
    void* command=nullptr;
    std::uint32_t viewport=UINT32_MAX,frame=UINT32_MAX;
    sl::Result result=sl::Result::eErrorInvalidParameter;
    bool metadata_valid=false,sequence_valid=false;
};
struct RestoreCall {
    void* command=nullptr;
    std::uint32_t result=UINT32_MAX; // chi::ComputeStatus, not sl::Result or HRESULT.
    bool caller_verified=false,sequence_valid=false;
};
class Sink {
public:
    virtual ~Sink()=default;
    // Enter/abort allow a consumer to freeze an exact invocation rather than
    // looking up mutable global metadata only after the original returned.
    virtual void entering(const Call&) noexcept {}
    virtual void aborted(const Call&) noexcept {}
    virtual void inner_returned(const Call&,const InnerCall&) noexcept {}
    // Verified common has selected its restore branch, but has not replayed
    // bindings yet. No success result exists at this point.
    virtual void restore_entering(const Call&,const RestoreCall&) noexcept {}
    virtual void restore_returned(const Call&,const RestoreCall&) noexcept {}
    // Called AFTER the original API returns, before returning to the caller.
    // Local tags remain local. Global tags/constants are delivered separately:
    // never pair them using "last seen" or a recycled token pointer alone.
    // No GPU work is authorized by metadata availability. The game adapter
    // must establish format, color, lifetime, state restoration and completion.
    // Implementation must be bounded, nonblocking, noexcept and thread-safe.
    virtual void returned(const Call&) noexcept=0;
};
}
