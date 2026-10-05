// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_capture_library.hpp"
#include "lab_viewer_color.hpp"
#include "lab_preconvert_reference.hpp"
#include "lab_preview_export.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
// The data root of the programs built beside this test, by the out-of-game
// root rule: an overglaze-root.json next to the executables names the program
// root (the build writes one for a build directory outside <root>\data\_build*),
// otherwise the build directory itself is <root>\data\_build*. The viewer under
// test resolves the same root from the same directory and its exports land
// there, so the fixture lives there too; nothing depends on where the checkout is.
std::filesystem::path build_directory(){std::wstring p(32768,L'\0');const auto n=GetModuleFileNameW(nullptr,p.data(),DWORD(p.size()));
    if(!n||n>=p.size())throw std::runtime_error("module path");p.resize(n);
    return std::filesystem::canonical(std::filesystem::path(p).parent_path());}
std::filesystem::path build_data_root(){const auto dir=build_directory();
    if(const auto o=dir/L"overglaze-root.json";std::filesystem::is_regular_file(o)){std::ifstream in(o,std::ios::binary);const auto j=lab::json::parse(in);
        if(j.value("schema","")!="overglaze-root-override-v1"||!j.contains("lab_root")||!j.at("lab_root").is_string())throw std::runtime_error("overglaze-root.json: unknown schema");
        return std::filesystem::canonical(std::filesystem::path(lab::wide(j.at("lab_root").get<std::string>()))/L"data");}
    const auto data=dir.parent_path();
    if(_wcsnicmp(dir.filename().c_str(),L"_build",6)!=0||_wcsicmp(data.filename().c_str(),L"data")!=0)throw std::runtime_error("run from <root>\\data\\_build* or beside an overglaze-root.json");
    return data;}
// Small, explicitly synthetic review card for actual window/layout checks.
// Not installed with the viewer, never confused with game evidence.
int make_review_fixture(){
    const auto root=build_data_root()/("viewer-functional-"+std::to_string(GetCurrentProcessId()));
    if(!std::filesystem::create_directory(root))throw std::runtime_error("Fresh review fixture required");
    const auto pair=root/"pair-1";std::filesystem::create_directory(pair);constexpr unsigned w=512,h=216;
    const char* roles[]{"rr-output","nr-input","nr-output","hdr-return"};
    const auto& domains=lab::color::kFramePairInterpretations;
    lab::json m={{"kind","dlsslab-frame-pair-v1"},{"complete",true},{"same_call",true},{"outer_queue_complete",true},{"matched_host_return",true},
        {"frame",1},{"call",1},{"replay_complete",false},{"final_present_captured",false},{"independent_off_baseline",false},
        {"color_contract",{{"operator",3},{"exposure",1},{"matrices","identity"},{"invalid_rgb_guard",true}}},
        {"provenance",{{"origin","synthetic-viewer-review-no-game-no-NR"}}},{"stages",lab::json::array()}};
    std::array<std::vector<std::uint16_t>,4> pixels;for(auto& p:pixels)p.resize(w*h*4);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const float v=float(x)/(w-1),scale=y<h/3?.04f:y<h*2/3?1.f:8.f;
        std::array<float,4> rr{v*scale,(1-v)*scale,(.1f+.4f*v)*scale,1},input{},output{};
        for(auto& c:rr)c=lab::preconvert::unhalf(lab::preconvert::half(c));
        const float lo=std::min({rr[0],rr[1],rr[2]}),hi=std::max({rr[0],rr[1],rr[2]}),a=lo/(1+lo),b=hi/(1+hi),f=(b-a)*(hi==lo?1:1/(hi-lo));
        for(unsigned c=0;c<3;++c){const auto linear=lab::preconvert::unhalf(lab::preconvert::half(a+f*(rr[c]-lo)));
            input[c]=lab::preconvert::unhalf(lab::preconvert::half(float(lab::viewer::srgb_encode(linear))));
            output[c]=lab::preconvert::unhalf(lab::preconvert::half(input[c]*((x/32+y/24)%2?1.f:.9f)));}
        input[3]=output[3]=1;const std::array<std::array<float,4>,4> stages{rr,input,output,lab::viewer::reconstruct_for_view(output,rr)};
        for(unsigned s=0;s<4;++s)for(unsigned c=0;c<4;++c)pixels[s][(y*w+x)*4+c]=lab::preconvert::half(stages[s][c]);}
    for(unsigned s=0;s<4;++s){const auto file=pair/(std::string(roles[s])+".rgba16f");{std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(pixels[s].data()),pixels[s].size()*2);if(!out)throw std::runtime_error("Review fixture write");}
        m["stages"].push_back({{"stage",roles[s]},{"file",file.filename().string()},{"format","R16G16B16A16_FLOAT"},{"endianness","little"},{"subresource",0},
            {"width",w},{"height",h},{"row_pitch",w*8},{"bytes",w*h*8},{"sha256",lab::sha256(file)},{"color_interpretation",domains[s]}});}
    {std::ofstream out(pair/"manifest.json");out<<m.dump(2);if(!out)throw std::runtime_error("Review manifest write");}
    std::cout<<lab::json{{"purpose","functional-verification"},{"manifest",lab::utf8((pair/"manifest.json").wstring())},{"raw_bytes",w*h*8*4},{"nr_executed",false}}.dump()<<'\n';return 0;
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==2&&std::wstring(argv[1])==L"--review-fixture")return make_review_fixture();
    if(argc==2){lab::RawCapture raw(argv[1]);std::cout<<lab::json{{"width",raw.width()},{"height",raw.height()},{"mapped_bytes",raw.bytes()},{"manifest_sha256",raw.manifest_hash()},{"raw_pixel",raw.pixel(2,raw.width()/2,raw.height()/2)}}.dump()<<'\n';return 0;}
    const auto root=build_data_root()/("library-functional-"+std::to_string(GetCurrentProcessId()));
    const auto pair=root/"run"/"pair-1";std::filesystem::create_directories(pair);
    const char* stages[]{"rr-output","nr-input","nr-output","hdr-return"};lab::json m={{"kind","dlsslab-frame-pair-v1"},{"complete",true},{"same_call",true},{"outer_queue_complete",true},{"matched_host_return",true},{"frame",123},{"call",234},{"replay_complete",false},{"final_present_captured",false},{"independent_off_baseline",false},{"provenance",{{"origin","synthetic-library-no-NR"}}},{"stages",lab::json::array()}};
    const auto& interpretations=lab::color::kFramePairInterpretations;
    // 3 pixels per row, 8-byte padding. The final pixel is a HDR numeric test.
    for(const auto* stage:stages){const auto file=pair/(std::string(stage)+".rgba16f");std::ofstream out(file,std::ios::binary);std::array<std::uint16_t,16> row{};
        for(unsigned i=0;i<12;++i)row[i]=lab::preconvert::half(i==8?128.f:i==9?-.5f:i==10?INFINITY:1.f);for(unsigned y=0;y<2;++y)out.write(reinterpret_cast<const char*>(row.data()),32);out.close();
        m["stages"].push_back({{"stage",stage},{"file",file.filename().string()},{"format","R16G16B16A16_FLOAT"},{"endianness","little"},{"subresource",0},{"width",3},{"height",2},{"row_pitch",32},{"bytes",64},{"sha256",lab::sha256(file)}});}
    for(unsigned i=0;i<4;++i)m["stages"][i]["color_interpretation"]=interpretations[i];
    const auto manifest=pair/"manifest.json";auto write=[&](const lab::json& x){std::ofstream f(manifest);f<<x.dump(2);};write(m);
    auto need=[](bool v){if(!v)throw std::runtime_error("Capture-library assertion");};
    need(lab::capture_directory(manifest)==std::filesystem::canonical(pair));
    for(const auto& bad:{std::filesystem::path(L"relative/manifest.json"),pair/L"other.json",pair/L"missing"/L"manifest.json",pair/L".."/L"pair-1"/L"manifest.json",std::filesystem::path(L"\\\\server\\share\\manifest.json")}){
        bool rejected=false;try{lab::capture_directory(bad);}catch(...){rejected=true;}need(rejected);}
    {lab::RawCapture raw(manifest);need(raw.bytes()==256&&raw.row_pitch(0)==32);auto p=raw.pixel(2,2,1);need(p[0]==128&&p[1]==-.5f&&std::isinf(p[2]));
        bool rejected=false;try{raw.pixel(4,0,0);}catch(...){rejected=true;}need(rejected);
        lab::Handle locked(CreateFileW((pair/L"nr-output.rgba16f").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));need(!locked.valid());}
    const auto scan=lab::scan_capture_library(root);need(scan.entries.size()==1&&scan.entries[0].frame==123&&scan.entries[0].synthetic);
    for(unsigned variant=0;variant<5;++variant){auto bad=m;if(variant==0)bad["stages"][2]["sha256"]=std::string(64,'a');if(variant==1)bad["same_call"]=false;
        if(variant==2)bad["stages"][0]["file"]="../nr-input.rgba16f";if(variant==3)bad["stages"][0]["row_pitch"]=23;if(variant==4)bad["stages"][0]["width"]=8193;
        write(bad);bool rejected=false;try{lab::RawCapture invalid(manifest);}catch(...){rejected=true;}need(rejected);}
    write(m);lab::viewer::Display d;d.stage=2;auto pixel=lab::viewer::display_pixel({.5f,.25f,1,1},d);need(std::abs(pixel[0]-.5)<1e-6);
    d.mapping=2;d.hdr=1;d.white_nits=160;d.stage=0;pixel=lab::viewer::display_pixel({5,1,0,1},d);need(pixel[0]<10&&pixel[1]<2&&std::abs(pixel[0]/pixel[1]-5)<.0001);
    pixel=lab::viewer::display_pixel({.5f,.5f,.5f,1},d);need(pixel[0]==1);pixel=lab::viewer::display_pixel({600,600,600,1},d);need(pixel[0]<=d.peak_nits/80&&pixel[0]>d.peak_nits/80-.001);
    d.hdr=0;pixel=lab::viewer::display_pixel({INFINITY,0,0,1},d);need(pixel[0]==1&&pixel[1]==0&&pixel[2]==1);
    for(unsigned mapping=0;mapping<4;++mapping){lab::RawCapture raw(manifest);d.mapping=mapping;d.contrast=1.25f;const unsigned left=mapping==2?1:0,right=mapping==2?2:3;const auto before=raw.manifest_hash();const auto exported=lab::export_sdr_pair(raw,left,right,d,root.parent_path());
        std::ifstream recipe_file(exported/"display-recipe.json");const auto recipe=lab::json::parse(recipe_file);recipe_file.close();
        need(recipe["complete"]==true&&recipe["not_raw_evidence"]==true&&recipe["source_manifest_sha256"]==before&&lab::sha256(manifest)==before&&recipe["shared_display"]["mapping"]==mapping&&recipe["shared_display"]["contrast"]==1.25f);
        if(mapping==3)need(recipe["shared_display"]["transform_version"]=="hill-aces-fitted-sdr-v1"&&recipe["shared_display"]["contrast_applied"]==false);
        need(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)));{
            Microsoft::WRL::ComPtr<IWICImagingFactory> factory;need(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))));
            for(const auto* filename:{L"left-sdr.png",L"right-sdr.png"}){Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;need(SUCCEEDED(factory->CreateDecoderFromFilename((exported/filename).c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)));
                Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> image;need(SUCCEEDED(decoder->GetFrame(0,&image)));UINT w{},h{};image->GetSize(&w,&h);need(w==3&&h==2);
                Microsoft::WRL::ComPtr<IWICMetadataQueryReader> tags;need(SUCCEEDED(image->GetMetadataQueryReader(&tags)));PROPVARIANT intent{};need(SUCCEEDED(tags->GetMetadataByName(L"/sRGB/RenderingIntent",&intent))&&intent.vt==VT_UI1&&intent.bVal==1);PropVariantClear(&intent);
                Microsoft::WRL::ComPtr<IWICFormatConverter> convert;need(SUCCEEDED(factory->CreateFormatConverter(&convert)));need(SUCCEEDED(convert->Initialize(image.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)));
                std::array<BYTE,24> actual{};need(SUCCEEDED(convert->CopyPixels(nullptr,12,24,actual.data())));auto display=d;display.stage=filename==std::wstring_view(L"left-sdr.png")?left:right;const auto expected=lab::viewer::display_pixel(raw.pixel(display.stage,0,0),display);
                need(actual[0]==std::lround(expected[0]*255)&&actual[8]==255&&actual[9]==0&&actual[10]==255);}}
        CoUninitialize();for(const auto* name:{L"left-sdr.png",L"right-sdr.png",L"display-recipe.json"})need(std::filesystem::remove(exported/name));need(std::filesystem::remove(exported));}
    if(argc==3&&std::wstring(argv[1])==L"--viewer"){
        // Distinct uniform colors make a real GPU wipe check discriminative.
        m["provenance"]["origin"]="synthetic-viewer-distinct-stages";
        m["color_contract"]={{"operator",3},{"exposure",1},{"matrices","identity"},{"invalid_rgb_guard",true}};
        for(unsigned s=0;s<4;++s){const auto path=pair/(std::string(stages[s])+".rgba16f");std::array<std::uint16_t,32> pixels{};
            for(unsigned y=0;y<2;++y)for(unsigned x=0;x<3;++x){pixels[y*16+x*4+(s==2?1:0)]=lab::preconvert::half(1.f);pixels[y*16+x*4+3]=lab::preconvert::half(1.f);}
            {std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(pixels.data()),64);}m["stages"][s]["sha256"]=lab::sha256(path);}write(m);
        const auto viewer=std::filesystem::canonical(argv[2]);need(viewer.filename()==L"overglaze_viewer.exe"&&viewer.parent_path()==build_directory());const auto report=root/"viewer-ui.json";
        auto command=L"\""+viewer.wstring()+L"\" --ui-test --pair \""+manifest.wstring()+L"\" --report \""+report.wstring()+L"\"";
        STARTUPINFOW start{sizeof(start)};start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};need(CreateProcessW(viewer.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,viewer.parent_path().c_str(),&start,&process)!=FALSE);
        lab::Handle thread(process.hThread),child(process.hProcess);need(WaitForSingleObject(child.value,35000)==WAIT_OBJECT_0);DWORD code{};need(GetExitCodeProcess(child.value,&code)&&code==0);
        std::ifstream report_file(report);const auto result=lab::json::parse(report_file);report_file.close();need(result["passed"]==true&&result["ui_steps_passed"]==22&&result["wipe_pixel_checks"].size()==2&&result["capture_folder_requested"]==lab::utf8(pair.wstring())&&result["explorer_suppressed_for_test"]==true&&result["display_preferences"]["mapping"]==3);
        const auto exported=std::filesystem::path(lab::wide(result.at("derived_export").get<std::string>()));need(exported.parent_path()==root.parent_path()&&exported.filename().wstring().rfind(L"preview-export-",0)==0);
        {std::ifstream f(exported/"display-recipe.json");const auto recipe=lab::json::parse(f);
            need(recipe["kind"]=="dlsslab-derived-sdr-preview-v4"&&recipe["rr_reference"]["sha256"]==m["stages"][0]["sha256"]&&recipe["shared_display"]["reconstruction_before_display_curve"]==true);
            for(const auto& side:recipe["images"])need(side["effective_display"]["view_reconstruction_applied"]==true&&side["effective_display"]["pure_nr_output_view"]==false);}
        for(const auto* name:{L"left-sdr.png",L"right-sdr.png",L"display-recipe.json"})need(std::filesystem::remove(exported/name));need(std::filesystem::remove(exported));
        // Keep only the small GUI interaction receipt, outside the cleaned fixture.
        std::filesystem::rename(report,root.parent_path()/(root.filename().wstring()+L"-viewer-ui.json"));
    }
    // Remove only files created by this test, after all mappings are closed.
    for(const auto* name:stages)std::filesystem::remove(pair/(std::string(name)+".rgba16f"));std::filesystem::remove(manifest);std::filesystem::remove(pair);std::filesystem::remove(pair.parent_path());std::filesystem::remove(root);
    std::cout<<"PASS immutable mappings, padded rows, HDR/nonfinite values, hash/scope rejection and bounded catalog; temporary synthetic files removed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
