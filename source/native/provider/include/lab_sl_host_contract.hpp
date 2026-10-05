// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_sl_boundary.hpp"
#include <cstdint>
namespace lab::slhost {
// The public Streamline PreferenceFlags the game gave its own SL instance, read
// from an already identity-verified, pinned common image. Kept apart from
// lab_sl_restore.hpp: both tracks need to know whether
// the host declared that it rebinds its own pipeline state, but only the
// research track hooks the private restore callback.
//
// This is a public flags word, never an inference from a missing callback.
struct HostContract {
    std::uint64_t flags=0;bool readable=false;
    bool host_rebind_required() const noexcept {
        constexpr auto mask=static_cast<std::uint64_t>(sl::PreferenceFlags::eDisableCLStateTracking)|
            static_cast<std::uint64_t>(sl::PreferenceFlags::eUseManualHooking);
        return readable&&(flags&mask)!=0;
    }
    bool operator==(const HostContract&) const=default;
};
// Guarded read: an unreadable address yields readable=false, never a fault.
HostContract read_host_contract(const void* flags) noexcept;
}
