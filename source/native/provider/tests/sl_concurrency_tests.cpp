// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_streamline.hpp"
#include <atomic>
#include <iostream>
#include <latch>
#include <mutex>
#include <thread>

// No game, NR, D3D12 device or textures. Real hooks exercise two threads at
// either the original API boundary or our post-return callback boundary.
namespace {
using namespace lab::slboundary;
std::latch* entered=nullptr;
std::latch* release=nullptr;
bool hold_original=false,hold_callback=false;
struct Token final:sl::FrameToken {operator std::uint32_t()const override{return 1;}} token;
__declspec(noinline) sl::Result get(sl::FrameToken*& out,const std::uint32_t*){out=&token;return sl::Result::eOk;}
__declspec(noinline) sl::Result set(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&){return sl::Result::eOk;}
__declspec(noinline) sl::Result tags(const sl::ViewportHandle&,const sl::ResourceTag*,std::uint32_t,sl::CommandBuffer*){return sl::Result::eOk;}
__declspec(noinline) sl::Result evaluate(sl::Feature,const sl::FrameToken&,const sl::BaseStructure**,std::uint32_t,sl::CommandBuffer*){
    if(hold_original){entered->count_down();release->wait();}return sl::Result::eOk;
}
struct Receiver final:Sink {
    std::mutex mutex;Call token_call{},evaluate_call{};
    void returned(const Call& c)noexcept override{
        {std::lock_guard lock(mutex);if(c.api==Api::token)token_call=c;else if(c.api==Api::evaluate)evaluate_call=c;}
        if(c.api==Api::evaluate&&hold_callback){entered->count_down();release->wait();}
    }
};
void need(bool v,const char* why){if(!v)throw std::runtime_error(why);}
}
int main(){bool installed=false;try{
    Receiver receiver;lab::json diagnostics;
    std::array<void*,4> targets{reinterpret_cast<void*>(get),reinterpret_cast<void*>(evaluate),reinterpret_cast<void*>(set),reinterpret_cast<void*>(tags)};
    need(lab::install_streamline_boundary(targets,&receiver,diagnostics),"Concurrency fixture hook install");installed=true;
    auto volatile get_fn=reinterpret_cast<PFun_slGetNewFrameToken*>(targets[0]);
    auto volatile eval_fn=reinterpret_cast<PFun_slEvaluateFeature*>(targets[1]);
    sl::ViewportHandle viewport(0u);const sl::BaseStructure* input[]{&viewport};
    for(bool original:{true,false}){
        std::latch reached(1),resume(1);entered=&reached;release=&resume;hold_original=original;hold_callback=!original;
        std::atomic<bool> forwarded=false;
        std::jthread worker([&]{forwarded=eval_fn(sl::kFeatureDLSS_RR,token,input,1,nullptr)==sl::Result::eOk;});
        reached.wait();sl::FrameToken* out=nullptr;const std::uint32_t frame=7;
        const auto result=get_fn(out,&frame);resume.count_down();worker.join();
        need(result==sl::Result::eOk&&out==&token&&forwarded,"Original results preserved across overlap");
        need(receiver.token_call.thread!=receiver.evaluate_call.thread,"Real independent calling threads");
        std::cout<<(original?"original":"post-return")<<": token.concurrent="<<receiver.token_call.concurrent
                 <<" evaluate.concurrent="<<receiver.evaluate_call.concurrent<<'\n';
        if(original){need(receiver.token_call.concurrent&&receiver.evaluate_call.concurrent,"Actual original API overlap must remain rejected");
            // Plague Tale fetches its frame token ~10x per frame from other threads.
            need(receiver.evaluate_call.token_only_overlap,"An Evaluate overlapped only by a token fetch says so");
            need(!receiver.token_call.token_only_overlap,"A token fetch that overlapped an Evaluate is not token-only");}
        else{need(!receiver.token_call.concurrent&&!receiver.evaluate_call.concurrent,"Post-return Lab callback alone must not be labelled original API overlap");}
        need(lab::streamline_snapshot()["boundary_active_calls"]==0,"All callback lifetime leases balanced");
    }
    // A tag call overlapping the Evaluate is a real conflict, never token-only.
    {std::latch reached(1),resume(1);entered=&reached;release=&resume;hold_original=true;hold_callback=false;
     std::jthread worker([&]{eval_fn(sl::kFeatureDLSS_RR,token,input,1,nullptr);});
     reached.wait();auto volatile tag_fn=reinterpret_cast<PFun_slSetTag*>(targets[3]);tag_fn(viewport,nullptr,0,nullptr);
     resume.count_down();worker.join();
     need(receiver.evaluate_call.concurrent&&!receiver.evaluate_call.token_only_overlap,"An Evaluate overlapped by a tag call is a real conflict");
     need(lab::streamline_snapshot()["boundary_active_calls"]==0,"Leases balanced after the tag overlap");}
    lab::uninstall_streamline_observer();installed=false;
    std::cout<<"PASS real-hook concurrent-original and post-return scopes; NR=0; raw_files=0\n";return 0;
}catch(const std::exception& e){if(installed)lab::uninstall_streamline_observer();std::cerr<<e.what()<<'\n';return 1;}}
