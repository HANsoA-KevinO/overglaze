// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_preconvert_reference.hpp"
#include <iostream>
namespace p=lab::preconvert;
void require(bool v){if(!v)throw std::runtime_error("preconvert CPU reference test failed");}
int main(){try {
    require(p::half(1)==0x3c00 && p::half(-2)==0xc000 && p::half(65504)==0x7bff);
    require(p::half(std::ldexp(1.0f,-24))==1 && p::half(std::ldexp(1.0f,-25))==0);
    require(p::half(1.0f+std::ldexp(1.0f,-11))==0x3c00);
    require(p::half(1.0f+3*std::ldexp(1.0f,-11))==0x3c02);
    for(unsigned h=0;h<65536;++h)if((h&0x7c00)!=0x7c00)require(p::half(p::unhalf(static_cast<std::uint16_t>(h)))==h);
    require(p::coordinate(0,7,3,2)==2 && p::coordinate(6,7,3,2)==4);
    require(p::coordinate(6,7,0,2)==2 && p::coordinate(1,2,5,0)==3);
    p::Constants c{{7,9},{3,4},{2,1},{7,9},1,1,1,0,{}};
    p::validate(c,5,5);require(p::rgb(-2,c)==0 && p::rgb(3,c)==3);
    c.hdr=1;require(p::rgb(-2,c)==0 && std::abs(p::rgb(0.003f,c)-0.03876f)<1e-7f);
    require(p::rgb(10,c)<=1 && p::rgb(1,c)>p::rgb(0.75f,c));
    c.paper_white=0;bool rejected=false;try{p::validate(c,5,5);}catch(...){rejected=true;}require(rejected);
    c.hdr=0;c.source_base[0]=3;rejected=false;try{p::validate(c,5,5);}catch(...){rejected=true;}require(rejected);
    std::cout<<"preconvert reference and all finite binary16 round trips passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
