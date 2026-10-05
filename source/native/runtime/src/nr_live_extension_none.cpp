// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Controller variant of the live bridge extension seam: there is no research
// extension, so the controller bridge links no frame capture, display pair
// capture, binding probe or boundary audit object at all. Every research entry
// point is simply absent from its export table, and a research host that loads
// this bridge reports research_available=false instead of failing.
#include "nr_live_extension.hpp"
#include <cstring>
namespace lab::live {
LiveExtension* create_live_extension(){return nullptr;}
void describe_live_variant(Capabilities& out)noexcept{
    out.variant=1;out.research_exports=0;
    out.supports_binding_preservation=1;out.supports_settings=1;out.supports_compute_only=1;
    std::strncpy(out.build,"controller-runtime-only",sizeof(out.build)-1);
}
}
