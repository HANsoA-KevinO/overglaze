// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_sl_host_contract.hpp"
#include <windows.h>
namespace lab::slhost {
HostContract read_host_contract(const void* flags) noexcept {
    HostContract out;if(!flags)return out;
    __try {out.flags=*static_cast<const volatile std::uint64_t*>(flags);out.readable=true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {out={};}
    return out;
}
}
