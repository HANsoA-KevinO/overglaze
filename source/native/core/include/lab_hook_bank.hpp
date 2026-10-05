// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <mutex>
namespace lab {
// Bounded process-lifetime code hooks. A separate detour per implementation
// selects its own immutable trampoline, including nested wrapper -> base calls.
// No caller-vtable lookup, COM-object replacement, or one-original-for-all ABI.
template<std::size_t Slots,std::size_t Banks=4> class HookBank final {
    std::array<std::array<void*,Slots>,Banks> targets_{};
    std::array<std::array<std::atomic<void*>,Slots>,Banks> originals_{};
    mutable std::mutex mutex_;
    json diagnostics_=json::array();bool failed_=false;
    static bool executable(void* p){MEMORY_BASIC_INFORMATION m{};
        return p&&VirtualQuery(p,&m,sizeof(m))&&m.State==MEM_COMMIT&&!(m.Protect&PAGE_GUARD)&&
            (m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));}
public:
    using Entries=std::array<void*,Slots>;
    using Detours=std::array<Entries,Banks>;
    template<class Fn>Fn original(unsigned bank,unsigned slot)const noexcept {
        return reinterpret_cast<Fn>(originals_[bank][slot].load(std::memory_order_acquire));
    }
    void install(const Entries& targets,const Detours& callbacks){
        std::lock_guard lock(mutex_);
        if(failed_)throw std::runtime_error("Prior native hook-bank failure; no retry");
        std::array<unsigned,Slots> plan{};std::array<bool,Slots> fresh{};
        // Validate the complete batch before changing any native function.
        for(unsigned slot=0;slot<Slots;++slot){
            if(!executable(targets[slot]))throw std::runtime_error("Native hook target is not executable");
            for(unsigned other=0;other<Slots;++other)if(other!=slot){
                if(targets[other]==targets[slot])throw std::runtime_error("Different native hook signatures alias");
                for(const auto& bank:targets_)if(bank[other]==targets[slot])throw std::runtime_error("Existing native hook signature alias");
            }
            unsigned selected=Banks,empty=Banks;
            for(unsigned bank=0;bank<Banks;++bank){
                if(targets_[bank][slot]==targets[slot]){selected=bank;break;}
                if(!targets_[bank][slot]&&empty==Banks)empty=bank;
            }
            if(selected!=Banks){plan[slot]=selected;continue;}
            if(empty==Banks)throw std::runtime_error("Native DXGI implementation capacity exceeded");
            if(!executable(callbacks[empty][slot]))throw std::runtime_error("Native hook callback unavailable");
            plan[slot]=empty;fresh[slot]=true;
        }
        const auto init=MH_Initialize();if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("MinHook unavailable");
        try{
            for(unsigned slot=0;slot<Slots;++slot)if(fresh[slot]){
                const auto bank=plan[slot];void* trampoline=nullptr;
                const auto result=MH_CreateHook(targets[slot],callbacks[bank][slot],&trampoline);
                diagnostics_.push_back({{"slot",slot},{"implementation",bank},{"address",reinterpret_cast<std::uint64_t>(targets[slot])},{"create",result}});
                if(result!=MH_OK)throw std::runtime_error("Native hook conflict/create failed");
                targets_[bank][slot]=targets[slot];originals_[bank][slot].store(trampoline,std::memory_order_release);
            }
            for(unsigned slot=0;slot<Slots;++slot)if(fresh[slot]){
                const auto result=MH_EnableHook(targets[slot]);
                for(auto& d:diagnostics_)if(d["slot"]==slot&&d["implementation"]==plan[slot])d["enable"]=result;
                if(result!=MH_OK)throw std::runtime_error("Native partial enable retained, no retry");
            }
        }catch(...){failed_=true;throw;} // Enabled code and original slots stay alive.
    }
    json snapshot()const{std::lock_guard lock(mutex_);return diagnostics_;}
    void uninstall_quiesced_for_test(){std::lock_guard lock(mutex_);
        for(auto& bank:targets_)for(auto* target:bank)if(target){MH_DisableHook(target);MH_RemoveHook(target);}
    }
};
}
