// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <array>
#include <string>
#include <string_view>
#include <windows.h>
namespace lab {
namespace rr {class Watch;}
// Host discovery has two halves:
// the public one (the loaded interposer, the public RR options function, the
// public Evaluate boundary) which both tracks run, and the reviewed private one
// (the RR begin/end callbacks and the common module's restore hook) which only
// the research track has. The private half lives behind this nullable
// extension, so the controller's link closure contains none of it.
//
// With no extension the host reports admission_mode
// "public-evaluate-evidence-only" and admits on public evidence, exactly as it
// already does on a build with no reviewed profile. It then also finds the RR
// options setter only through the public slGetFeatureFunction path.

// What the research discovery may report back. None of it grants admission by
// itself: the adapter still applies its own proofs to every call.
struct WorkbenchResearchHost {
    virtual void set_host_flags_address(const void* flags) noexcept=0;
    virtual void set_native_evaluate_contract(bool) noexcept=0;
    // The installation's own fact, not an executable-name comparison.
    virtual bool game_native_evaluate_host_rebind()const noexcept=0;
protected:~WorkbenchResearchHost()=default;
};

// The host's RR options record, lent to an extension that looks for the options
// setter by other means than the public path. The extension fills it the way the
// public path would; the host still decides admission.
struct WorkbenchOptionsRecord {
    rr::Watch& watch;
    json& module;
    json& hooks;
    std::string& error;
    std::string& resolution;
    bool& attempted;
    bool& installed;
};

struct WorkbenchResearchExtension {
    virtual ~WorkbenchResearchExtension()=default;
    // Install the reviewed begin/end callbacks for this interposer image.
    // True: they are installed, and admission then REQUIRES that evidence.
    virtual bool attach_inner_callbacks(HMODULE interposer,const json& options_module)=0;
    // The synthetic fixture supplies its own begin/end entry points; the same
    // hooks, without module discovery. It also consumes the restore attempt.
    virtual bool install_fixture_inner_callbacks(const std::array<void*,2>& inner)=0;
    // Periodic discovery of the verified common module and its restore hook.
    // Only called once the inner callbacks are installed.
    virtual void poll_restore(WorkbenchResearchHost&)=0;
    virtual void stop() noexcept=0;
    // Fills the rr_inner and common_restore blocks of the host snapshot.
    virtual void describe(json& snapshot)=0;
    // Provenance only, never admission: whether a loaded module is one of the
    // builds the research track reviewed by hand. The host reports it as
    // known_reviewed_build; without an extension it reports nothing.
    virtual bool known_reviewed_interposer(std::string_view /*game_id*/,std::string_view /*sha256*/)const noexcept{return false;}
    virtual bool known_reviewed_options_module(std::string_view /*sha256*/)const noexcept{return false;}
    // Options discovery beyond the public path. check_options runs on every
    // options poll; poll_options only while the public setter is not resolvable
    // and nothing was attempted. poll_options returns true once it reached the
    // install step, with the module that holds the (pinned) setter.
    virtual void check_options(WorkbenchOptionsRecord&,ULONGLONG /*now*/){}
    virtual bool poll_options(WorkbenchOptionsRecord&,ULONGLONG /*now*/,HMODULE& /*module*/){return false;}
};
}
