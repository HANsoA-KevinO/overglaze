// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_game_manager.hpp"
#include <future>
#include <memory>
#include <mutex>
#include <optional>
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
    friend struct GameManagerPageTestAccess;
    // How a late-loading game just installed is started: the Steam launch
    // option (via "steam") or the watch command (via "watch"), as built by the
    // manager from this program's own root.
    struct Launch {std::string title,via,command;};
    struct Result {std::vector<games::Status> rows;games::Discovery found;bool discovering=false;std::string message,selection;
        games::StorageUsage storage;bool has_storage=false;std::optional<Launch> launch;};
    // How the worker threads open the manager. Production: reviewed models
    // only and the package's own installation checker; the UI test swaps in a
    // synthetic model hash, no checker and unsigned synthetic modules.
    struct Opening {std::string model_sha256;bool run_checker=true,allow_unsigned_modules=false;};
    Opening opening_;
    Launch launch_;bool show_launch_=false;
    // The status the confirmation was opened for: what the dialog says about
    // risks and the launch option comes from it, not from a later selection.
    games::Status pending_status_;
    // The running operation's latest stage, written by the worker's progress
    // callback and read by draw().
    struct Live {std::mutex lock;std::string text;unsigned index=0,count=0;};
    std::filesystem::path root_;std::future<Result> job_;
    std::shared_ptr<Live> live_=std::make_shared<Live>();
    std::vector<games::Status> rows_;games::Discovery found_;
    games::StorageUsage storage_; // read-only usage count, shown under 安装细节
    std::string selected_,error_,notice_;char path_[4096]{},search_[256]{};
    bool show_add_=false,approved_=false;int candidate_=0,filter_=0;
    // Keep the confirmation target stable even if the library selection changes.
    std::string pending_,pending_title_,pending_path_,operation_;
    void mark(const char*);
    void refresh(bool import_known=false);
    // late_package: make-package for a game whose EXE carries Denuvo (late loading).
    void act(const std::string& action,const std::string& id,bool late_package=false);
    void poll();
};
}
