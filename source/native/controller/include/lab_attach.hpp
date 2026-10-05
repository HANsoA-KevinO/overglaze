// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
namespace lab::games {
// What must already be true before anything is written into another process.
// Every field is checked against the adapter package, not against the caller's
// word: a wrong PID, a changed game build or a loader that is not the one the
// package pinned each stop the attach before injection.
struct AttachGates {
    std::filesystem::path expected_executable;   // from the registry entry
    std::string expected_executable_sha256;      // from the package's pins
    std::string expected_loader_sha256;          // from the package's payload
    DWORD wait_ms=10000;                         // bound on the remote LoadLibraryW
};
struct AttachResult {
    DWORD pid=0;
    std::filesystem::path executable;
    std::string loader_sha256,note;
    bool loaded=false;
};
// Injects the late-loading controller by CreateRemoteThread(LoadLibraryW) with an
// absolute path. Throws with a reason rather than returning a partial result.
// Loading is not starting: the injected DLL refuses on its own terms unless a
// validated installation sits beside it, and only a control-pipe handshake shows
// that the host actually came up.
AttachResult attach(DWORD pid,const std::filesystem::path& loader,const AttachGates&);
// PID of a running process whose full image path is exactly this executable,
// or 0. Matched on the whole path, never on the file name alone. If the game
// is running twice the first match is returned: attach() re-verifies the
// image path and hash of whichever PID it is handed, so the pick is still a
// correct instance of that game, just not necessarily the newer one.
DWORD find_process(const std::filesystem::path& executable);
}
