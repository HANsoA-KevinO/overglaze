// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_pair_preview.hpp"
#include "lab_preconvert_reference.hpp"
#include <fstream>
#include <cstring>
#include <cmath>

namespace lab {
namespace {void require(bool x,const char* text){if(!x)throw std::runtime_error(text);}}
std::array<unsigned char,4> pair_preview_pixel(const std::array<float,4>& value,bool hdr){
    std::array<unsigned char,4> result{0,0,0,255};
    for(unsigned c=0;c<3;++c){double v=value[c];if(!std::isfinite(v))return {255,0,255,255};
        if(hdr){v=std::max(0.,v);v=v/(1+v);v=v<=.0031308?v*12.92:1.055*std::pow(v,1./2.4)-.055;}
        result[c]=static_cast<unsigned char>(std::lround(std::clamp(v,0.,1.)*255));}
    return result;
}
PairPreview load_pair_preview(const std::filesystem::path& source,const std::filesystem::path& data_root){
    // The caller's own data root (the console resolves it from its location):
    // only an immediate <data>\<run>\<pair>\manifest.json of that tree is read.
    const auto path=std::filesystem::canonical(source);const auto root=std::filesystem::canonical(data_root);
    require(path.filename()==L"manifest.json"&&path.parent_path().parent_path().parent_path()==root,"Preview must be an immediate lab-run frame pair");
    Handle manifest_lock(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));check(manifest_lock.valid(),"Lock pair manifest");
    require(std::filesystem::file_size(path)<=64*1024,"Pair manifest too large");std::ifstream f(path);auto manifest=json::parse(f);f.close();
    require(manifest.value("kind","")=="dlsslab-frame-pair-v1"&&manifest.value("complete",false)&&manifest.value("same_call",false)&&
        manifest.value("outer_queue_complete",false)&&manifest.value("matched_host_return",false)&&manifest.value("frame",0ULL)&&manifest.value("call",0ULL),"Incomplete same-call capture evidence");
    require(!manifest.value("replay_complete",true)&&!manifest.value("final_present_captured",true)&&!manifest.value("independent_off_baseline",true),"Unexpected capture scope");
    const auto& stages=manifest.at("stages");require(stages.is_array()&&stages.size()==4,"Exactly four stages required");
    PairPreview result;result.manifest=manifest;result.source=utf8(path.wstring());
    constexpr const char* names[]{"rr-output","nr-input","nr-output","hdr-return"};
    for(unsigned i=0;i<4;++i){const auto& row=stages[i];const std::string filename=std::string(names[i])+".rgba16f";
        require(row.value("stage","")==names[i]&&row.value("file","")==filename&&row.value("format","")=="R16G16B16A16_FLOAT"&&
            row.value("endianness","")=="little"&&row.value("subresource",1u)==0,"Unknown raw stage contract");
        const auto width=row.at("width").get<unsigned>(),height=row.at("height").get<unsigned>();
        require(width&&height&&width<=8192&&height<=8192&&row.at("row_pitch")==width*8,"Raw layout exceeds supported bounds");
        if(!i){result.original_width=width;result.original_height=height;const auto step=(std::max(width,height)+2047)/2048;
            result.width=(width+step-1)/step;result.height=(height+step-1)/step;}
        require(width==result.original_width&&height==result.original_height,"Stage extents differ");
        const auto file=path.parent_path()/filename;Handle locked(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));check(locked.valid(),"Lock raw pair stage");
        const auto bytes=std::uint64_t(width)*height*8;
        require(std::filesystem::file_size(file)==bytes&&row.at("bytes")==bytes&&sha256(file)==row.at("sha256").get<std::string>(),"Raw stage size/hash mismatch");
        auto& rgba=result.rgba[i];rgba.resize(std::size_t(result.width)*result.height*4);std::vector<unsigned char> line(width*8);
        const auto step=(std::max(width,height)+2047)/2048;
        for(unsigned y=0;y<height;++y){DWORD read=0;check(ReadFile(locked.value,line.data(),static_cast<DWORD>(line.size()),&read,nullptr)&&read==line.size(),"Read raw pair row");
            if(y%step)continue;for(unsigned x=0;x<width;x+=step){std::array<float,4> values;for(unsigned c=0;c<4;++c){std::uint16_t bits;memcpy(&bits,line.data()+x*8+c*2,2);values[c]=preconvert::unhalf(bits);}
                const auto pixel=pair_preview_pixel(values,i==0||i==3);memcpy(rgba.data()+(std::size_t(y/step)*result.width+x/step)*4,pixel.data(),4);}}
    }
    return result;
}
}
