// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_control.hpp"
#include <thread>
#include <atomic>

namespace lab {
class PipeServer {
public:
    explicit PipeServer(Controller& controller);
    ~PipeServer();
    void start();
    void stop();
    bool ready() const { return ready_.load(); }
private:
    void serve(std::stop_token token);
    Controller& controller_;
    std::jthread thread_;
    std::atomic<bool> ready_ = false;
};
json pipe_request(DWORD pid, const json& request);
// Local discovery only: does not connect, acquire a lease or start a process.
std::vector<DWORD> discover_lab_processes();
// Read-only Hello, bounded to eight local pipes. Never acquires a control lease.
std::vector<DWORD> discover_automatic_games();
}
