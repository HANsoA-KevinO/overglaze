// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_capture_library.hpp"
#include "lab_preconvert_reference.hpp"
#include <algorithm>
#include <cstring>
namespace lab {
namespace {
void need(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
void no_reparse(std::filesystem::path p){for(;!p.empty();){const auto attr=GetFileAttributesW(p.c_str());need(attr!=INVALID_FILE_ATTRIBUTES&&!(attr&FILE_ATTRIBUTE_REPARSE_POINT),"Capture path missing or contains reparse points");auto parent=p.parent_path();if(parent==p)break;p=parent;}}
struct LockedFile {
    Handle file;Handle mapping;void* view=nullptr;std::uint64_t size=0;
    explicit LockedFile(const std::filesystem::path& p,std::uint64_t limit){no_reparse(p);file.value=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);check(file.valid(),"Open immutable capture");
        LARGE_INTEGER n{};check(GetFileSizeEx(file.value,&n)!=FALSE,"Capture size");need(n.QuadPart>0&&std::uint64_t(n.QuadPart)<=limit,"Capture size outside budget");size=n.QuadPart;
        mapping.value=CreateFileMappingW(file.value,nullptr,PAGE_READONLY,0,0,nullptr);check(mapping.valid(),"Read-only capture mapping");view=MapViewOfFile(mapping.value,FILE_MAP_READ,0,0,0);check(view!=nullptr,"Read-only capture view");}
    ~LockedFile(){if(view)UnmapViewOfFile(view);}
};
json read_manifest(const std::filesystem::path& p){LockedFile file(p,65536);return json::parse(static_cast<const char*>(file.view),static_cast<const char*>(file.view)+file.size);}
bool scope(const json& m){return m.value("kind","")=="dlsslab-frame-pair-v1"&&m.value("complete",false)&&m.value("same_call",false)&&m.value("outer_queue_complete",false)&&m.value("matched_host_return",false)&&m.value("frame",0ULL)&&m.value("call",0ULL)&&!m.value("replay_complete",true)&&!m.value("final_present_captured",true)&&!m.value("independent_off_baseline",true);}
bool display_scope(const json& m){
    if(m.value("kind","")!="dlsslab-display-pair-v1"||!m.value("complete",false)||!m.value("nr_off",false)||m.value("nr_executed",true)||!m.value("frame",0ULL)||!m.value("call",0ULL))return false;
    const auto p=m.value("presentation",json::object()),a=m.value("association",json::object());
    return p.value("return","")=="S_OK"&&p.value("color_space",~0u)==0&&a.value("candidate",false)&&a.value("same_native_queue",false)&&a.value("rr_submission_before_display_copy",false)&&a.value("unique_admitted_rr_in_interval",false);
}
}
std::filesystem::path capture_directory(const std::filesystem::path& manifest){
    need(manifest.is_absolute()&&manifest==manifest.lexically_normal()&&manifest.filename()==L"manifest.json"&&manifest.root_name().wstring().size()==2,"An absolute local capture location is required");
    const auto folder=manifest.parent_path();no_reparse(folder);need(std::filesystem::is_directory(folder),"Capture directory no longer exists");
    return std::filesystem::canonical(folder);
}
CaptureCatalog scan_capture_library(const std::filesystem::path& root){
    no_reparse(root);CaptureCatalog out;std::size_t visited=0;
    for(const auto& run:std::filesystem::directory_iterator(root)){
        if(++visited>20000){out.truncated=true;break;}if(!run.is_directory()||(GetFileAttributesW(run.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT))continue;
        for(const auto& pair:std::filesystem::directory_iterator(run.path(),std::filesystem::directory_options::skip_permission_denied)){
            if(++visited>20000||out.entries.size()>=5000){out.truncated=true;break;}
            const auto pair_name=pair.path().filename().wstring();
            if(!pair.is_directory()||(pair_name.rfind(L"pair-",0)!=0&&pair_name.rfind(L"display-pair-",0)!=0)||(GetFileAttributesW(pair.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT))continue;
            const auto path=pair.path()/L"manifest.json";if(!std::filesystem::is_regular_file(path))continue;
            CaptureEntry e;e.manifest=path;e.run=utf8(run.path().filename().wstring());e.pair=utf8(pair.path().filename().wstring());
            WIN32_FILE_ATTRIBUTE_DATA attributes{};if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes))e.modified=(std::uint64_t(attributes.ftLastWriteTime.dwHighDateTime)<<32)|attributes.ftLastWriteTime.dwLowDateTime;
            try{const auto m=read_manifest(path);const bool display=display_scope(m);need(scope(m)||display,"Incomplete or unknown capture evidence");const auto& s=m.at("stages");need(s.is_array()&&s.size()==(display?2:4),"Unexpected stage count");
                e.frame=m.at("frame");e.call=m.at("call");e.origin=m.value("provenance",json::object()).value("origin","unknown");e.synthetic=e.origin.find("synthetic")!=std::string::npos;
                e.width=s[0].at("width");e.height=s[0].at("height");for(const auto& stage:s)e.bytes+=stage.at("bytes").get<std::uint64_t>();
                const auto provenance=m.value("provenance",json::object());const auto app=provenance.value("application",json::object());
                const auto app_path=app.value("path","");if(!app_path.empty())e.application=utf8(std::filesystem::path(wide(app_path)).filename().wstring());
                if(m.contains("settings")){const auto& values=m.at("settings");if(values.contains("tone")&&values.contains("structure"))e.parameters="T "+values.at("tone").dump()+" / S "+values.at("structure").dump();}
                for(const auto& row:s){const auto file=std::filesystem::path(wide(row.at("file").get<std::string>()));need(file==file.filename(),"Stage file must be local");need(std::filesystem::is_regular_file(pair.path()/file),"Raw files missing or previously cleaned");}}
            catch(const std::exception& x){e.error=x.what();++out.skipped;}
            out.entries.push_back(std::move(e));
        }if(out.truncated)break;
    }
    std::sort(out.entries.begin(),out.entries.end(),[](const auto& a,const auto& b){return a.modified!=b.modified?a.modified>b.modified:a.manifest.native()>b.manifest.native();});return out;
}
struct RawCapture::Impl {std::filesystem::path path;json metadata;std::string hash;std::unique_ptr<LockedFile> manifest;std::array<std::unique_ptr<LockedFile>,4> stages;std::array<unsigned,4> pitch{},formats{};unsigned width=0,height=0,count=4;bool display=false;std::uint64_t bytes=0;};
RawCapture::RawCapture(const std::filesystem::path& source):p_(std::make_unique<Impl>()){
    need(source.is_absolute()&&source.filename()==L"manifest.json","An absolute capture manifest is required");no_reparse(source);auto& s=*p_;s.path=std::filesystem::canonical(source);s.manifest=std::make_unique<LockedFile>(s.path,65536);
    s.metadata=json::parse(static_cast<const char*>(s.manifest->view),static_cast<const char*>(s.manifest->view)+s.manifest->size);s.display=display_scope(s.metadata);need(scope(s.metadata)||s.display,"Capture lacks supported evidence scope");s.hash=sha256(s.path);s.count=s.display?2:4;
    const auto& rows=s.metadata.at("stages");need(rows.is_array()&&rows.size()==s.count,"Unexpected capture stage count");
    const char* names[]{"rr-output","nr-input","nr-output","hdr-return"};
    for(unsigned i=0;i<s.count;++i){const auto& row=rows[i];const bool final=s.display&&i==1;const std::string name=final?"display-output.raw":std::string(names[i])+".rgba16f";
        need(row.at("stage")== (final?"pre-present-sdr":names[i])&&row.at("file")==name&&row.at("endianness")=="little"&&row.at("subresource")==0,"Unknown raw stage layout");
        const unsigned format=s.display?row.at("dxgi_format").get<unsigned>():10;
        if(s.display){need((!final&&format==10)||(final&&(format==28||format==87)),"Display viewer supports FP16 RR and RGB/BGRA8 SDR; other formats remain unsupported");
            const auto fence=row.at("fence_value").get<std::uint64_t>();need(fence&&row.at("completed_value").get<std::uint64_t>()>=fence,"Incomplete raw copy fence");}
        else need(row.at("format")=="R16G16B16A16_FLOAT","Unknown raw stage format");s.formats[i]=format;
        const unsigned bpp=final?4:8;
        const auto w=row.at("width").get<unsigned>(),h=row.at("height").get<unsigned>(),pitch=row.at("row_pitch").get<unsigned>();
        need(w&&h&&w<=8192&&h<=8192&&std::uint64_t(w)*h<=24000000&&pitch>=w*bpp&&pitch<=w*bpp+1024&&pitch%2==0,"Capture dimensions/row pitch exceed viewer budget");
        if(!i){s.width=w;s.height=h;}need(w==s.width&&h==s.height,"Capture stage dimensions differ");
        const auto bytes=std::uint64_t(pitch)*h;s.bytes+=bytes;need(s.bytes<=1024ULL*1024*1024&&row.at("bytes")==bytes,"Capture package exceeds 1 GiB mapped-byte budget");
        const auto file=s.path.parent_path()/name;s.stages[i]=std::make_unique<LockedFile>(file,bytes);need(s.stages[i]->size==bytes&&sha256(file)==row.at("sha256").get<std::string>(),"Raw capture SHA-256/size mismatch");s.pitch[i]=pitch;
    }
}
RawCapture::~RawCapture()=default;
const json& RawCapture::manifest()const{return p_->metadata;}
const std::filesystem::path& RawCapture::path()const{return p_->path;}
const std::string& RawCapture::manifest_hash()const{return p_->hash;}
unsigned RawCapture::width()const{return p_->width;}unsigned RawCapture::height()const{return p_->height;}
unsigned RawCapture::stage_count()const{return p_->count;}
bool RawCapture::display_pair()const{return p_->display;}
unsigned RawCapture::format(unsigned stage)const{need(stage<stage_count(),"Stage index out of range");return p_->formats[stage];}
color::Contract RawCapture::color_contract(unsigned stage)const{return color::resolve(p_->metadata,stage);}
unsigned RawCapture::row_pitch(unsigned stage)const{need(stage<stage_count(),"Stage index out of range");return p_->pitch[stage];}
const void* RawCapture::data(unsigned stage)const{need(stage<stage_count(),"Stage index out of range");return p_->stages[stage]->view;}
std::array<std::uint16_t,4> RawCapture::bits(unsigned stage,unsigned x,unsigned y)const{need(stage<stage_count()&&x<width()&&y<height(),"Pixel outside capture");std::array<std::uint16_t,4> out;
    const auto f=format(stage);const auto* pixel=static_cast<const std::uint8_t*>(data(stage))+std::size_t(y)*row_pitch(stage)+x*(f==10?8:4);
    if(f==10)memcpy(out.data(),pixel,8);else{for(unsigned i=0;i<4;++i)out[i]=pixel[i];if(f==87)std::swap(out[0],out[2]);}return out;}
std::array<float,4> RawCapture::pixel(unsigned stage,unsigned x,unsigned y)const{const auto raw=bits(stage,x,y);std::array<float,4> out;for(unsigned i=0;i<4;++i)out[i]=format(stage)==10?preconvert::unhalf(raw[i]):raw[i]/255.f;return out;}
std::uint64_t RawCapture::bytes()const{return p_->bytes;}
}
