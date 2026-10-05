// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#define NGX_SNIPPET_BUILD
#include "lab_sl_guide_contract.hpp"
#include <iostream>
#include <limits>
#include <cstddef>
int main(){try {
    unsigned checked=0;auto need=[&](bool v){++checked;if(!v)throw std::runtime_error("SL guide contract failure");};
    sl::Constants c;c.mvecScale={-2.f/640,.5f/360};c.jitterOffset={.25f,-.125f};
    c.depthInverted=sl::eTrue;c.cameraMotionIncluded=sl::eTrue;c.motionVectors3D=sl::eFalse;c.reset=sl::eFalse;
    auto a=lab::nr::sl_guide_constants(c,640,360);need(a.scale_x==-2&&a.scale_y==.5f&&a.depth_inverted&&!a.reset);
    c.reset=sl::eTrue;a=lab::nr::sl_guide_constants(c,640,360);need(a.reset);c.reset=sl::eFalse;
    for(unsigned test=0;test<13;++test){auto bad=c;unsigned w=640,h=360;
        if(test==0)bad.mvecScale.x=sl::INVALID_FLOAT;if(test==1)bad.mvecScale.y=std::numeric_limits<float>::infinity();
        if(test==2)bad.jitterOffset.x=std::numeric_limits<float>::quiet_NaN();if(test==3)bad.depthInverted=sl::eInvalid;
        if(test==4)bad.reset=sl::eInvalid;if(test==5)bad.cameraMotionIncluded=sl::eFalse;if(test==6)bad.motionVectors3D=sl::eTrue;
        if(test==7)bad.motionVectorsJittered=sl::eTrue;if(test==8)bad.orthographicProjection=sl::eTrue;
        if(test==9)bad.motionVectorsDilated=sl::eInvalid;if(test==10)w=0;if(test==11)h=8193;
        if(test==12)bad.mvecScale.x=std::numeric_limits<float>::max()/2;
        bool refused=false;try{lab::nr::sl_guide_constants(bad,w,h);}catch(const std::logic_error&){refused=true;}need(refused);
    }
    c.mvecScale={0,0};a=lab::nr::sl_guide_constants(c,640,360);need(a.scale_x==0&&a.scale_y==0);
    // Fixed public header offsets used by the disassembled official bridge.
    // Offset agreement checks the mapping, not the game's pixel semantics.
    need(offsetof(sl::Constants,jitterOffset)==0x160);
    need(offsetof(sl::Constants,mvecScale)==0x168);
    need(offsetof(sl::Constants,depthInverted)==0x1bc);
    need(offsetof(sl::Constants,reset)==0x1bf);
    // The observed game's normalized scale of 1 is NOT a one-pixel MV scale.
    c.mvecScale={1,1};a=lab::nr::sl_guide_constants(c,2560,1080);
    need(a.scale_x==2560&&a.scale_y==1080);
    for(unsigned w:{640u,1280u,2560u,5120u})for(unsigned h:{360u,720u,1080u,2160u}){
        c.mvecScale={-2.f/w,.5f/h};a=lab::nr::sl_guide_constants(c,w,h);
        need(std::abs(a.scale_x+2)<1e-6f&&std::abs(a.scale_y-.5f)<1e-6f);
        c.depthInverted=sl::eFalse;c.reset=sl::eTrue;c.motionVectorsDilated=sl::eTrue;
        c.jitterOffset={-.75f,.375f};const auto b=lab::nr::sl_guide_constants(c,w,h);
        need(b.scale_x==a.scale_x&&b.scale_y==a.scale_y&&!b.depth_inverted&&b.reset);
        c.depthInverted=sl::eTrue;c.reset=sl::eFalse;c.motionVectorsDilated=sl::eFalse;
    }
    for(unsigned axis=0;axis<2;++axis)for(float bad_value:{sl::INVALID_FLOAT,std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::max()/2}){
        auto bad=c;(axis?bad.mvecScale.y:bad.mvecScale.x)=bad_value;
        bool refused=false;try{lab::nr::sl_guide_constants(bad,640,360);}catch(const std::logic_error&){refused=true;}need(refused);
    }
    for(unsigned axis=0;axis<2;++axis)for(unsigned dimension:{0u,8193u,std::numeric_limits<unsigned>::max()}){
        bool refused=false;try{lab::nr::sl_guide_constants(c,axis?640:dimension,axis?dimension:360);}catch(const std::logic_error&){refused=true;}need(refused);
    }
    std::cout<<"PASS guide_constant_checks="<<checked<<" raw_files=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
