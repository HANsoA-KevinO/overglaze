// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_ui_preferences.hpp"
#include "lab_nr_settings.hpp"
#include <condition_variable>
#include <fstream>
#include <thread>
namespace lab {
namespace {
void need(bool v,const char* why){if(!v)throw std::runtime_error(why);}
float number(const json& j,const char* key,float lo,float hi){need(j.at(key).is_number(),"Preference must be numeric");const auto v=j.at(key).get<double>();need(std::isfinite(v)&&v>=lo&&v<=hi,"Preference outside allowed range");return float(v);}
void flag(const json& j,const char* key){need(j.at(key).is_boolean(),"Preference must be boolean");}
void plain(std::filesystem::path p){for(;!p.empty();){const auto a=GetFileAttributesW(p.c_str());need(a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_REPARSE_POINT),"Preference path missing or contains a reparse point");const auto parent=p.parent_path();if(parent==p)break;p=parent;}}
void validate(const json& j){const auto kind=j.at("kind").get<std::string>();if(kind=="overglaze-viewer-preferences")ViewerPreferences::parse(j);else if(kind=="overglaze-overlay-preferences")OverlayPreferences::parse(j);else throw std::runtime_error("Unknown preference kind");}
// Preferences written before the product rename carry the old kind. They
// are read once under the new kind and written back only in the new one; the
// fields and versions are unchanged, so nothing else is converted.
json current_kind(json j){
    if(j.is_object()&&j.contains("kind")&&j.at("kind").is_string()){const auto kind=j.at("kind").get<std::string>();
        if(kind=="dlsslab-viewer-preferences")j["kind"]="overglaze-viewer-preferences";
        else if(kind=="dlsslab-overlay-preferences")j["kind"]="overglaze-overlay-preferences";}
    return j;}
}
json ViewerPreferences::document() const{return {{"version",4},{"kind","overglaze-viewer-preferences"},{"exposure",display.exposure},{"white_nits",display.white_nits},{"peak_nits",display.peak_nits},{"curve",display.curve},{"difference_gain",display.difference_gain},{"nearest",nearest},{"library",library},{"hdr",hdr},{"view",view},{"wipe",wipe},{"mapping",display.mapping},{"contrast",display.contrast},{"processing",processing}};}
ViewerPreferences ViewerPreferences::parse(const json& j){need(j.is_object()&&((j.size()==12&&j.at("version")==1)||(j.size()==14&&(j.at("version")==2||j.at("version")==3))||(j.size()==15&&j.at("version")==4))&&j.at("kind")=="overglaze-viewer-preferences","Unknown viewer preference version/fields");ViewerPreferences p;
    p.display.exposure=number(j,"exposure",-6,6);p.display.white_nits=number(j,"white_nits",80,400);p.display.peak_nits=number(j,"peak_nits",400,4000);p.display.difference_gain=number(j,"difference_gain",1,64);
    need(j.at("curve").is_number_integer()&&j.at("view").is_number_integer(),"Preference enum must be an integer");p.display.curve=unsigned(number(j,"curve",0,2));p.view=unsigned(number(j,"view",0,2));p.wipe=number(j,"wipe",0,1);
    if(j.at("version")!=1){need(j.at("mapping").is_number_integer(),"Mapping must be an integer");p.display.mapping=unsigned(number(j,"mapping",0,j.at("version").get<int>()>=3?3.f:2.f));p.display.contrast=number(j,"contrast",.75,1.5);}
    else p.display.mapping=2; // Preserve the old rendering until explicitly switched.
    if(j.at("version")==4){flag(j,"processing");p.processing=j.at("processing");}
    for(const auto* key:{"nearest","library","hdr"})flag(j,key);p.nearest=j.at("nearest");p.library=j.at("library");p.hdr=j.at("hdr");return p;}
json OverlayPreferences::document() const{
    if(!has_model)return {{"version",1},{"kind","overglaze-overlay-preferences"},{"hotkey",hotkey},{"white_nits",white}};
    return {{"version",4},{"kind","overglaze-overlay-preferences"},{"hotkey",hotkey},{"white_nits",white},
        {"tone",tone},{"structure",structure},{"style",style},{"exposure_stops",exposure_stops},{"exposure_auto",exposure_auto},{"skin",skin},{"automask",automask},
        {"extrapolate",extrapolate},{"extrapolate_factor",extrapolate_factor}};}
OverlayPreferences OverlayPreferences::parse(const json& j){
    need(j.is_object()&&j.at("kind")=="overglaze-overlay-preferences"&&((j.size()==4&&j.at("version")==1)||(j.size()==9&&j.at("version")==2)||(j.size()==11&&j.at("version")==3)||(j.size()==13&&j.at("version")==4)),"Unknown overlay preference version/fields");OverlayPreferences p;
    need(j.at("hotkey").is_number_integer(),"Hotkey must be an integer");const auto k=j.at("hotkey").get<std::int64_t>();need(k==VK_INSERT||k==VK_F7||k==VK_F8||k==VK_F9,"Unsupported panel hotkey");p.hotkey=unsigned(k);p.white=number(j,"white_nits",80,300);
    if(j.at("version")!=1){p.has_model=true;p.tone=number(j,"tone",0,nr::Settings::max_tone_structure);p.structure=number(j,"structure",0,nr::Settings::max_tone_structure);p.exposure_stops=number(j,"exposure_stops",nr::Settings::min_exposure_stops,nr::Settings::max_exposure_stops);
        need(j.at("style").is_number_integer()&&j.at("style")>=0&&j.at("style")<=2,"Style must be 0..2");p.style=j.at("style").get<unsigned>();
        need(j.at("exposure_auto").is_number_integer()&&j.at("exposure_auto")>=0&&j.at("exposure_auto")<=1,"exposure_auto must be 0/1");p.exposure_auto=j.at("exposure_auto").get<unsigned>();
        if(j.at("version")==3||j.at("version")==4){p.skin=number(j,"skin",0,nr::Settings::max_skin);
            need(j.at("automask").is_number_integer()&&j.at("automask")>=0&&j.at("automask")<=1,"automask must be 0/1");p.automask=j.at("automask").get<unsigned>();}
        // Files before version 4 keep extrapolation off at the default factor.
        if(j.at("version")==4){need(j.at("extrapolate").is_number_integer()&&j.at("extrapolate")>=0&&j.at("extrapolate")<=1,"extrapolate must be 0/1");p.extrapolate=j.at("extrapolate").get<unsigned>();
            p.extrapolate_factor=number(j,"extrapolate_factor",nr::Settings::min_extrapolate_factor,nr::Settings::max_extrapolate_factor);}}
    return p;}
struct UiPreferencesFile::Impl {
    mutable std::mutex mutex;std::condition_variable changed;std::thread worker;std::filesystem::path file;json initial,last;std::optional<json> pending;
    std::string failure;bool closing=false,writable=true;std::unique_ptr<Handle> writer;
    Impl(std::filesystem::path path,json defaults,std::filesystem::path inherit,std::filesystem::path data_root):file(std::move(path)),initial(defaults),last(defaults){validate(defaults);if(file.empty()){writable=false;return;}
        try{// Host contract V4: the data root is the caller's -- the one the installation
            // recorded, or the viewer's resolved root -- never a compiled path.
            const auto root=data_root.empty()?file.parent_path().parent_path():data_root;
            need(root.is_absolute()&&root==root.lexically_normal()&&root.has_relative_path(),"Preference data root must be an absolute directory");
            need(file.is_absolute()&&file==file.lexically_normal()&&file.extension()==L".json"&&file.parent_path().parent_path()==root,"Preferences must be in a direct managed subdirectory");
            plain(root);if(!std::filesystem::exists(file.parent_path()))need(std::filesystem::create_directory(file.parent_path()),"Cannot create preference directory");plain(file.parent_path());
            const auto lock=file.wstring()+L".lock";if(std::filesystem::exists(lock))plain(lock);writer=std::make_unique<Handle>(CreateFileW(lock.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            need(writer->valid(),"Another process owns preference writes");
            if(std::filesystem::exists(file)){plain(file);need(std::filesystem::file_size(file)<=4096,"Preference file too large");std::ifstream stream(file,std::ios::binary);auto j=current_kind(json::parse(stream));validate(j);need(j.at("kind")==defaults.at("kind"),"Preference kind mismatch");initial=last=std::move(j);}
            else if(!inherit.empty()&&std::filesystem::exists(inherit)){
                need(inherit.is_absolute()&&inherit==inherit.lexically_normal()&&inherit.parent_path()==file.parent_path()&&inherit.extension()==L".json","Legacy preference outside same settings directory");
                plain(inherit);need(std::filesystem::file_size(inherit)<=4096,"Legacy preferences too large");std::ifstream stream(inherit,std::ios::binary);auto j=current_kind(json::parse(stream));validate(j);
                need(j.at("kind")=="overglaze-viewer-preferences"&&defaults.at("kind")==j.at("kind")&&(j.at("version")==1||j.at("version")==2||j.at("version")==3),"Only legacy viewer display preferences may be inherited");initial=std::move(j);
            }
            worker=std::thread([this]{run();});
        }catch(const std::exception& e){failure=e.what();writable=false;writer.reset();}}
    void write(const json& doc){plain(file.parent_path());if(std::filesystem::exists(file))plain(file);const auto bytes=doc.dump(2);need(bytes.size()<=4096,"Preference size limit");auto id=uuid();
        const auto temp=file.wstring()+L".tmp-"+wide(id);Handle output(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));need(output.valid(),"Cannot create preference update");
        DWORD count=0;const bool ok=WriteFile(output.value,bytes.data(),DWORD(bytes.size()),&count,nullptr)&&count==bytes.size()&&FlushFileBuffers(output.value);
        CloseHandle(output.value);output.value=INVALID_HANDLE_VALUE;
        if(!ok||!MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp.c_str());throw std::runtime_error("Preference atomic update failed");}}
    void run(){std::unique_lock lock(mutex);for(;;){changed.wait(lock,[&]{return closing||pending.has_value();});if(!pending&&closing)break;
        if(!closing)changed.wait_for(lock,std::chrono::milliseconds(250),[&]{return closing;});auto doc=std::move(*pending);pending.reset();lock.unlock();std::string error;
        try{write(doc);}catch(const std::exception& e){error=e.what();}lock.lock();failure=std::move(error);if(closing&&!pending)break;}}
};
UiPreferencesFile::UiPreferencesFile(std::filesystem::path p,json defaults,std::filesystem::path inherit,std::filesystem::path data_root):p_(std::make_unique<Impl>(std::move(p),std::move(defaults),std::move(inherit),std::move(data_root))){}
UiPreferencesFile::~UiPreferencesFile(){close();}
json UiPreferencesFile::loaded()const{std::lock_guard lock(p_->mutex);return p_->initial;}
void UiPreferencesFile::save(json j){validate(j);std::lock_guard lock(p_->mutex);if(!p_->writable||p_->closing||j==p_->last)return;need(j.at("kind")==p_->initial.at("kind"),"Preference kind mismatch");p_->last=j;p_->pending=std::move(j);p_->changed.notify_one();}
std::string UiPreferencesFile::error()const{std::lock_guard lock(p_->mutex);return p_->failure;}
void UiPreferencesFile::close(){{std::lock_guard lock(p_->mutex);p_->closing=true;p_->changed.notify_one();}if(p_->worker.joinable())p_->worker.join();p_->writer.reset();}
}
