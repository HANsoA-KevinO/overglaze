// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_capture_library.hpp"
#include <iostream>

// Read-only CPU verification against an existing same-call wrapper output.
// No game, NR, screenshots, source edits, or per-pixel result files.
int wmain(int argc,wchar_t** argv){try{
    using namespace lab;
    if(argc!=3)throw std::runtime_error("Usage: overglaze_check_view_reconstruction <manifest> <new-report.json>");
    const std::filesystem::path output=argv[2];
    if(!output.is_absolute()||std::filesystem::exists(output))throw std::runtime_error("New report path required");
    RawCapture capture(argv[1]);
    if(!capture.color_contract(2).reconstruction_available||capture.display_pair())throw std::runtime_error("Unsupported reference contract");
    json examples=json::array();std::uint64_t components=0,failed=0,nonfinite=0,guards=0;double maximum=0,max_ratio=0;
    const auto start=GetTickCount64();
    for(unsigned y=0;y<capture.height();++y){if(GetTickCount64()-start>45000)throw std::runtime_error("Bounded check exceeded 45 seconds");
        for(unsigned x=0;x<capture.width();++x){const auto rr=capture.pixel(0,x,y),nr=capture.pixel(2,x,y),saved=capture.pixel(3,x,y);
            bool guarded=false;for(unsigned c=0;c<3;++c)guarded|=!std::isfinite(nr[c])||nr[c]<0||!std::isfinite(rr[c])||rr[c]<0;guards+=guarded;
            const auto derived=viewer::reconstruct_for_view(nr,rr);
            for(unsigned c=0;c<3;++c){++components;if(!std::isfinite(derived[c])||!std::isfinite(saved[c])){++nonfinite;continue;}
                const double error=std::abs(double(derived[c])-saved[c]);const double ratio=error/(.004+.004*std::abs(derived[c]));
                maximum=std::max(maximum,error);max_ratio=std::max(max_ratio,ratio);
                if(ratio>1){++failed;if(examples.size()<8)examples.push_back({{"x",x},{"y",y},{"channel",c},{"derived",derived[c]},{"saved",saved[c]}});}
            }
        }
    }
    json sources=json::array();for(const auto& row:capture.manifest().at("stages")){
        const auto path=capture.path().parent_path()/wide(row.at("file").get<std::string>());
        if(sha256(path)!=row.at("sha256").get<std::string>())throw std::runtime_error("Source changed after verification");
        sources.push_back({{"path",utf8(path.wstring())},{"sha256",row.at("sha256")}});
    }
    const bool passed=!failed&&!nonfinite;
    json result={{"purpose","functional-verification-of-view-only-reconstruction"},{"passed",passed},{"source",utf8(capture.path().wstring())},
        {"manifest_sha256",capture.manifest_hash()},{"source_files",sources},{"rgb_components",components},{"over_tolerance",failed},
        {"nonfinite_components",nonfinite},{"guarded_pixels",guards},{"max_absolute_error",maximum},{"max_tolerance_ratio",max_ratio},
        {"tolerance","0.004 + 0.004 * abs(derived), unchanged from prior reconstruction audit"},{"examples",examples},
        {"new_nr_calls",0},{"game_started",false},{"originals_modified",false},{"game_final_grade_proven",false},{"tool",module_identity(nullptr)}};
    const auto text=result.dump(2);Handle file(CreateFileW(output.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));check(file.valid(),"Create new verification receipt");DWORD written=0;
    check(WriteFile(file.value,text.data(),DWORD(text.size()),&written,nullptr)&&written==text.size(),"Write verification receipt");
    std::cout<<(passed?"PASS ":"FAIL ")<<components<<" RGB components; over tolerance="<<failed<<"; no new NR\n";return passed?0:1;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
