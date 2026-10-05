// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_streamline.hpp"
#include <iostream>
namespace {
unsigned originals=0,virtuals=0;bool throws=false;
struct Token:sl::FrameToken {operator uint32_t()const override{++virtuals;return 7;}} token;
sl::ViewportHandle viewport{0u};sl::ResourceTag resource;
void* command=reinterpret_cast<void*>(0x9000);
void need(bool b,const char* why){if(!b)throw std::runtime_error(why);}
__declspec(noinline) sl::Result get(sl::FrameToken*& out,const uint32_t*){++originals;out=&token;return sl::Result::eOk;}
__declspec(noinline) sl::Result eval(sl::Feature,const sl::FrameToken&,const sl::BaseStructure**,uint32_t,void*){++originals;return sl::Result::eOk;}
__declspec(noinline) sl::Result constants(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&){++originals;return sl::Result::eOk;}
__declspec(noinline) sl::Result tag(const sl::FrameToken& t,const sl::ViewportHandle& v,const sl::ResourceTag* r,uint32_t n,void* c){
    ++originals;need(&t==&token&&&v==&viewport&&r==&resource&&n==1&&c==command,"Five arguments unchanged across x64 stack/register boundary");
    if(throws)throw std::runtime_error("foreign fixture error");return sl::Result::eErrorInvalidParameter;
}
struct Sink:lab::slboundary::Sink {
    unsigned count=0,aborts=0;lab::slboundary::Call last;
    void returned(const lab::slboundary::Call& c)noexcept override{++count;last=c;}
    void aborted(const lab::slboundary::Call&)noexcept override{++aborts;}
};
}
int main(){bool installed=false;try{
    Sink sink;lab::json diagnostics;std::array<void*,4> targets{reinterpret_cast<void*>(&get),reinterpret_cast<void*>(&eval),reinterpret_cast<void*>(&constants),reinterpret_cast<void*>(&tag)};
    need(lab::install_streamline_boundary(targets,&sink,diagnostics,true),"Install frame-tag ABI hook");installed=true;
    auto volatile invoke=reinterpret_cast<lab::SetTagForFrame*>(targets[3]);
    need(invoke(token,viewport,&resource,1,command)==sl::Result::eErrorInvalidParameter,"Original result passthrough");
    need(sink.count==1&&sink.last.api==lab::slboundary::Api::tags&&sink.last.frame_scoped_tags&&sink.last.token==&token&&sink.last.command==command,"Per-frame event identity");
    throws=true;bool caught=false;try{invoke(token,viewport,&resource,1,command);}catch(const std::runtime_error&){caught=true;}
    need(caught&&sink.aborts==1&&sink.count==1,"Original exception not swallowed or reported as success");
    throws=false;lab::detach_streamline_boundary();invoke(token,viewport,&resource,1,command);
    need(originals==3&&virtuals==0&&sink.count==1,"Detached forwarding and no virtual frame-token read");
    lab::uninstall_streamline_observer();installed=false;
    std::cout<<"PASS frame-tag five-argument ABI, immutable forwarding, failed result, abort, detach; no game or GPU execution\n";return 0;
}catch(const std::exception& e){if(installed)lab::uninstall_streamline_observer();std::cerr<<e.what()<<'\n';return 1;}}
