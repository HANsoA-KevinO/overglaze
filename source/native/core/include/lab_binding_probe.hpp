// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <type_traits>

namespace lab::diagnostic {
// One explicitly armed functional intervention, never a normal NR mode.
// Deadline is checked before each callback; completion means no more binding
// restores are scheduled, NOT a GPU fence or visual correctness assertion.
enum class BindingProbePhase : unsigned { idle, armed, running, complete, cancelled, failed };
struct BindingProbeStatus {
    unsigned size=sizeof(BindingProbeStatus),abi=1;
    BindingProbePhase phase=BindingProbePhase::idle;
    std::uint64_t started_ms=0,deadline_ms=0,restores=0,first_frame=0,last_frame=0,last_call=0;
};
static_assert(std::is_trivially_copyable_v<BindingProbeStatus>);
class BindingProbe {
    BindingProbeStatus status_;
public:
    static constexpr std::uint64_t max_ms=60000,max_restores=16384;
    bool active()const{return status_.phase==BindingProbePhase::armed||status_.phase==BindingProbePhase::running;}
    bool arm(std::uint64_t now){
        if(status_.phase!=BindingProbePhase::idle||now>UINT64_MAX-max_ms)return false;
        status_.started_ms=now;status_.deadline_ms=now+max_ms;status_.phase=BindingProbePhase::armed;return true;
    }
    void cancel(){if(active())status_.phase=BindingProbePhase::cancelled;}
    void fail(){if(active())status_.phase=BindingProbePhase::failed;}
    void expire(std::uint64_t now){if(active()&&(now<status_.started_ms||now>=status_.deadline_ms||status_.restores>=max_restores))status_.phase=BindingProbePhase::complete;}
    bool admit(std::uint64_t now,std::uint64_t frame,std::uint64_t call,bool ready){
        expire(now);if(!active())return false;
        if(!ready||!frame||!call||frame<=status_.last_frame||call<=status_.last_call){fail();return false;}
        return true;
    }
    void recorded(std::uint64_t frame,std::uint64_t call){
        if(!active())return;
        if(!status_.restores)status_.first_frame=frame;
        status_.last_frame=frame;status_.last_call=call;++status_.restores;status_.phase=BindingProbePhase::running;
        if(status_.restores==max_restores)status_.phase=BindingProbePhase::complete;
    }
    const BindingProbeStatus& status()const{return status_;}
};
inline const char* binding_probe_name(BindingProbePhase p){switch(p){
case BindingProbePhase::idle:return "idle";case BindingProbePhase::armed:return "armed";
case BindingProbePhase::running:return "running";case BindingProbePhase::complete:return "complete";
case BindingProbePhase::cancelled:return "cancelled";default:return "failed";}}
}
