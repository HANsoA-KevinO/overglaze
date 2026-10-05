// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_game_manager.hpp"
#include <future>
#include <memory>
#include <mutex>
namespace lab {
class GameManagerPage {
public:
    explicit GameManagerPage(std::filesystem::path root,bool import_known=true);
    void draw(HWND window,float dpi);
    json controls=json::object();
    bool busy()const{return job_.valid();}
    std::size_t count()const{return rows_.size();}
    std::string error()const{return error_;}
private:
    struct Result {std::vector<games::Status> rows;games::Discovery found;bool discovering=false;std::string message;
        games::StorageUsage storage;bool has_storage=false;};
    // The running operation's latest stage, written by the worker's progress
    // callback and read by draw().
    struct Live {std::mutex lock;std::string text;unsigned index=0,count=0;};
    std::filesystem::path root_;std::future<Result> job_;
    std::shared_ptr<Live> live_=std::make_shared<Live>();
    std::vector<games::Status> rows_;games::Discovery found_;
    games::StorageUsage storage_; // read-only usage count, shown under 安装细节
    std::string selected_,error_,notice_;char path_[4096]{};
    bool show_add_=false,approved_=false;int candidate_=0;
    std::string pending_,operation_;
    void mark(const char*);
    void refresh(bool import_known=false);
    void act(const std::string& action,const std::string& id);
    void poll();
};
}
