// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstdint>

namespace lab::nr {
enum class DepthEncoding : unsigned { hardware=0, view_z_magnitude=1 };
// Perspective projection of view-space Z, not radial ray distance, normalized
// depth, or meters inferred from a texture format. Row-vector coefficients:
// clipZ = z*A+B, clipW = z*C+D. For a front-facing point abs(viewZ) is
// converted to the handedness of C. This accepts signed Z or positive Z distance
// without guessing a unit scale. The caller must establish view-space units.
struct DepthProjection {
    float a=0,b=0,c=0,d=0;
    float near_plane=0,far_plane=0;
    unsigned inverted=0,reserved=0;
    void validate() const {
        for(float v:{a,b,c,d,near_plane,far_plane})if(!std::isfinite(v))throw std::logic_error("Nonfinite depth projection");
        if((c!=1.f&&c!=-1.f)||d!=0.f||near_plane<=0||far_plane<=near_plane||inverted>1||reserved)
            throw std::logic_error("Only explicit finite perspective view-Z depth is supported");
        const auto ndc=[&](double distance){const double z=distance*c;return (z*a+b)/(z*c);};
        if(std::abs(ndc(near_plane)-(inverted?1.:0.))>2e-5 || std::abs(ndc(far_plane)-(inverted?0.:1.))>2e-5)
            throw std::logic_error("Projection, clipping planes and depth direction disagree");
    }
    // Explicit sentinel policy for the NR-only copy: zero/NaN/Inf => far plane;
    // finite view-Z outside clipping planes is clamped. Never edits game depth.
    float project(float view_z) const {
        if(!std::isfinite(view_z)||view_z==0)return inverted?0.f:1.f;
        const float z=std::clamp(std::abs(view_z),near_plane,far_plane)*c;
        return std::clamp((z*a+b)/(z*c+d),0.f,1.f);
    }
};
static_assert(sizeof(DepthProjection)==32);
inline bool guide_extent_supported(unsigned w,unsigned h,unsigned gw,unsigned gh) noexcept {
    return w&&h&&w<=8192&&h<=8192&&gw&&gh&&gw<=w&&gh<=h;
}
}
