// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <sl_consts.h>
#include <cmath>
#include <stdexcept>
#include "lab_depth_projection.hpp"

namespace lab::nr {
struct SlGuideConstants {float scale_x=0,scale_y=0;bool depth_inverted=false,reset=false;};
inline DepthProjection sl_view_depth_projection(const sl::Constants& c) {
    const auto& m=c.cameraViewToClip;
    for(unsigned i=0;i<4;++i)for(float v:{m[i].x,m[i].y,m[i].z,m[i].w})
        if(!std::isfinite(v)||v==sl::INVALID_FLOAT)throw std::logic_error("Missing view-Z projection");
    if(c.orthographicProjection!=sl::eFalse||(c.depthInverted!=sl::eTrue&&c.depthInverted!=sl::eFalse)||
       m[0].z!=0||m[1].z!=0||m[0].w!=0||m[1].w!=0)
        throw std::logic_error("Oblique/orthographic/unknown view-Z projection unsupported");
    DepthProjection p{m[2].z,m[3].z,m[2].w,m[3].w,c.cameraNear,c.cameraFar,c.depthInverted==sl::eTrue?1u:0u,0};
    p.validate();return p;
}

// Shared by both the real-game host and the independent adapter. This checks
// the admitted public contract; it does not certify the texture's pixel meaning.
inline SlGuideConstants sl_guide_constants(const sl::Constants& c,unsigned guide_width,unsigned guide_height) {
    const auto valid=[](float v){return std::isfinite(v) && v!=sl::INVALID_FLOAT;};
    const auto boolean=[](sl::Boolean v){return v==sl::eFalse || v==sl::eTrue;};
    if(!guide_width || !guide_height || guide_width>8192 || guide_height>8192 || !valid(c.mvecScale.x) || !valid(c.mvecScale.y) ||
       !valid(c.jitterOffset.x) || !valid(c.jitterOffset.y) || !boolean(c.depthInverted) || !boolean(c.reset) ||
       c.cameraMotionIncluded!=sl::eTrue || c.motionVectors3D!=sl::eFalse || c.motionVectorsJittered!=sl::eFalse ||
       c.orthographicProjection!=sl::eFalse || !boolean(c.motionVectorsDilated))
        throw std::logic_error("Unsupported or missing public motion/depth constants");
    // Fixed official bridge: normalized SL scale times actual MV extent.
    // Preserve signs and zero; use neither display extent nor a guessed scale.
    // Jitter is validated, not subtracted again from already unjittered MVs.
    const float x=c.mvecScale.x*guide_width,y=c.mvecScale.y*guide_height;
    if(!std::isfinite(x)||!std::isfinite(y))throw std::logic_error("Motion scale overflow");
    return {x,y,c.depthInverted==sl::eTrue,c.reset==sl::eTrue};
}
}
