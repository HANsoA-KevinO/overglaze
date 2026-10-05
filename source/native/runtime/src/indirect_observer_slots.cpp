// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#define CINTERFACE
#include <d3d12.h>
#include <cstddef>
namespace lab::indirect_observer {
std::size_t create_signature_slot(){return offsetof(ID3D12DeviceVtbl,CreateCommandSignature)/sizeof(void*);}
}
