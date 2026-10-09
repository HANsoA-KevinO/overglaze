// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <fstream>

namespace lab {
// The user's decision to finish or skip setup, and whether they allow an
// unrecognized (non-original) model -- off by default. Model readiness is
// checked afresh on every normal application start.
struct ModelSetupState {
    enum class Decision {pending,skipped,completed};
    Decision decision=Decision::pending;
    // Version 2 only: "allow unrecognized models". A file without it (version
    // 1, every earlier preference) means off, and off is written as version 1.
    bool allow_unrecognized=false;
    bool should_prompt(bool allowed,bool checked,bool known)const noexcept {
        return allowed&&checked&&!known&&decision==Decision::pending;
    }
    json document()const{json d={{"kind","overglaze-model-setup"},{"version",allow_unrecognized?2:1},
        {"decision",decision==Decision::completed?"completed":decision==Decision::skipped?"skipped":"pending"}};
        if(allow_unrecognized)d["allow_unrecognized_model"]=true;return d;}
    static ModelSetupState parse(const json& j){
        const bool v2=j.is_object()&&j.value("version",0)==2;
        if(!j.is_object()||j.size()!=(v2?4u:3u)||j.value("kind","")!="overglaze-model-setup"||(j.at("version")!=1&&!v2))
            throw std::runtime_error("Unknown model setup preferences");
        ModelSetupState s;const auto d=j.at("decision").get<std::string>();
        if(d=="completed")s.decision=Decision::completed;
        else if(d=="skipped")s.decision=Decision::skipped;
        else if(d!="pending")throw std::runtime_error("Unknown model setup decision");
        if(v2){if(!j.at("allow_unrecognized_model").is_boolean())throw std::runtime_error("Unknown model setup preference");
            s.allow_unrecognized=j.at("allow_unrecognized_model").get<bool>();}
        return s;
    }
};
class ModelSetupPreferences {
    std::filesystem::path root_,file_;bool enabled_=false;
    static void plain(std::filesystem::path p){for(;!p.empty();){
        const auto a=GetFileAttributesW(p.c_str());
        if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Model setup path missing or contains a reparse point");
        const auto parent=p.parent_path();if(parent==p)break;p=parent;}}
public:
    ModelSetupState state;std::string error;
    explicit ModelSetupPreferences(std::filesystem::path data={}):root_(std::move(data)),enabled_(!root_.empty()){
        if(!enabled_)return;
        try{if(!root_.is_absolute()||root_!=root_.lexically_normal()||!root_.has_relative_path())throw std::runtime_error("Invalid model setup data root");
            plain(root_);file_=root_/L"settings"/L"model-setup-v1.json";
            if(std::filesystem::exists(file_)){plain(file_);if(std::filesystem::file_size(file_)>4096)throw std::runtime_error("Model setup preferences too large");
                std::ifstream input(file_,std::ios::binary);state=ModelSetupState::parse(json::parse(input));}
        }catch(const std::exception& e){error=e.what();enabled_=false;}
    }
    bool enabled()const noexcept{return enabled_;}
    void remember(ModelSetupState::Decision decision){state.decision=decision;save();}
    // The "allow unrecognized models" setting, saved at once.
    void allow_unrecognized(bool allow){state.allow_unrecognized=allow;save();}
private:
    void save(){
        if(!enabled_)return;
        std::filesystem::path temp;
        try{plain(root_);if(!std::filesystem::exists(file_.parent_path()))std::filesystem::create_directory(file_.parent_path());plain(file_.parent_path());
            if(std::filesystem::exists(file_))plain(file_);
            const auto lock_path=file_.wstring()+L".lock";
            if(std::filesystem::exists(lock_path))plain(lock_path);
            Handle lock(CreateFileW(lock_path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(!lock.valid())throw std::runtime_error("Another process owns model setup preferences");
            temp=file_.wstring()+L".tmp-"+wide(uuid());const auto bytes=state.document().dump(2);
            Handle output(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
            if(!output.valid())throw std::runtime_error("Cannot save model setup preferences");
            DWORD count=0;const bool written=WriteFile(output.value,bytes.data(),DWORD(bytes.size()),&count,nullptr)&&count==bytes.size()&&FlushFileBuffers(output.value);
            CloseHandle(output.value);output.value=INVALID_HANDLE_VALUE;
            if(!written||!MoveFileExW(temp.c_str(),file_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot update model setup preferences");
            temp.clear();error.clear();
        }catch(const std::exception& e){if(!temp.empty())DeleteFileW(temp.c_str());error=e.what();}
    }
};
}
