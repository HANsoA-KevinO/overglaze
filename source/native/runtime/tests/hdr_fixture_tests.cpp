// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_hdr_fixture.hpp"
#include <iostream>
int main(){try{
    using namespace lab;const auto require=[](bool b){if(!b)throw std::runtime_error("HDR fixture invariant failed");};
    require(!hdr_fixture::valid("pq"));
    bool rejected=false;try{hdr_fixture::pixel(0,0,0,720,"direct");}catch(const std::invalid_argument&){rejected=true;}require(rejected);
    double max_direct=0;unsigned collapsed=0;
    for(unsigned y=0;y<720;y+=7)for(unsigned x=0;x<1280;++x){
        auto a=hdr_fixture::pixel(x,y,1280,720,"bounded"),b=hdr_fixture::pixel(x,y,1280,720,"direct"),
            c=hdr_fixture::pixel(x,y,1280,720,"clipped"),d=hdr_fixture::pixel(x,y,1280,720,"encoded");
        for(unsigned k=0;k<3;++k){
            float low=preconvert::unhalf(a[k]),high=preconvert::unhalf(b[k]),clip=preconvert::unhalf(c[k]),enc=preconvert::unhalf(d[k]);
            require(high==low*16 && clip==std::min(high,1.f) && std::isfinite(enc) && enc>=0 && enc<=1);
            if(high>1){require(clip==1);++collapsed;}max_direct=std::max(max_direct,double(high));
        }
        require(a[3]==0x3c00 && b[3]==0x3c00 && c[3]==0x3c00 && d[3]==0x3c00);
    }
    require(max_direct==16 && collapsed>1000);std::cout<<"HDR numeric fixture invariants passed; no NR executed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
