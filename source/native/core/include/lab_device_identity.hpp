// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <stdexcept>

namespace lab {
// Core has no dependency on a host's wrapping ABI. A host may supply a precise
// normalizer, never a boolean permission to skip the cross-device check.
struct DeviceIdentity {
    using Device=Microsoft::WRL::ComPtr<ID3D12Device>;
    using Normalize=Device (*)(ID3D12Device*);
    Normalize normalize=nullptr;
    bool same(ID3D12Device* a,ID3D12Device* b) const {
        if(!a||!b)return false;
        Device x=normalize?normalize(a):Device(a),y=normalize?normalize(b):Device(b);
        Microsoft::WRL::ComPtr<IUnknown> xi,yi;
        return x&&y&&SUCCEEDED(x.As(&xi))&&SUCCEEDED(y.As(&yi))&&xi==yi;
    }
};
}
