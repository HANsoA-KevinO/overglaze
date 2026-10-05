// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_pipe.hpp"
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 3 || argc > 4) {
            std::cerr << "Usage: overglazectl <pid> <GetStatus|GetCapabilities|request.json> [client-id]\n"
                         "JSON requests retain caller-supplied request_id, session_id and expected_revision.\n";
            return 2;
        }
        std::wstring pid_text(argv[1]); size_t parsed = 0;
        auto value = std::stoul(pid_text, &parsed);
        if (parsed != pid_text.size() || !value || value > MAXDWORD) throw std::runtime_error("Invalid PID");
        lab::json request;
        auto method = lab::utf8(argv[2]);
        if (method == "GetStatus" || method == "GetCapabilities" || method == "Hello") {
            request = {{"protocol", "1.0"}, {"method", method}, {"request_id", lab::uuid()},
                       {"client_id", argc == 4 ? lab::utf8(argv[3]) : "overglazectl-readonly"}};
        } else {
            std::ifstream file(std::filesystem::path(argv[2]), std::ios::binary);
            if (!file) throw std::runtime_error("Cannot open request JSON");
            file >> request;
        }
        auto response = lab::pipe_request(static_cast<DWORD>(value), request);
        std::cout << response.dump(2) << '\n';
        return response.value("ok", false) ? 0 : 3;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
