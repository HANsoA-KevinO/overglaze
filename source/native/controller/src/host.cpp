// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_pipe.hpp"
#include <iostream>
#include <chrono>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || std::wstring(argv[1]) != L"--synthetic") {
        std::cerr << "Usage: overglaze_host --synthetic [--seconds N]\nNo NR DLL or game is loaded.\n";
        return 2;
    }
    try {
        unsigned long seconds = 300;
        if (argc == 4 && std::wstring(argv[2]) == L"--seconds") seconds = std::stoul(argv[3]);
        else if (argc != 2) throw std::runtime_error("Unexpected arguments");
        if (!seconds || seconds > 3600) throw std::runtime_error("Seconds must be 1..3600");
        lab::Controller controller(true);
        lab::PipeServer server(controller); server.start();
        std::cout << "SYNTHETIC control host; NOT DLSS. PID=" << GetCurrentProcessId() << std::endl;
        auto deadline = GetTickCount64() + seconds * 1000ULL;
        std::uint64_t frame = 0;
        while (GetTickCount64() < deadline) {
            controller.frame_boundary(frame++, GetTickCount64());
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        server.stop();
        std::cout << controller.status().dump(2) << '\n';
        return controller.status()["last_error"].get<std::string>().empty() ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
