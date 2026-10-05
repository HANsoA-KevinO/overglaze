// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_sl_guide_contract.hpp"
#include <iostream>
#include <limits>
namespace {unsigned checks=0;void need(bool v){++checks;if(!v)throw std::runtime_error("Depth projection check failed");}
template<class F>void refuses(F f){bool threw=false;try{f();}catch(const std::logic_error&){threw=true;}need(threw);}}
int main(){try{
    for(float handedness:{-1.f,1.f})for(unsigned inverted:{0u,1u}){
        const float n=.05f,f=50000.f;
        lab::nr::DepthProjection p{handedness*(inverted?n/(n-f):f/(f-n)),inverted?n*f/(f-n):-n*f/(f-n),handedness,0,n,f,inverted,0};p.validate();
        for(float v:{n,.1f,1.f,10.f,100.f,1000.f,f}){
            const double expected=inverted?(double(n)/v-double(n)/f)/(1-double(n)/f):(1-double(n)/v)/(1-double(n)/f);
            need(std::abs(p.project(v)-expected)<2e-6);need(p.project(v)==p.project(-v));}
        need(p.project(0)==float(!inverted));need(p.project(std::numeric_limits<float>::infinity())==float(!inverted));
        need(p.project(std::numeric_limits<float>::quiet_NaN())==float(!inverted));
        need(std::abs(p.project(n/2)-float(inverted))<2e-6);need(std::abs(p.project(f*2)-float(!inverted))<2e-6);
        auto bad=p;bad.c=0;refuses([&]{bad.validate();});bad=p;bad.inverted^=1;refuses([&]{bad.validate();});
        bad=p;bad.b=std::numeric_limits<float>::quiet_NaN();refuses([&]{bad.validate();});
    }
    sl::Constants c;c.cameraViewToClip={};for(unsigned i=0;i<4;++i)c.cameraViewToClip[i]={0,0,0,0};
    c.cameraViewToClip[0].x=1.54545605f;c.cameraViewToClip[1].y=2.74747753f;
    c.cameraViewToClip[2]={0,0,1.0000010206567822e-6f,-1};c.cameraViewToClip[3]={0,0,.05000004917383194f,0};
    c.cameraNear=.05000000074505806f;c.cameraFar=50000;c.depthInverted=sl::eTrue;c.orthographicProjection=sl::eFalse;
    const auto actual=lab::nr::sl_view_depth_projection(c);need(std::abs(actual.project(10.f)-.004999)<2e-6);
    c.cameraViewToClip[0].z=.1f;refuses([&]{lab::nr::sl_view_depth_projection(c);});c.cameraViewToClip[0].z=0;
    c.orthographicProjection=sl::eTrue;refuses([&]{lab::nr::sl_view_depth_projection(c);});
    need(lab::nr::guide_extent_supported(5120,2880,2970,1670));need(lab::nr::guide_extent_supported(1280,720,743,419));
    need(!lab::nr::guide_extent_supported(5120,2880,5121,1670));need(!lab::nr::guide_extent_supported(1280,720,743,0));
    need(!lab::nr::guide_extent_supported(8193,720,743,419));
    std::cout<<"PASS depth projection checks="<<checks<<" GPU=0 NR=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
