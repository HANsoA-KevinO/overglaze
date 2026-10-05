// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_preview_export.hpp"
#include "lab_png_color_tags.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
namespace lab {
using Microsoft::WRL::ComPtr;
namespace {
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("SDR PNG encoder HRESULT="+std::to_string(unsigned(r)));}
struct ComScope {ComScope(){hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));}~ComScope(){CoUninitialize();}};
void write_new(const std::filesystem::path& path,const void* bytes,std::size_t size){need(size<=256ULL*1024*1024,"Encoded preview exceeds 256 MiB");Handle f(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));check(f.valid(),"Create new derived preview");
    DWORD written=0;check(WriteFile(f.value,bytes,static_cast<DWORD>(size),&written,nullptr)&&written==size,"Write derived preview");check(FlushFileBuffers(f.value)!=0,"Flush derived preview");}
// Exports go only into a Lab data root: an existing directory named "data", on
// a local fixed disk, spelled as a normal X:\ path, with no reparse point on the
// way. The viewer passes the root it resolved from its own location rather than
// one compiled path, so a copied installation exports into its own data and
// never into another tree.
bool export_root_ok(const std::filesystem::path& root){
    const auto& s=root.native();
    if(!(root.is_absolute()&&root==root.lexically_normal()&&s.size()>3&&s[1]==L':'&&(s[2]==L'\\'||s[2]==L'/')&&s.find(L':',2)==std::wstring::npos))return false;
    if(_wcsicmp(root.filename().c_str(),L"data")!=0)return false;
    const std::wstring drive{s[0],L':',L'\\'};if(GetDriveTypeW(drive.c_str())!=DRIVE_FIXED)return false;
    for(auto p=root;;){const auto a=GetFileAttributesW(p.c_str());if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        const auto parent=p.parent_path();if(parent==p)break;p=parent;}
    return std::filesystem::is_directory(root);}
}
std::filesystem::path export_sdr_pair(const RawCapture& capture,int left,int right,viewer::Display display,const std::filesystem::path& root){
    need(left>=0&&left<int(capture.stage_count())&&right>=0&&right<int(capture.stage_count())&&export_root_ok(root),"Invalid derived export scope");
    need(std::isfinite(display.exposure)&&display.exposure>=-6&&display.exposure<=6&&display.curve<=2&&display.nr_linear<=1&&display.mapping<=3&&std::isfinite(display.contrast)&&display.contrast>=.75f&&display.contrast<=1.5f,"Invalid display recipe");
    display.hdr=0;
    // Resolve both before creating output. Missing legacy metadata stays unknown.
    const auto lc=capture.color_contract(left),rc=capture.color_contract(right);
    const auto left_recipe=color::recipe(lc,display),right_recipe=color::recipe(rc,display);
    need(color::comparable(lc,rc)||(capture.display_pair()&&left==0&&right==1),"Cross-domain pair is not a comparable NR experiment");
    need(std::filesystem::space(root).available>=(30ULL<<30)+(512ULL<<20),"Export needs 30 GiB reserve plus 512 MiB allowance");ComScope com;
    auto id=uuid();id.erase(std::remove(id.begin(),id.end(),'{'),id.end());id.erase(std::remove(id.begin(),id.end(),'}'),id.end());
    const auto directory=root/("preview-export-"+id);need(std::filesystem::create_directory(directory),"Export directory must be new");
    json recipe={{"kind","dlsslab-derived-sdr-preview-v4"},{"complete",false},{"source",utf8(capture.path().wstring())},{"source_manifest_sha256",capture.manifest_hash()},
        {"originals_modified",false},{"not_raw_evidence",true},{"not_game_final_grade",true},{"not_hdr_image",true},
        {"shared_display",{{"mapping",display.mapping},{"transform_version","toe-linear-shoulder-maxrgb-v1"},{"contrast",display.contrast},{"curve",display.curve},{"exposure_ev",display.exposure},{"nr_interpreted_linear",display.nr_linear!=0},{"nr_stages_bypass_tone_curve",true},{"output_encoding","sRGB"},{"source_primaries_assumption","Rec.709/D65; unverified"},{"sdr_peak_relative",1}}},{"images",json::array()}};
    ComPtr<IWICImagingFactory> factory;hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    auto& shared=recipe["shared_display"];
    shared["transform_version"]=display.mapping==3?"hill-aces-fitted-sdr-v1":display.mapping==0?"toe-linear-shoulder-maxrgb-v1":display.mapping==1?"domain-display-v1":"legacy-curve-v1";
    shared["contrast_applied"]=display.mapping==0;
    shared["display_transform_applied_to_nr_proxy"]=false;
    shared["official_aces_reference_transform"]=false;
    recipe["color_contract_version"]="dlsslab-color-contract-v1";
    recipe["shared_display_is_request_only"]=true;
    recipe["raw_difference_allowed"]=color::comparable(lc,rc);
    recipe["pair_scope"]=capture.display_pair()?"candidate-interval-not-proven-same-frame":"recorded-same-NR-call";
    recipe["display_pair_association"]=capture.manifest().value("association",json(nullptr));
    recipe["producer"]=module_identity(nullptr);
    recipe["render_pipeline_modified"]=false;recipe["new_nr_evaluates"]=0;
    recipe["view_reconstruction_requested"]=display.nr_reconstruct!=0;
    shared["nr_stages_bypass_tone_curve"]=!(left_recipe.at("view_reconstruction_applied")==true||right_recipe.at("view_reconstruction_applied")==true);
    shared["reconstruction_before_display_curve"]=!shared["nr_stages_bypass_tone_curve"].get<bool>();
    if(left_recipe.at("view_reconstruction_applied")==true||right_recipe.at("view_reconstruction_applied")==true)
        recipe["rr_reference"]={{"stage","rr-output"},{"file",capture.manifest().at("stages").at(0).at("file")},{"sha256",capture.manifest().at("stages").at(0).at("sha256")},{"purpose","display-only-reference-not-model-input-modification"}};
    for(unsigned side=0;side<2;++side){const unsigned stage=side?right:left;const auto source=capture.color_contract(stage);const auto applied=color::effective(source,display);
        ComPtr<IStream> stream;hr(CreateStreamOnHGlobal(nullptr,TRUE,&stream));ComPtr<IWICBitmapEncoder> encoder;hr(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;hr(encoder->CreateNewFrame(&frame,nullptr));hr(frame->Initialize(nullptr));hr(frame->SetSize(capture.width(),capture.height()));hr(frame->SetResolution(96,96));
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;hr(frame->SetPixelFormat(&format));need(format==GUID_WICPixelFormat32bppBGRA,"Unexpected PNG pixel format");
        // PNG SetColorContexts requires an ICC profile, not an EXIF context.
        // Declare actual encoded sRGB using the native PNG metadata chunk.
        if(!source.final()){ComPtr<IWICMetadataQueryWriter> metadata;hr(frame->GetMetadataQueryWriter(&metadata));PROPVARIANT intent{};intent.vt=VT_UI1;intent.bVal=1;hr(metadata->SetMetadataByName(L"/sRGB/RenderingIntent",&intent));}
        std::vector<std::uint8_t> row(capture.width()*4);
        for(unsigned y=0;y<capture.height();++y){for(unsigned x=0;x<capture.width();++x){const auto reference=applied.nr_reconstruct?std::optional(capture.pixel(0,x,y)):std::nullopt;const auto p=viewer::display_pixel(capture.pixel(stage,x,y),applied,reference);for(unsigned c=0;c<3;++c)row[x*4+2-c]=static_cast<std::uint8_t>(std::lround(std::clamp(p[c],0.f,1.f)*255));row[x*4+3]=255;}
            hr(frame->WritePixels(1,static_cast<UINT>(row.size()),static_cast<UINT>(row.size()),row.data()));}
        hr(frame->Commit());hr(encoder->Commit());STATSTG stat{};hr(stream->Stat(&stat,STATFLAG_NONAME));need(stat.cbSize.QuadPart<=256ULL*1024*1024,"Encoded PNG exceeds budget");HGLOBAL memory{};hr(GetHGlobalFromStream(stream.Get(),&memory));
        void* bytes=GlobalLock(memory);need(bytes!=nullptr,"Encoded PNG memory unavailable");const auto path=directory/(side?"right-sdr.png":"left-sdr.png");try{
            if(source.final()){const auto tagged=color::tag_g22_p709(bytes,static_cast<std::size_t>(stat.cbSize.QuadPart));write_new(path,tagged.data(),tagged.size());}
            else write_new(path,bytes,static_cast<std::size_t>(stat.cbSize.QuadPart));
        }catch(...){GlobalUnlock(memory);throw;}GlobalUnlock(memory);
        recipe["images"].push_back({{"file",path.filename().string()},{"stage",capture.manifest().at("stages")[stage].at("stage")},{"source_sha256",capture.manifest().at("stages")[stage].at("sha256")},{"sha256",sha256(path)},{"bytes",std::filesystem::file_size(path)},{"width",capture.width()},{"height",capture.height()},{"effective_display",side?right_recipe:left_recipe}});
    }
    recipe["complete"]=true;const auto text=recipe.dump(2);write_new(directory/"display-recipe.json",text.data(),text.size());return directory;
}
}
