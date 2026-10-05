// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <unknwn.h>
#include <cstdint>
namespace lab::standalone {
void factory_created(IUnknown*) noexcept;
// The late entry: an injected loader calls this from its own worker thread once
// the process is running. It both starts the host and begins discovery, because
// there is no game call to ride in on. Installed games only; never from DllMain.
// Flag 1: open the panel as soon as it attaches (the root proxy's on-Insert
// start, where the player's Insert press is what brought the host in).
constexpr unsigned late_open_panel=1u;
void start_late_attach(unsigned flags=0) noexcept;
// Health of the system-dxgi.dll resolution behind the proxy exports. The proxy
// never fail-fasts the game: a failed resolution is retried on the next call
// and answered with safe defaults meanwhile. last_stage: 1 system directory,
// 2 load failed, 3 load returned this proxy module, 4 export missing.
struct ForwarderStatus {
    unsigned attempts,failures,fallback_calls,last_stage;unsigned long last_error;
    bool resolved,compat_string_pending,compat_string_replayed;
};
ForwarderStatus forwarder_status() noexcept;
// Test-only attachment requires the exact launcher-verified fixture executable
// and addresses belonging to that executable. No image/ABI overrides in games.
struct FixtureBindings {
    unsigned size=sizeof(FixtureBindings),version=2;
    void* public_api[4]{}; // token, evaluate, constants, tag
    void* options=nullptr;void* inner[2]{};const std::uint64_t* flags=nullptr;
    unsigned frame_tagging=0;
};
using AttachFixture=bool(__cdecl*)(const FixtureBindings*);
using Stop=void(__cdecl*)();
}
