// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <type_traits>
#include <limits>

namespace lab::diagnostic {
// Read-only shadow-state inspection, NOT a native D3D12 state getter, barrier
// tracker, GPU completion record or render admission. Addresses are identities
// only; this structure owns/dereferences no game object.
enum class BindingPoint : unsigned {outer_entry,rr_begin_return,rr_end_return,outer_return};
struct BindingBankSnapshot {
    std::uint64_t root=0,tables=0,descriptors=0,constants=0,constant_hash=0;
    std::array<std::uint64_t,64> table_addresses{};
};
struct BindingSnapshot {
    unsigned tracked=0,ready=0;
    std::uint64_t generation=0,pipeline=0,raytracing_pipeline=0,heap_permutations=0;
    std::array<std::uint64_t,2> heaps{};
    unsigned heap_count=0;
    BindingBankSnapshot compute,graphics;
    char reason[96]{};
};
struct BindingBoundaryEvent {
    unsigned size=sizeof(BindingBoundaryEvent),abi=1;
    BindingPoint point=BindingPoint::outer_entry;
    unsigned valid=0,thread=0,viewport=0;
    std::uint64_t call=0,frame=0,command=0;
    std::array<std::uint64_t,3> resources{}; // frozen SL tags, not NGX queries
    std::array<unsigned,3> declared_states{};
};
struct BindingBoundarySample {BindingBoundaryEvent event;BindingSnapshot bindings;};
struct BindingBoundaryCall {
    std::array<BindingBoundarySample,4> points{};
    unsigned seen=0,complete=0,invalid=0;
};
enum class BindingAuditPhase : unsigned {idle,armed,complete,expired,cancelled,failed};
struct BindingBoundaryReport {
    unsigned size=sizeof(BindingBoundaryReport),abi=1;
    BindingAuditPhase phase=BindingAuditPhase::idle;
    unsigned count=0,complete_calls=0;
    std::uint64_t started_ms=0,deadline_ms=0,callback_losses=0,pre_entry_callbacks=0;
    std::array<BindingBoundaryCall,4> calls{};
};
static_assert(std::is_trivially_copyable_v<BindingBoundaryEvent>);
static_assert(std::is_trivially_copyable_v<BindingBoundaryReport> && sizeof(BindingBoundaryReport)<32768);
// Owner serializes access. No retries, dynamic allocation or per-frame log.
class BindingBoundaryAudit {
    BindingBoundaryReport report_;
public:
    const BindingBoundaryReport& report()const noexcept{return report_;}
    bool arm(std::uint64_t now) noexcept {
        if(report_.phase!=BindingAuditPhase::idle||now>std::numeric_limits<std::uint64_t>::max()-10000)return false;
        report_.phase=BindingAuditPhase::armed;report_.started_ms=now;report_.deadline_ms=now+10000;return true;
    }
    void expire(std::uint64_t now)noexcept {if(report_.phase==BindingAuditPhase::armed){
        if(now<report_.started_ms)loss();else if(now>=report_.deadline_ms)report_.phase=BindingAuditPhase::expired;}}
    void cancel()noexcept {if(report_.phase==BindingAuditPhase::armed)report_.phase=BindingAuditPhase::cancelled;}
    void loss()noexcept {++report_.callback_losses;if(report_.phase!=BindingAuditPhase::idle)report_.phase=BindingAuditPhase::failed;}
    bool accepts(const BindingBoundaryEvent& e,std::uint64_t now)noexcept {
        expire(now);if(report_.phase!=BindingAuditPhase::armed)return false;
        if(e.size!=sizeof(e)||e.abi!=1||unsigned(e.point)>3||!e.call||!e.command){loss();return false;}
        // External arming can happen inside an already-running SL call. Select
        // the NEXT complete entry, never mistake that call's tail for lost data.
        if(!report_.count&&e.point!=BindingPoint::outer_entry){++report_.pre_entry_callbacks;return false;}
        return true;
    }
    void append(const BindingBoundaryEvent& e,const BindingSnapshot& b,std::uint64_t now)noexcept {
        if(!accepts(e,now))return;
        BindingBoundaryCall* row=nullptr;
        for(unsigned i=0;i<report_.count;++i)if(report_.calls[i].points[0].event.call==e.call)row=&report_.calls[i];
        if(e.point==BindingPoint::outer_entry){
            if(row||report_.count==report_.calls.size()||(report_.count&&report_.calls[report_.count-1].seen!=15)){loss();return;}
            row=&report_.calls[report_.count++];
        }else if(!row){loss();return;}
        const auto index=unsigned(e.point),bit=1u<<index;
        if(row->seen&bit){row->invalid=1;loss();return;}
        // Exactly ordered, same call/thread/frame/list/recording, including
        // both independently observed inner callback metadata packets.
        if(row->seen!=bit-1||!e.valid||!b.tracked)row->invalid=1;
        if(index){const auto& first=row->points[0];
            if(e.thread!=first.event.thread||e.frame!=first.event.frame||e.viewport!=first.event.viewport||
               e.command!=first.event.command||b.generation!=first.bindings.generation)row->invalid=1;}
        row->points[index]={e,b};row->seen|=bit;
        if(index==3){row->complete=row->seen==15&&!row->invalid;if(row->complete)++report_.complete_calls;
            if(report_.count==report_.calls.size())report_.phase=BindingAuditPhase::complete;}
    }
};
inline const char* binding_audit_name(BindingAuditPhase p)noexcept {
    switch(p){case BindingAuditPhase::idle:return "idle";case BindingAuditPhase::armed:return "armed";
    case BindingAuditPhase::complete:return "complete";case BindingAuditPhase::expired:return "expired";
    case BindingAuditPhase::cancelled:return "cancelled";default:return "failed";}
}
}
