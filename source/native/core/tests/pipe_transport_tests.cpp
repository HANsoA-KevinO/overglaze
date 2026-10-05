// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_pipe.hpp"
#include <array>
#include <chrono>
#include <iostream>

#define REQUIRE(expr) do { if (!(expr)) throw std::runtime_error("FAILED: " #expr); } while (false)

int main() {
    try {
        lab::Controller controller;
        lab::PipeServer server(controller); server.start();
        const auto deadline=GetTickCount64()+2000;
        while(!server.ready() && GetTickCount64()<deadline) Sleep(1);
        REQUIRE(server.ready());
        auto request=lab::json{{"protocol","1.0"},{"request_id","delayed-reader"},
            {"client_id","transport-test"},{"method","GetStatus"}};
        const auto path=lab::pipe_name(GetCurrentProcessId());
        auto connect=[&]() -> HANDLE {
            const auto until=GetTickCount64()+2000;
            for (;;) {
                auto pipe=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
                if(pipe!=INVALID_HANDLE_VALUE) return pipe;
                REQUIRE(GetLastError()==ERROR_PIPE_BUSY);
                REQUIRE(GetTickCount64()<until);
                WaitNamedPipeW(path.c_str(),20);
            }
        };
        auto write=[&](HANDLE pipe) {
            DWORD mode=PIPE_READMODE_MESSAGE; REQUIRE(SetNamedPipeHandleState(pipe,&mode,nullptr,nullptr));
            auto payload=request.dump(); DWORD count=0;
            REQUIRE(WriteFile(pipe,payload.data(),static_cast<DWORD>(payload.size()),&count,nullptr));
            REQUIRE(count==payload.size());
        };
        auto read=[&](HANDLE pipe) {
            std::array<char,65536> bytes{}; DWORD count=0;
            REQUIRE(ReadFile(pipe,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr));
            auto response=lab::json::parse(bytes.data(),bytes.data()+count);
            REQUIRE(response["ok"]==true); REQUIRE(response["request_id"]==request["request_id"]);
        };
        {
            lab::Handle client(connect()); write(client.value);
            // A completed server write is not proof that the client consumed it.
            Sleep(150); read(client.value);
        }
        for(unsigned i=0;i<300;++i) REQUIRE(lab::pipe_request(GetCurrentProcessId(),request)["ok"]==true);
        { lab::Handle abandoned(connect()); write(abandoned.value); }
        REQUIRE(lab::pipe_request(GetCurrentProcessId(),request)["ok"]==true);
        controller.enable_nr_preparation(true);
        controller.publish_host({{"origin","synthetic"},{"backend","standalone-d3d12"},{"automatic_lifecycle",true}});
        auto contains_self=[](const auto& candidates){return std::find(candidates.begin(),candidates.end(),GetCurrentProcessId())!=candidates.end();};
        REQUIRE(!contains_self(lab::discover_automatic_games()));
        // A transport test double, not evidence that this process is a game.
        controller.publish_host({{"origin","game"},{"backend","standalone-d3d12"},{"automatic_lifecycle",true}});
        REQUIRE(contains_self(lab::discover_automatic_games()));
        REQUIRE(controller.status()["control_owner"]=="");
        controller.publish_host({{"origin","game"},{"backend","unknown"},{"automatic_lifecycle",true}});
        REQUIRE(!contains_self(lab::discover_automatic_games()));
        {
            // A client that retains its handle must not prevent server shutdown.
            lab::Handle idle(connect()); write(idle.value); read(idle.value);
            auto before=GetTickCount64(); server.stop();
            REQUIRE(GetTickCount64()-before<1000);
        }
        std::cout<<"PASS: delayed reader, 300 reconnects, abandoned response, cancellable idle client\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
