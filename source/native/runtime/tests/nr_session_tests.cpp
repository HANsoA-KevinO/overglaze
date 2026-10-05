// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#define NGX_SNIPPET_BUILD
#include "lab_nr_session.hpp"
#include <iostream>
#include <atomic>
#include <memory>

#define REQUIRE(expr) do { if (!(expr)) throw std::runtime_error("FAILED: " #expr); } while (false)
namespace {
struct Fixture {
    unsigned init=0, create=0, evaluate=0, release=0, shutdown=0;
    NVSDK_NGX_Result init_result=NVSDK_NGX_Result_Success, create_result=NVSDK_NGX_Result_Success;
    NVSDK_NGX_Result evaluate_result=NVSDK_NGX_Result_Success, release_result=NVSDK_NGX_Result_Success;
    bool null_handle=false, throw_release=false;
} *current=nullptr;
const auto failure=static_cast<NVSDK_NGX_Result>(0xbad);
// Opaque sentinels are used only by the fake API. Never dereferenced as COM/NGX.
auto* device=reinterpret_cast<ID3D12Device*>(1);
auto* commands=reinterpret_cast<ID3D12GraphicsCommandList*>(2);
auto* parameters=reinterpret_cast<const NVSDK_NGX_Parameter*>(3);
auto* handle=reinterpret_cast<NVSDK_NGX_Handle*>(4);
NVSDK_NGX_Result NVSDK_CONV init(unsigned long long id,const wchar_t*,ID3D12Device* d,NVSDK_NGX_Version version,const NVSDK_NGX_Parameter* p) {
    REQUIRE(id==0 && d==device && p==parameters && version==NVSDK_NGX_Version_API);
    ++current->init;return current->init_result;
}
NVSDK_NGX_Result NVSDK_CONV create(ID3D12GraphicsCommandList* c,NVSDK_NGX_Feature feature,const NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** out) {
    REQUIRE(c==commands && static_cast<unsigned>(feature)==18 && p==parameters && out);
    ++current->create;*out=current->null_handle?nullptr:handle;return current->create_result;
}
NVSDK_NGX_Result NVSDK_CONV evaluate(ID3D12GraphicsCommandList* c,const NVSDK_NGX_Handle* h,const NVSDK_NGX_Parameter* p,PFN_NVSDK_NGX_ProgressCallback callback) {
    REQUIRE(c==commands && h==handle && p==parameters && callback==nullptr);
    ++current->evaluate;return current->evaluate_result;
}
NVSDK_NGX_Result NVSDK_CONV release(NVSDK_NGX_Handle* h) {
    REQUIRE(h==handle);++current->release;
    if(current->throw_release) throw std::runtime_error("foreign cleanup failure");
    return current->release_result;
}
NVSDK_NGX_Result NVSDK_CONV shutdown(ID3D12Device* d) {REQUIRE(d==device);++current->shutdown;return NVSDK_NGX_Result_Success;}
lab::nr::Api api(){return {init,create,evaluate,release,shutdown};}
template<class F> void refused(F&& call) {
    bool rejected=false;try{call();}catch(const std::logic_error&){rejected=true;}
    REQUIRE(rejected);
}
void ready(lab::nr::Session& s) {
    REQUIRE(s.initialize(device,L"synthetic",parameters)==NVSDK_NGX_Result_Success);
    REQUIRE(s.create(commands,parameters)==NVSDK_NGX_Result_Success);
    s.acknowledge_outer_queue_completion();
}
}

int main() {
    try {
        {auto incomplete=api();incomplete.evaluate=nullptr;refused([&]{lab::nr::Session s(incomplete);});}
        {
            Fixture f;current=&f;lab::nr::Session s(api());
            refused([&]{s.evaluate(commands,parameters);});refused([&]{s.create(commands,parameters);});
            refused([&]{s.initialize(nullptr,L"synthetic",parameters);});
            REQUIRE(s.initialize(device,L"synthetic",parameters)==NVSDK_NGX_Result_Success);
            refused([&]{s.initialize(device,L"synthetic",parameters);});
            REQUIRE(s.create(commands,parameters)==NVSDK_NGX_Result_Success);
            REQUIRE(s.pending());
            refused([&]{s.release();});refused([&]{s.shutdown();});refused([&]{s.evaluate(commands,parameters);});
            s.acknowledge_outer_queue_completion();
            refused([&]{s.acknowledge_outer_queue_completion();});
            for(int i=0;i<3;++i){REQUIRE(s.evaluate(commands,parameters)==NVSDK_NGX_Result_Success);s.acknowledge_outer_queue_completion();}
            REQUIRE(s.successful_evaluates()==3 && s.outer_queue_completions()==4);
            REQUIRE(s.release()==NVSDK_NGX_Result_Success);refused([&]{s.release();});
            REQUIRE(s.shutdown()==NVSDK_NGX_Result_Success);refused([&]{s.shutdown();});
            REQUIRE(!s.initialized() && !s.has_feature() && !s.pending() && !s.abandoned());
            REQUIRE(f.init==1 && f.create==1 && f.evaluate==3 && f.release==1 && f.shutdown==1);
        }
        {
            Fixture f;f.init_result=failure;current=&f;lab::nr::Session s(api());
            REQUIRE(s.initialize(device,L"synthetic",parameters)==failure);
            refused([&]{s.create(commands,parameters);});refused([&]{s.shutdown();});
            REQUIRE(f.create==0 && f.shutdown==0);
        }
        for(bool missing : {false,true}) {
            Fixture f;f.create_result=missing?NVSDK_NGX_Result_Success:failure;f.null_handle=missing;current=&f;
            lab::nr::Session s(api());s.initialize(device,L"synthetic",parameters);s.create(commands,parameters);
            REQUIRE(s.pending() && s.operation_failed());refused([&]{s.release();});
            s.acknowledge_outer_queue_completion();refused([&]{s.evaluate(commands,parameters);});
            if(s.has_feature())s.release();s.shutdown();REQUIRE(f.evaluate==0);
        }
        {
            Fixture f;current=&f;lab::nr::Session s(api());ready(s);f.evaluate_result=failure;
            REQUIRE(s.evaluate(commands,parameters)==failure);REQUIRE(s.pending());refused([&]{s.release();});
            s.acknowledge_outer_queue_completion();refused([&]{s.evaluate(commands,parameters);});
            s.release();s.shutdown();REQUIRE(f.evaluate==1);
        }
        {
            Fixture f;current=&f;
            {lab::nr::Session s(api());ready(s);s.evaluate(commands,parameters);s.abandon();
             refused([&]{s.acknowledge_outer_queue_completion();});refused([&]{s.release();});refused([&]{s.shutdown();});}
            REQUIRE(f.release==0 && f.shutdown==0); // No destructor cleanup on unknown completion.
        }
        for(bool foreign_exception : {false,true}) {
            Fixture f;current=&f;lab::nr::Session s(api());ready(s);f.release_result=failure;f.throw_release=foreign_exception;
            if(foreign_exception){bool caught=false;try{s.release();}catch(const std::runtime_error&){caught=true;}REQUIRE(caught);}
            else REQUIRE(s.release()==failure);
            REQUIRE(s.abandoned());refused([&]{s.release();});refused([&]{s.shutdown();});
            REQUIRE(f.release==1 && f.shutdown==0);
        }
        {
            Fixture f;current=&f;lab::nr::Session s(api());ready(s);std::atomic<bool> rejected=false;
            std::thread other([&]{try{s.evaluate(commands,parameters);}catch(const std::logic_error&){rejected=true;}});other.join();
            REQUIRE(rejected && f.evaluate==0);s.release();s.shutdown();
        }
        {
            Fixture f;current=&f;lab::SerialCallGate gate,wrong;
            refused([&]{lab::nr::Session invalid(api(),&gate);});
            std::unique_ptr<lab::nr::Session> s;
            {
                auto lease=gate.try_enter();REQUIRE(lease);
                s=std::make_unique<lab::nr::Session>(api(),&gate);ready(*s);
                auto nested=gate.try_enter();REQUIRE(!nested); // Never recursively admit a host callback.
                bool busy=false,direct_refused=false;
                std::thread competitor([&]{auto other=gate.try_enter();busy=!other;
                    try{s->evaluate(commands,parameters);}catch(const std::logic_error&){direct_refused=true;}});
                competitor.join();REQUIRE(busy&&direct_refused&&f.evaluate==0);
            }
            refused([&]{s->pending();});
            {auto unrelated=wrong.try_enter();REQUIRE(unrelated);refused([&]{s->evaluate(commands,parameters);});}
            // Sequential different-thread calls share ONE Feature, not one
            // Feature per thread. CPU lease release never retires GPU work.
            for(unsigned frame=0;frame<3;++frame){
                std::exception_ptr error;
                std::thread render([&]{try{auto lease=gate.try_enter();REQUIRE(lease);
                    REQUIRE(s->evaluate(commands,parameters)==NVSDK_NGX_Result_Success);
                }catch(...){error=std::current_exception();}});render.join();if(error)std::rethrow_exception(error);
                auto completion=gate.try_enter();REQUIRE(completion&&s->pending());
                refused([&]{s->evaluate(commands,parameters);});refused([&]{s->release();});
                s->acknowledge_outer_queue_completion();
            }
            try {auto lease=gate.try_enter();REQUIRE(lease);throw std::runtime_error("host exception");}
            catch(const std::runtime_error&){}
            {auto lease=gate.try_enter();REQUIRE(lease);REQUIRE(s->successful_evaluates()==3&&f.create==1);
                f.evaluate_result=failure;REQUIRE(s->evaluate(commands,parameters)==failure);}
            {auto lease=gate.try_enter();REQUIRE(lease&&s->operation_failed()&&s->pending());
                refused([&]{s->release();});s->acknowledge_outer_queue_completion();
                refused([&]{s->evaluate(commands,parameters);});s->release();s->shutdown();}
            REQUIRE(f.init==1&&f.create==1&&f.evaluate==4&&f.release==1&&f.shutdown==1);
        }
        {
            lab::SerialCallGate gate;unsigned serialized=0;std::atomic<unsigned> accepted=0;
            // Bounded contention test: losing callers skip, they do not spin
            // until admitted. Protected ordinary memory crosses release/acquire.
            auto worker=[&]{for(unsigned i=0;i<10000;++i){auto lease=gate.try_enter();if(!lease)continue;
                ++serialized;++accepted;}};
            std::thread a(worker),b(worker);a.join();b.join();REQUIRE(accepted>0&&serialized==accepted);
        }
        std::cout<<"PASS: typed snippet session, pending/failed work, fixed owner and explicit serial migration, contention/reentry, no implicit cleanup or retry\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
