// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_game_profile.hpp"
namespace lab::profiles {
// Package-time corrections, behind exact executable AND module identities.
// No game-name inference in the host; no claim of in-game NR acceptance.
// Onimusha: the first NR insertion recorded once, never retired, and presents
// stopped with preservation disabled and no observed host-rebind contract.
// Preservation is the next bounded validation candidate, not a proven crash
// diagnosis. Keep all viewport/depth/colour facts and identity checks intact.
inline bool apply_package_binding_workaround(Facts& f){
    if(f.binding_preservation||f.native_evaluate_host_rebind||f.route!="sl-rr"||
       f.executable!="OnimushaWotS.exe"||
       f.executable_sha256!="69ffeadc34cf3c828d5713fb9e869aa6d187231a117c385ba6d434a711c99b26")return false;
    const std::pair<const char*,const char*> pins[]{
        {"sl.common.dll","3a479ce1652337245f427246f6b2e1436fb0b83a003a50bddda82e25e3764aff"},
        {"sl.dlss_d.dll","ec7c01cf0529ef80ff17f64f51f117dfc4eb8fa9a1b01a285c948f924e524940"},
        {"sl.interposer.dll","2c972ea8aa2edb9ea7a6cce638cf9937f3373cd6ee240187037bb79a6f514a85"}};
    for(const auto& [name,hash]:pins){const auto i=f.modules.find(name);if(i==f.modules.end()||i->second!=hash)return false;}
    f.binding_preservation=true;return true;
}
}
