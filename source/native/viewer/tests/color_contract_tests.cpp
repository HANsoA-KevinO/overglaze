// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_capture_library.hpp"
#include "lab_preview_export.hpp"
#include "lab_preconvert_reference.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
namespace {
void need(bool b,const char* why){if(!b)throw std::runtime_error(why);}
template<class F> void rejects(F f){bool rejected=false;try{f();}catch(...){rejected=true;}need(rejected,"Negative contract case accepted");}
void write(const std::filesystem::path& p,const void* bytes,std::size_t n){need(!std::filesystem::exists(p),"New test file required");std::ofstream f(p,std::ios::binary);f.write(static_cast<const char*>(bytes),n);need(bool(f),"Test write");}
void write(const std::filesystem::path& p,const lab::json& j){const auto s=j.dump(2);write(p,s.data(),s.size());}
lab::json read(const std::filesystem::path& p){std::ifstream f(p);return lab::json::parse(f);}
}
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
int wmain(int argc,wchar_t** argv){try{
    using namespace lab;unsigned checks=0;
    {viewer::Display saved;viewer::viewing_preset(saved,0);saved.exposure=-1.25f;saved.nr_reconstruct=1;
        const auto expected=saved;for(unsigned i=0;i<20;++i){const auto off=viewer::viewing_request(saved,false),on=viewer::viewing_request(saved,true);
            need(off.mapping==1&&off.exposure==0&&off.contrast==1&&off.nr_reconstruct==0,"Bypass does not reconstruct, expose or tone-map");
            need(on.mapping==expected.mapping&&on.exposure==expected.exposure&&on.contrast==expected.contrast&&on.nr_reconstruct==1,"Re-enable restores the exact shared settings");checks+=2;}
        viewer::viewing_preset(saved,1);need(saved.mapping==3&&saved.contrast==1&&saved.exposure==0&&saved.nr_reconstruct==1,"Preset changes only viewing controls, preserves explicit assist choice");
        viewer::Display fresh;viewer::viewing_preset(fresh,0);need(fresh.contrast==1.15f&&fresh.nr_reconstruct==0,"Natural preset never opts into RR assistance");
        rejects([&]{viewer::viewing_preset(fresh,2);});checks+=3;}
    json m={{"kind","dlsslab-frame-pair-v1"},{"stages",json::array()}};
    const char* names[]{"rr-output","nr-input","nr-output","hdr-return"};
    const auto& domains=color::kFramePairInterpretations;
    for(unsigned i=0;i<4;++i){m["stages"].push_back({{"stage",names[i]},{"format","R16G16B16A16_FLOAT"},{"color_interpretation",domains[i]}});
        const auto c=color::resolve(m,i);need(c.known()&&unsigned(c.stage)==i,"Exact known contract");
        auto old=m;old["stages"][i].erase("color_interpretation");need(!color::resolve(old,i).known(),"Legacy data cannot gain color evidence");
        rejects([&]{color::effective(color::resolve(old,i),{});});
        old=m;old["stages"][i]["color_interpretation"]="HDR";need(!color::resolve(old,i).known(),"Format/HDR label cannot establish color contract");checks+=4;}
    // Captures written before the neutral names still open, and only at their own stage.
    {auto legacy=m;legacy["stages"][1]["color_interpretation"]=color::kLegacyFramePairInput;legacy["stages"][3]["color_interpretation"]=color::kLegacyFramePairReturn;
     need(unsigned(color::resolve(legacy,1).stage)==1&&unsigned(color::resolve(legacy,3).stage)==3,"Legacy interpretation spellings read as aliases");
     auto swapped=m;swapped["stages"][1]["color_interpretation"]=color::kLegacyFramePairReturn;swapped["stages"][3]["color_interpretation"]=color::kLegacyFramePairInput;
     need(!color::resolve(swapped,1).known()&&!color::resolve(swapped,3).known(),"A legacy spelling names only its own stage");checks+=2;}
    need(color::comparable(color::resolve(m,0),color::resolve(m,3))&&color::comparable(color::resolve(m,1),color::resolve(m,2))&&!color::comparable(color::resolve(m,0),color::resolve(m,1)),"Cross-domain differences refused");++checks;
    for(unsigned i:{1u,2u})for(unsigned mapping=0;mapping<4;++mapping){viewer::Display request;request.mapping=mapping;request.contrast=1.5;auto before=request;
        const auto d=color::effective(color::resolve(m,i),request);need(d.mapping==1&&request.mapping==before.mapping&&request.contrast==before.contrast,"Viewing requests are immutable and NR bypasses ACES");
        const auto raw=std::array<float,4>{.18f,.5f,.8f,.3f};const auto out=viewer::display_pixel(raw,d);for(unsigned c=0;c<3;++c)need(std::abs(out[c]-raw[c])<1e-6,"Interface default code view");checks+=2;}
    auto invalid=viewer::Display{};invalid.exposure=NAN;rejects([&]{color::effective(color::resolve(m,0),invalid);});++checks;
    auto supported=m;supported["same_call"]=true;supported["complete"]=true;
    supported["color_contract"]={{"operator",3},{"exposure",1},{"matrices","identity"},{"invalid_rgb_guard",true}};
    for(auto& row:supported["stages"]){row["width"]=3;row["height"]=2;}
    viewer::Display processing;processing.nr_reconstruct=1;processing.mapping=3;
    need(color::resolve(supported,2).reconstruction_available,"Exact display reference admission");
    need(color::effective(color::resolve(supported,2),processing).mapping==3,"Explicit reconstructed working view can use ACES");
    need(color::recipe(color::resolve(supported,2),processing).at("view_reconstruction_applied")==true,"Derived view labelled");
    rejects([&]{viewer::display_pixel({.5f,.5f,.5f,1},color::effective(color::resolve(supported,2),processing));});checks+=4;
    for(unsigned variant=0;variant<8;++variant){auto missing=supported;
        if(variant==0)missing.erase("color_contract");if(variant==1)missing["color_contract"]["operator"]=2;
        if(variant==2)missing["color_contract"]["exposure"]=2;if(variant==3)missing["color_contract"]["matrices"]="unknown";
        if(variant==4)missing["color_contract"]["invalid_rgb_guard"]=false;if(variant==5)missing["same_call"]=false;
        if(variant==6)missing["stages"][0]["width"]=4;if(variant==7)missing["stages"][0]["color_interpretation"]="HDR";
        need(!color::resolve(missing,2).reconstruction_available,"Missing/wrong reference rejected");
        rejects([&]{color::effective(color::resolve(missing,2),processing);});
        need(color::effective(color::resolve(missing,2),{}).mapping==1,"Unsupported record retains raw view");checks+=3;}
    const auto rr_reference=std::array<float,4>{1,1,1,.25f};
    const auto restored=viewer::reconstruct_for_view({.5f,.5f,.5f,1},rr_reference);
    need(restored[0]==preconvert::unhalf(preconvert::half(float((viewer::srgb_decode(.5)+1e-6)/(.5+1e-6))))&&restored[3]==.25f,"Known neutral restoration and alpha");++checks;
    for(float bad:{-.001f,INFINITY,NAN,65505.f}){need(viewer::reconstruct_for_view({.5f,.5f,bad,1},rr_reference)==rr_reference,"Whole-pixel guard fallback");++checks;}
    auto off=processing;off.nr_reconstruct=0;need(color::recipe(color::resolve(supported,2),off).at("pure_nr_output_view")==true,"Toggle back to raw label");++checks;
    const auto root=build_data_root()/("color-contract-functional-"+std::to_string(GetCurrentProcessId()));
    need(std::filesystem::create_directory(root),"New fixture root required");const auto pair=root/"run"/"display-pair-1";std::filesystem::create_directories(pair);
    json fixture={{"kind","dlsslab-display-pair-v1"},{"complete",true},{"frame",123},{"call",234},{"nr_off",true},{"nr_executed",false},
        {"presentation",{{"return","S_OK"},{"color_space",0}}},{"association",{{"candidate",true},{"same_native_queue",true},{"rr_submission_before_display_copy",true},{"unique_admitted_rr_in_interval",true},{"same_frame_proven",false}}},
        {"provenance",{{"origin","synthetic-color-contract-no-NR"}}},{"stages",json::array()}};
    constexpr unsigned w=256,h=2,rr_pitch=w*8+8,sdr_pitch=w*4+8;
    std::vector<std::uint16_t> rr(rr_pitch*h/2);for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<4;++c)rr[y*rr_pitch/2+x*4+c]=preconvert::half(c==3?1.f:x/32.f);
    write(pair/"rr-output.rgba16f",rr.data(),rr.size()*2);
    for(unsigned i=0;i<2;++i)fixture["stages"].push_back({{"stage",i?"pre-present-sdr":"rr-output"},{"file",i?"display-output.raw":"rr-output.rgba16f"},{"dxgi_format",i?28:10},{"endianness","little"},{"subresource",0},{"width",w},{"height",h},{"row_pitch",i?sdr_pitch:rr_pitch},{"bytes",(i?sdr_pitch:rr_pitch)*h},{"fence_value",1},{"completed_value",1},{"interpretation",i?"raw-UNORM-display-codes-no-additional-gamma":"RR-working-values-no-display-transform"}});
    fixture["stages"][0]["sha256"]=sha256(pair/"rr-output.rgba16f");
    json receipts=json::array(),ui_receipt=nullptr;need(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"COM");
    for(unsigned format:{28u,87u}){
        std::vector<std::uint8_t> sdr(sdr_pitch*h,0xAB);for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){auto* p=sdr.data()+y*sdr_pitch+x*4;p[format==87?2:0]=std::uint8_t(x);p[1]=std::uint8_t(255-x);p[format==87?0:2]=std::uint8_t((x*7)&255);p[3]=std::uint8_t(y);}
        write(pair/"display-output.raw",sdr.data(),sdr.size());fixture["stages"][1]["dxgi_format"]=format;fixture["stages"][1]["sha256"]=sha256(pair/"display-output.raw");
        const auto manifest=pair/"manifest.json";write(manifest,fixture);
        {RawCapture raw(manifest);need(raw.stage_count()==2&&raw.display_pair()&&raw.format(1)==format,"Display pair layout");
            need(scan_capture_library(root).entries.size()==1,"Display pair catalog entry");
            const auto before=sha256(manifest);const auto c=raw.color_contract(1);need(c.final()&&!color::comparable(raw.color_contract(0),c),"Final code domain");
            for(float ev:{-6.f,6.f}){viewer::Display request;request.exposure=ev;request.mapping=3;request.nr_linear=1;request.contrast=1.5;
                const auto applied=color::effective(c,request);need(applied.exposure==0&&applied.mapping==1&&applied.nr_linear==0,"Final identity ignores controls");
                for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const auto bits=raw.bits(1,x,y);const auto p=viewer::display_pixel(raw.pixel(1,x,y),applied);need(bits[0]==x&&bits[1]==255-x&&bits[2]==((x*7)&255),"BGRA decode / row pitch");for(unsigned k=0;k<3;++k)need(std::lround(p[k]*255)==bits[k],"Final all-code identity");}
                const auto exported=export_sdr_pair(raw,0,1,request,root.parent_path());const auto result=read(exported/"display-recipe.json");
                need(result["images"][1]["effective_display"]["identity_rgb_codes"]==true&&result["images"][1]["effective_display"]["exposure_ev"]==0&&result["images"][0]["effective_display"]["exposure_ev"]==ev&&result["raw_difference_allowed"]==false,"Actual versus requested contract");
                {Microsoft::WRL::ComPtr<IWICImagingFactory> factory;need(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))),"WIC factory");Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
                    need(SUCCEEDED(factory->CreateDecoderFromFilename((exported/L"right-sdr.png").c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)),"Final PNG decode");Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;need(SUCCEEDED(decoder->GetFrame(0,&frame)),"PNG frame");
                    Microsoft::WRL::ComPtr<IWICFormatConverter> convert;need(SUCCEEDED(factory->CreateFormatConverter(&convert)),"PNG converter");need(SUCCEEDED(convert->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)),"PNG layout");std::vector<BYTE> pixels(w*h*4);need(SUCCEEDED(convert->CopyPixels(nullptr,w*4,UINT(pixels.size()),pixels.data())),"PNG pixels");
                    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const auto b=raw.bits(1,x,y);for(unsigned k=0;k<3;++k)need(pixels[(y*w+x)*4+k]==b[k],"PNG roundtrip all RGB codes");}
                    Microsoft::WRL::ComPtr<IWICMetadataQueryReader> tags;need(SUCCEEDED(frame->GetMetadataQueryReader(&tags)),"PNG tags");PROPVARIANT v{};need(FAILED(tags->GetMetadataByName(L"/sRGB/RenderingIntent",&v)),"G22 image must not claim sRGB");PropVariantClear(&v);need(SUCCEEDED(tags->GetMetadataByName(L"/gAMA/ImageGamma",&v))&&v.ulVal==45455,"G22 gamma declaration");PropVariantClear(&v);}
                receipts.push_back(result);for(const auto* name:{"left-sdr.png","right-sdr.png","display-recipe.json"})need(std::filesystem::remove(exported/name),"Exact functional export cleanup");need(std::filesystem::remove(exported),"Empty export cleanup");checks+=8;
            }
            need(sha256(manifest)==before&&sha256(pair/"display-output.raw")==fixture["stages"][1]["sha256"].get<std::string>(),"Originals unchanged");
            viewer::Display hdr;hdr.hdr=1;rejects([&]{color::effective(c,hdr);});rejects([&]{raw.pixel(2,0,0);});checks+=3;
        }
        if(format==28&&argc==3&&std::wstring(argv[1])==L"--viewer"){
            const auto viewer=std::filesystem::canonical(argv[2]);need(viewer.filename()==L"overglaze_viewer.exe"&&viewer.parent_path()==build_directory(),"Bounded viewer path");
            const auto report=root/"viewer-ui.json";auto command=L"\""+viewer.wstring()+L"\" --ui-test --pair \""+manifest.wstring()+L"\" --report \""+report.wstring()+L"\"";
            STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
            need(CreateProcessW(viewer.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,viewer.parent_path().c_str(),&startup,&process)!=FALSE,"Start isolated viewer test");Handle thread(process.hThread),child(process.hProcess);
            need(WaitForSingleObject(child.value,35000)==WAIT_OBJECT_0,"Viewer test timeout");DWORD exit_code{};need(GetExitCodeProcess(child.value,&exit_code)&&exit_code==0,"Viewer test failed");
            ui_receipt=read(report);need(ui_receipt["passed"]==true&&ui_receipt["ui_steps_passed"]==8&&ui_receipt["final_sdr_code_checks"].size()>=3,"Final pair UI contract checks");
            const auto exported=std::filesystem::path(wide(ui_receipt.at("derived_export").get<std::string>()));need(exported.parent_path()==root.parent_path()&&exported.filename().wstring().rfind(L"preview-export-",0)==0,"Fixture export scope");
            for(const auto* name:{"left-sdr.png","right-sdr.png","display-recipe.json"})need(std::filesystem::remove(exported/name),"UI functional export cleanup");need(std::filesystem::remove(exported),"UI empty export cleanup");
            need(std::filesystem::remove(report),"Move small UI receipt into result");++checks;
        }
        for(unsigned variant=0;variant<4;++variant){auto bad=fixture;if(variant==0)bad["presentation"]["color_space"]=12;if(variant==1)bad["stages"][1]["completed_value"]=0;if(variant==2)bad["nr_off"]=false;if(variant==3)bad["stages"][1]["dxgi_format"]=24;
            need(std::filesystem::remove(manifest),"Replace synthetic manifest only");write(manifest,bad);rejects([&]{RawCapture invalid(manifest);});++checks;}
        need(std::filesystem::remove(manifest),"Synthetic manifest cleanup");need(std::filesystem::remove(pair/"display-output.raw"),"Synthetic SDR cleanup");
    }
    CoUninitialize();need(std::filesystem::remove(pair/"rr-output.rgba16f"),"Synthetic RR cleanup");need(std::filesystem::remove(pair)&&std::filesystem::remove(pair.parent_path())&&std::filesystem::remove(root),"Empty fixture cleanup");
    const json result={{"passed",true},{"purpose","functional-verification"},{"checks",checks},{"all_256_codes_verified",true},{"nr_evaluates",0},{"game_started",false},{"temporary_raw_removed",true},{"exports",receipts}};
    if(argc==2)write(argv[1],result);
    if(!ui_receipt.is_null())write(root.parent_path()/(root.filename().wstring()+L"-ui.json"),ui_receipt);
    std::cout<<"PASS "<<checks<<" color contract checks, padded RGBA/BGRA all-code PNG roundtrip; 0 NR; functional files removed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
