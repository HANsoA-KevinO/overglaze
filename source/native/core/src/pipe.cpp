// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_pipe.hpp"
#include <sddl.h>
#include <array>
#include <vector>
#include <algorithm>

namespace lab {
namespace {
constexpr DWORD kMaxMessage = 65536;
struct Security {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    Security() {
        Handle token;
        check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value), "Process token");
        DWORD size = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> buffer(size);
        check(GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size), "Token user");
        LPWSTR sid = nullptr;
        check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid), "SID string");
        std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr), "Pipe DACL");
        attributes = {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    }
    ~Security() { if (descriptor) LocalFree(descriptor); }
};
bool complete(HANDLE file, OVERLAPPED& ov, HANDLE stop, DWORD& count, DWORD timeout = 2000) {
    HANDLE waits[] = {ov.hEvent, stop};
    auto result = WaitForMultipleObjects(stop ? 2 : 1, waits, FALSE, timeout);
    if (result != WAIT_OBJECT_0) {
        CancelIoEx(file, &ov);
        // Cancellation must finish before the OVERLAPPED/buffer leave scope.
        GetOverlappedResult(file, &ov, &count, TRUE);
        return false;
    }
    return GetOverlappedResult(file, &ov, &count, FALSE) != FALSE;
}
bool transfer(HANDLE file, bool write, void* buffer, DWORD size, DWORD& count, HANDLE stop = nullptr) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    check(event.valid(), "I/O event");
    OVERLAPPED ov{}; ov.hEvent = event.value;
    BOOL ok = write ? WriteFile(file, buffer, size, &count, &ov) : ReadFile(file, buffer, size, &count, &ov);
    if (ok) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    return complete(file, ov, stop, count);
}
}
PipeServer::PipeServer(Controller& controller) : controller_(controller) {}
PipeServer::~PipeServer() { stop(); }
void PipeServer::start() {
    if (thread_.joinable()) throw std::runtime_error("Pipe server already started");
    thread_ = std::jthread([this](std::stop_token t) { serve(t); });
}
void PipeServer::stop() { if (thread_.joinable()) { thread_.request_stop(); thread_.join(); } }
void PipeServer::serve(std::stop_token token) {
    try {
        Security security;
        Handle stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        check(stop_event.valid(), "Stop event");
        std::stop_callback wake(token, [&] { SetEvent(stop_event.value); });
        // Keep the first-instance handle alive across clients to prevent name squatting.
        Handle pipe(CreateNamedPipeW(pipe_name(GetCurrentProcessId()).c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, kMaxMessage, kMaxMessage, 0, &security.attributes));
        check(pipe.valid(), "Create local user-only pipe");
        ready_ = true;
        while (!token.stop_requested()) {
            Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            check(connected.valid(), "Connect event");
            OVERLAPPED ov{}; ov.hEvent = connected.value;
            BOOL ok = ConnectNamedPipe(pipe.value, &ov);
            DWORD error = ok ? ERROR_SUCCESS : GetLastError(), unused = 0;
            if (error == ERROR_IO_PENDING) {
                HANDLE waits[] = {ov.hEvent, stop_event.value};
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
                    CancelIoEx(pipe.value, &ov); GetOverlappedResult(pipe.value, &ov, &unused, TRUE); break;
                }
                ok = GetOverlappedResult(pipe.value, &ov, &unused, FALSE);
            } else ok = error == ERROR_PIPE_CONNECTED || error == ERROR_SUCCESS;
            if (ok) {
                std::array<char, kMaxMessage> buffer{}; DWORD read = 0;
                if (transfer(pipe.value, false, buffer.data(), kMaxMessage, read, stop_event.value) && read) {
                    auto request = json::parse(buffer.data(), buffer.data() + read, nullptr, false);
                    auto response = controller_.handle(request, GetTickCount64()).dump();
                    if (response.size() <= kMaxMessage) {
                        DWORD written = 0;
                        if (transfer(pipe.value, true, response.data(), static_cast<DWORD>(response.size()), written, stop_event.value)
                            && written == response.size()) {
                            // DisconnectNamedPipe discards unread response bytes.
                            // Wait for client close using cancellable, bounded I/O;
                            // FlushFileBuffers would allow an unbounded wait here.
                            // This connection accepts exactly one request.
                            char unused_byte{}; DWORD ignored=0;
                            transfer(pipe.value, false, &unused_byte, 1, ignored, stop_event.value);
                        }
                    }
                }
            }
            DisconnectNamedPipe(pipe.value);
        }
    } catch (const std::exception& e) { controller_.diagnostic(e.what()); }
    ready_ = false;
}
std::vector<DWORD> discover_lab_processes() {
    WIN32_FIND_DATAW data{};
    HANDLE search=FindFirstFileW(L"\\\\.\\pipe\\*",&data);
    if(search==INVALID_HANDLE_VALUE) {
        if(GetLastError()==ERROR_FILE_NOT_FOUND) return {};
        throw std::runtime_error("Local pipe discovery failed");
    }
    struct Guard { HANDLE value; ~Guard() { FindClose(value); } } guard{search};
    std::vector<DWORD> result;
    do {
        std::wstring name=data.cFileName;
        const std::wstring prefix=L"Overglaze.";
        if(!name.starts_with(prefix)) continue;
        auto digits=name.substr(prefix.size());
        if(digits.empty() || digits.size()>10 || !std::all_of(digits.begin(),digits.end(),[](wchar_t c) { return c>=L'0'&&c<=L'9'; })) continue;
        auto pid=wcstoull(digits.c_str(),nullptr,10);
        if(pid && pid<=INT_MAX) result.push_back(static_cast<DWORD>(pid));
    } while(FindNextFileW(search,&data));
    if(GetLastError()!=ERROR_NO_MORE_FILES) throw std::runtime_error("Incomplete local pipe discovery");
    std::sort(result.begin(),result.end());
    result.erase(std::unique(result.begin(),result.end()),result.end());
    return result;
}
json pipe_request(DWORD pid, const json& request) {
    const auto path = pipe_name(pid);
    Handle pipe;
    const auto deadline = GetTickCount64() + 2000;
    // WaitNamedPipe returns immediately if no instance exists yet. Also retry
    // the race between an available instance and another client's CreateFile.
    while (!pipe.valid()) {
        pipe.value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe.valid()) break;
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY)
            throw std::runtime_error("Open lab pipe: Win32=" + std::to_string(error));
        if (GetTickCount64() >= deadline) throw std::runtime_error("Lab pipe unavailable (2 second connection timeout)");
        if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(path.c_str(), 20);
        else Sleep(10);
    }
    ULONG actual_pid = 0;
    check(GetNamedPipeServerProcessId(pipe.value, &actual_pid) && actual_pid == pid, "Unexpected pipe server process");
    DWORD mode = PIPE_READMODE_MESSAGE;
    check(SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr), "Pipe message mode");
    auto payload = request.dump();
    if (payload.empty() || payload.size() > kMaxMessage) throw std::runtime_error("Request exceeds 64 KiB");
    DWORD size = 0;
    check(transfer(pipe.value, true, payload.data(), static_cast<DWORD>(payload.size()), size) && size == payload.size(), "Pipe write/timeout");
    std::array<char, kMaxMessage> response{};
    check(transfer(pipe.value, false, response.data(), kMaxMessage, size), "Pipe read/timeout");
    return json::parse(response.data(), response.data() + size);
}
std::vector<DWORD> discover_automatic_games(){
    auto candidates=discover_lab_processes();std::vector<DWORD> result;
    if(candidates.size()>8)candidates.resize(8);
    for(const auto pid:candidates)try{
        auto reply=pipe_request(pid,{{"protocol","1.0"},{"request_id",uuid()},{"client_id","console-auto-discovery"},{"method","Hello"}});
        if(!reply.value("ok",false))continue;const auto& s=reply.at("status");const auto host=s.value("host",json());
        if(s.value("pid",0u)==pid&&host.is_object()&&host.value("origin","")=="game"&&
           host.value("backend","")=="standalone-d3d12"&&host.value("automatic_lifecycle",false))result.push_back(pid);
    }catch(...){/* A vanished/busy process is not a new control target. */}
    return result;
}
}
