// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The NGX parameter object the bridge hands to the NR model, plus the two
// helpers every bridge translation unit needs. They live in their own header so
// the live core header does not have to text-include nr_bridge.cpp for them.
//
// Set/Get are the model's own ABI. Two opt-in observation windows are layered
// on Get: begin/end_settings_reads records the Tone/Structure/Style values the
// DLL actually read back during one Evaluate, and begin/end_scene_reads is a
// bounded research trace of every DLSSNR.* query in that same window. Neither
// changes what is returned to the model.
#ifndef NGX_SNIPPET_BUILD
#define NGX_SNIPPET_BUILD
#endif
#include <d3d11.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include "lab_platform.hpp"
#include "lab_nr_settings.hpp"
#include "lab_model_versions.hpp"
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>
#include <limits>
#include <cstring>
#include <typeinfo>

namespace lab::nr {
// The first reviewed model version (lab_model_versions.hpp). The live bridge
// accepts any version in that table; research fixtures that reproduce one exact
// historical result keep pinning this one.
inline constexpr auto kOriginalModelSha256 = lab::model::kKnownVersions[0].sha256;
inline void checked(HRESULT result) { if (FAILED(result)) throw std::runtime_error("D3D12 HRESULT=" + std::to_string(static_cast<unsigned long>(result))); }
using ParameterValue = std::variant<unsigned long long, float, double, unsigned int, int, ID3D11Resource*, ID3D12Resource*, void*>;

class Parameters final : public NVSDK_NGX_Parameter {
    std::map<std::string, ParameterValue> values_;
    mutable std::mutex trace_mutex_;
    mutable std::mutex values_mutex_;
    const bool trace_queries_;
    mutable bool watch_settings_=false;
    mutable lab::nr::SettingsReads settings_reads_;
    // Separate opt-in research watch. Default paths retain their existing ABI.
    mutable bool watch_scene_=false;
    mutable lab::json scene_reads_=lab::json::object();
public:
    explicit Parameters(bool trace_queries=true):trace_queries_(trace_queries){}
    void begin_settings_reads() {std::lock_guard lock(values_mutex_);settings_reads_={};watch_settings_=true;}
    lab::nr::SettingsReads end_settings_reads() {std::lock_guard lock(values_mutex_);watch_settings_=false;return settings_reads_;}
    void begin_scene_reads(){std::lock_guard lock(values_mutex_);scene_reads_=lab::json::object();watch_scene_=true;}
    lab::json end_scene_reads(){std::lock_guard lock(values_mutex_);watch_scene_=false;return scene_reads_;}
    mutable lab::json queries = lab::json::object();
    template<class T> void put(const char* name, T value) { std::lock_guard lock(values_mutex_); values_[name] = value; }
    template<class T> NVSDK_NGX_Result get(const char* name, T* out) const {
        std::lock_guard values_lock(values_mutex_);
        bool found = false;
        auto it = values_.find(name);
        if (it != values_.end() && out) {
            std::visit([&](auto value) {
                using U = decltype(value);
                if constexpr (std::is_same_v<T, U>) { *out = value; found = true; }
                else if constexpr (std::is_arithmetic_v<T> && std::is_arithmetic_v<U>) {
                    const auto numeric = static_cast<long double>(value);
                    if (numeric >= std::numeric_limits<T>::lowest() && numeric <= std::numeric_limits<T>::max()) {
                        *out = static_cast<T>(value); found = true;
                    }
                } else if constexpr (std::is_pointer_v<T> && std::is_pointer_v<U>) {
                    *out = reinterpret_cast<T>(value); found = true;
                }
            }, it->second);
        }
        // Bounded observation of values actually returned to the DLL during the
        // caller's Evaluate window. Host Set/snapshot and Init/Create do not count.
        if(watch_scene_&&std::string_view(name).starts_with("DLSSNR.")){
            // Capture the actual returned value, including Intensity. The map is
            // capped; a cap hit fails research validation, never silently passes.
            auto key=std::string(name)+" / "+typeid(T).name();
            if(scene_reads_.size()<256||scene_reads_.contains(key)){
                auto& r=scene_reads_[key];if(r.is_null())r={{"hits",0},{"misses",0}};
                auto field=found?"hits":"misses";r[field]=r[field].get<unsigned>()+1;
                if(found){if constexpr(std::is_arithmetic_v<T>)r["value"]=*out;
                    else r["pointer"]=reinterpret_cast<std::uintptr_t>(*out);}
            }else scene_reads_["overflow"]=true;
        }
        if constexpr(std::is_same_v<T,float>){if(watch_settings_&&found){
            if(std::strcmp(name,"DLSSNR.LocalToneStrength")==0){settings_reads_.values.tone=*out;settings_reads_.mask|=1;}
            if(std::strcmp(name,"DLSSNR.LocalStructureStrength")==0){settings_reads_.values.structure=*out;settings_reads_.mask|=2;}
            if(std::strcmp(name,"DLSSNR.SkinStructureStrength")==0){settings_reads_.values.skin=*out;settings_reads_.skin_observed=true;}
        }}
        if constexpr(std::is_same_v<T,int>||std::is_same_v<T,unsigned int>){if(watch_settings_&&found&&std::strcmp(name,"DLSSNR.UseAutoMask")==0){
            settings_reads_.values.automask=*out?1u:0u;settings_reads_.automask_observed=true;
        }}
        if constexpr(std::is_same_v<T,unsigned int>){if(watch_settings_&&found&&std::strcmp(name,"DLSSNR.Style")==0){
            settings_reads_.values.style=*out;settings_reads_.style_observed=true;
        }}
        if(trace_queries_) { std::lock_guard lock(trace_mutex_);
          auto key = std::string(name) + " / " + typeid(T).name();
          auto& record = queries[key];
          if (record.is_null()) record = {{"hits", 0}, {"misses", 0}};
          auto field = found ? "hits" : "misses";
          record[field] = record[field].get<unsigned>() + 1;
        }
        return found ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_UnsupportedParameter;
    }
#define LAB_PARAMETER_TYPE(T) \
    void Set(const char* name, T value) override { put(name, value); } \
    NVSDK_NGX_Result Get(const char* name, T* out) const override { return get(name, out); }
    LAB_PARAMETER_TYPE(unsigned long long)
    LAB_PARAMETER_TYPE(float)
    LAB_PARAMETER_TYPE(double)
    LAB_PARAMETER_TYPE(unsigned int)
    LAB_PARAMETER_TYPE(int)
    LAB_PARAMETER_TYPE(ID3D11Resource*)
    LAB_PARAMETER_TYPE(ID3D12Resource*)
    LAB_PARAMETER_TYPE(void*)
#undef LAB_PARAMETER_TYPE
    void Reset() override { std::lock_guard lock(values_mutex_); values_.clear(); }
    lab::json snapshot() const {
        std::lock_guard lock(values_mutex_);lab::json result=lab::json::object();
        for(const auto& [key,value]:values_)std::visit([&](auto v){
            using T=decltype(v);
            if constexpr(std::is_pointer_v<T>)result[key]={{"resource_pointer",reinterpret_cast<std::uintptr_t>(v)}};
            else result[key]=v;
        },value);
        return result;
    }
};
}
