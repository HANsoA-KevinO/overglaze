// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_capture_library.hpp"
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
namespace {
struct Stats {
    std::uint64_t count=0,nonfinite=0,negative=0,over_one=0,positive_y=0;
    double minimum=INFINITY,maximum=-INFINITY,sum=0;
    // Rec.709-weighted NUMERIC brightness, not measured luminance. Finite
    // positive values use 1/128-stop buckets; tails are explicit bounds.
    std::array<std::uint64_t,6146> histogram{};
    void add(std::array<float,4> p){++count;
        if(!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2])){++nonfinite;return;}
        bool neg=false,over=false;for(unsigned c=0;c<3;++c){minimum=std::min(minimum,double(p[c]));maximum=std::max(maximum,double(p[c]));neg|=p[c]<0;over|=p[c]>1;}
        negative+=neg;over_one+=over;const double y=.2126*p[0]+.7152*p[1]+.0722*p[2];sum+=y;
        if(y<=0){++histogram[0];return;}++positive_y;
        const int bin=int(std::floor((std::log2(y)+24)*128))+1;++histogram[std::clamp(bin,1,6145)];
    }
    lab::json percentile(double fraction)const{
        const auto rank=std::max<std::uint64_t>(1,std::uint64_t(std::ceil((count-nonfinite)*fraction)));std::uint64_t n=0;
        for(unsigned b=0;b<histogram.size();++b){n+=histogram[b];if(n<rank)continue;
            if(!b)return {{"nonpositive",true}};
            if(b==1)return {{"upper",std::exp2(-24.+1./128)}};
            if(b==6145)return {{"lower",std::exp2(24.)}};
            return {{"lower",std::exp2((b-1)/128.-24)},{"upper",std::exp2(b/128.-24)}};
        }return nullptr;
    }
    lab::json json()const{return {{"pixels",count},{"nonfinite_rgb_pixels",nonfinite},{"negative_rgb_pixels",negative},{"any_rgb_above_one_pixels",over_one},
        {"rgb_min",std::isfinite(minimum)?lab::json(minimum):lab::json(nullptr)},{"rgb_max",std::isfinite(maximum)?lab::json(maximum):lab::json(nullptr)},
        {"numeric_y_mean",count>nonfinite?lab::json(sum/(count-nonfinite)):lab::json(nullptr)},
        {"numeric_y_percentile_bounds",{{"p01",percentile(.01)},{"p05",percentile(.05)},{"p50",percentile(.5)},{"p95",percentile(.95)},{"p99",percentile(.99)},{"p999",percentile(.999)}}}};}
};
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==2&&std::wstring(argv[1])==L"--test"){
        Stats s;for(float x:{0.f,1.f,4.f,-1.f,INFINITY})s.add({x,x,x,1});
        if(s.count!=5||s.nonfinite!=1||s.over_one!=1||s.negative!=1||s.minimum!=-1||s.maximum!=4||!s.percentile(.5).at("nonpositive").get<bool>()||s.percentile(.75).at("lower")!=1)throw std::runtime_error("Statistics reference failed");
        std::cout<<"PASS numeric histogram boundaries, finite/negative/high-range counts\n";return 0;
    }
    if(argc!=3)throw std::runtime_error("Usage: overglaze_capture_stats <absolute manifest> <new report.json>");
    const std::filesystem::path out=argv[2];if(!out.is_absolute()||std::filesystem::exists(out))throw std::runtime_error("New absolute report path required");
    lab::RawCapture raw(argv[1]);lab::json result={{"kind","dlsslab-four-stage-numeric-range-v1"},{"purpose","existing-research-image-analysis"},
        {"manifest",lab::utf8(raw.path().wstring())},{"manifest_sha256",raw.manifest_hash()},{"source_modified",false},{"new_game_or_gpu_execution",false},
        {"scope","All pixels; numeric RGB and .2126R+.7152G+.0722B. No gamut, transfer function, exposure or absolute nits inferred."},
        {"stages",lab::json::array()},{"tool",lab::module_identity(nullptr)}};
    if(raw.display_pair())result["kind"]="dlsslab-display-pair-numeric-range-v1";
    for(unsigned i=0;i<raw.stage_count();++i){Stats s;for(unsigned y=0;y<raw.height();++y)for(unsigned x=0;x<raw.width();++x)s.add(raw.pixel(i,x,y));
        auto item=s.json();item["stage"]=raw.manifest().at("stages")[i].at("stage");item["sha256"]=raw.manifest().at("stages")[i].at("sha256");result["stages"].push_back(std::move(item));}
    const auto data=result.dump(2);lab::Handle file(CreateFileW(out.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));lab::check(file.valid(),"Create numeric report");DWORD written{};
    lab::check(WriteFile(file.value,data.data(),DWORD(data.size()),&written,nullptr)&&written==data.size(),"Write numeric report");std::cout<<data<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
