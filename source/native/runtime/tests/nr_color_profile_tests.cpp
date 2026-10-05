// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_nr_color_profile_report.hpp"
#include <cstring>
#include <iostream>
#include <limits>

namespace {
void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
int main(){try{
    // Byte-for-byte compatibility with the previous explicit live recipe. The
    // consolidation must not silently alter the current game/NR color path.
    lab::nr::PrePostColorConstants previous{};
    previous.width=5120;previous.height=2880;previous.pre_post=1;
    previous.tonemap_operator=3;previous.exposure=1;previous.sigma=10;previous.gamma=.454f;
    previous.pre_matrix={{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};previous.post_matrix=previous.pre_matrix;
    const auto current=lab::nr::experimental_color_constants(5120,2880);
    need(!std::memcmp(&previous,&current,sizeof(current)),"Existing live recipe changed");
    const auto reduced=lab::nr::experimental_color_constants(1280,720);
    need(reduced.width==1280&&reduced.height==720&&reduced.exposure==current.exposure&&
         reduced.pre_matrix==current.pre_matrix,"Host extent changed color math");
    for(auto extent:{std::pair{0u,720u},std::pair{1280u,0u},std::pair{8193u,720u},std::pair{1280u,8193u}}){
        bool rejected=false;try{lab::nr::experimental_color_constants(extent.first,extent.second);}
        catch(const std::invalid_argument&){rejected=true;}need(rejected,"Bad extent accepted");
    }
    for(bool guard:{false,true}){
        const auto report=lab::nr::color_profile_report(current,guard);
        need(report["official_runtime_equivalence"]==false&&report["official_runtime_constants_observed"]==false&&
             report["game_color_semantics_verified"]==false,"Static recipe promoted to runtime proof");
        need(report["lab_extensions"]["invalid_rgb_guard"]==guard,"Guard incorrectly claimed official");
        need(report["exposure"]==current.exposure&&report["operator"]==current.tonemap_operator&&
             report["pre_matrix"]==current.pre_matrix,"Report differs from supplied constants");
        need(report["viewer_transform_in_model_path"]==false&&report["host_selects_color_math"]==false,"Host/viewer changed NR policy");
    }
    // Exposure is now an explicit host setting (2^exposure_stops); any finite
    // positive value is reported as supplied. Other recipe changes still refuse.
    auto exposed=current;exposed.exposure=32;
    need(lab::nr::color_profile_report(exposed,false)["exposure"]==32,"Host exposure must be reported as supplied");
    for(float bad:{0.f,-1.f,std::numeric_limits<float>::quiet_NaN()}){auto other=current;other.exposure=bad;bool rejected=false;
        try{lab::nr::color_profile_report(other,false);}catch(const std::invalid_argument&){rejected=true;}
        need(rejected,"Invalid exposure accepted");}
    auto other=current;other.tonemap_operator=1;bool rejected=false;
    try{lab::nr::color_profile_report(other,false);}catch(const std::invalid_argument&){rejected=true;}
    need(rejected,"Different constants mislabeled as the current profile");
    std::cout<<"PASS: shared color recipe unchanged; explicit Lab constants, separate static/runtime evidence and guard provenance\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
