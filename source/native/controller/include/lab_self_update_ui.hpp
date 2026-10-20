// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The desktop program's update notice and its 设置 / 更新 section
// (lab_self_update.hpp does the work). Out of game only; every network step
// and file write runs on a worker thread, never on the UI thread.
#include "lab_self_update.hpp"
#include <future>
#include <memory>

namespace lab {
class UpdatePanel {
public:
    // enabled=false (the smoke and UI regression modes): no settings file, no
    // network, nothing drawn but the version line.
    explicit UpdatePanel(bool enabled);
    ~UpdatePanel();
    UpdatePanel(const UpdatePanel&)=delete;UpdatePanel& operator=(const UpdatePanel&)=delete;
    void poll();
    // Under the navigation bar: the one-time "已更新" notice, an offer, the download.
    void banner(HWND window,float dpi);
    // The 更新 section of the Settings popup.
    void settings(HWND window,float dpi);
    // The Setup was started: the program exits so the Setup can replace it.
    bool exit_requested()const{return exit_;}
    // The notice's 查看游戏库 button; true once per click.
    bool take_open_games(){const bool v=open_games_;open_games_=false;return v;}
    const std::string& version()const{return running_text_;}
private:
    enum class Mode {automatic,manual};
    struct Job {update::Cancel cancel{false};update::Progress progress;std::atomic<int> stage{0};};
    bool enabled_=false,exit_=false,open_games_=false,dismissed_=false;
    update::Installation install_;std::optional<update::Version> running_;std::string running_text_,version_error_;
    std::shared_ptr<update::SettingsStore> store_;
    std::shared_ptr<update::Cancel> check_cancel_=std::make_shared<update::Cancel>(false);
    std::future<update::Offer> checking_;Mode check_mode_=Mode::automatic;std::string check_message_;
    std::optional<update::Offer> offer_;
    std::shared_ptr<Job> job_;std::future<std::string> installing_;std::string install_message_;
    // After a relaunch by the Setup.
    update::AfterUpdate after_=update::AfterUpdate::none;std::string after_from_,after_to_;bool notice_=false;
    std::future<int> plugin_updates_job_;int plugin_updates_=-1;
    std::future<void> cleanup_;
    void start_check(Mode);
    void start_install();
    void open_page(HWND window,const std::string& url);
};
}
