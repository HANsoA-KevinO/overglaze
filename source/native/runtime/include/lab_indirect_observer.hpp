// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <d3d12.h>
namespace lab::indirect_observer {
// Lightweight creation metadata only. Must start before command signatures are
// created; swapchain fallback cannot retroactively recover missed signatures.
// Shared runtime: the research host starts it at its first
// factory, the controller host on its early (root-proxy) path for a package with
// binding preservation (007 First Light); a late attach never has it, which is why
// its unknown-signature policy is tolerant there and strict everywhere else.
void start();
void device(ID3D12Device*);
json status();
void uninstall_quiesced_for_test();
std::size_t create_signature_slot();
}
