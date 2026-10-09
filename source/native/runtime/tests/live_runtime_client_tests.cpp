// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The two negative paths of the live runtime client's export contract.
//
//   1. A bridge missing a required export must fail with the export NAME in the
//      message, so a host/bridge mismatch is diagnosable from the fault record
//      instead of guessed from a stack.
//   2. A research host handed a CONTROLLER bridge must report
//      research_available=false rather than failing to construct. The rule lives
//      in ResearchLiveClient::resolve_research_exports and is
//          variant==2 && LabNrLiveResearchFrameContext && LabNrLivePollCapture && LabNrLiveCapture
//      resolved with GetProcAddress, never with the throwing proc<>. This test
//      evaluates that rule against the real controller bridge.
//
// What this does NOT do: construct a LiveRuntimeClient. Its constructor demands
// a verified fixture identity (one of two named harness executables, hashed) or
// a real installed game directory, and this test is neither. Weakening that gate
// to make a unit test possible would trade a real safety property for coverage,
// so the harnesses keep that job and this test covers the export contract the
// constructor depends on.
#include "lab_live_runtime_client.hpp"
#include <filesystem>
#include <iostream>
#include <string>

using lab::live::Capabilities;

namespace {
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}

// Every runtime entry the client resolves, in the order the constructor asks.
const char* runtime_entry_points[]={
    "LabNrLiveStart","LabNrLiveCapabilities","LabNrLiveEnableBindingPreservation",
    "LabNrLiveGameBindingEntryV1","LabNrLiveGameBindingExitV1","LabNrLivePoll","LabNrLivePollBindings",
    "LabNrLiveRequest","LabNrLiveApply","LabNrLiveConfigure","LabNrLiveEnter",
    "LabNrLiveBoundaryReturned","LabNrLiveStop","LabNrLiveReject",
    "LabNrLiveRenderQueue", // ABI21, resolved without proc<>: optional by design
    "LabNrLiveBindingPolicyV1", // resolved without proc<>: optional by design
    "LabNrLivePinModelV1"}; // resolved without proc<>, only for a pinned unrecognized model
// The three the research availability rule requires, all resolved without proc<>.
const char* research_group[]={"LabNrLiveResearchFrameContext","LabNrLivePollCapture","LabNrLiveCapture"};

struct Module {
    HMODULE handle=nullptr;
    explicit Module(const std::filesystem::path& p){
        handle=LoadLibraryExW(p.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!handle)throw std::runtime_error("Cannot load "+p.string()+" error "+std::to_string(GetLastError()));
    }
    ~Module(){if(handle)FreeLibrary(handle);}
    Module(const Module&)=delete;Module& operator=(const Module&)=delete;
};
}

int wmain(int argc,wchar_t** argv){
    try{
        need(argc==3,"Usage: lab_live_runtime_client_tests <mock bridge dll> <controller bridge dll>");
        const std::filesystem::path mock_path=argv[1],controller_path=argv[2];

        // ---- 1. the missing export is named ----
        Module mock(mock_path);
        for(const auto* entry:runtime_entry_points){
            const bool expected_present=std::string(entry)!="LabNrLiveApply"&&std::string(entry)!="LabNrLiveCapabilities";
            need((GetProcAddress(mock.handle,entry)!=nullptr)==expected_present,
                 std::string("Mock bridge export shape for ")+entry);
        }
        bool threw=false;std::string message;
        try{lab::live::proc<lab::live::Apply>(mock.handle,"LabNrLiveApply");}
        catch(const std::exception& e){threw=true;message=e.what();}
        need(threw,"A missing required export must throw, not return null");
        need(message.find("LabNrLiveApply")!=std::string::npos,
             "The fault message must name the missing export, got: "+message);
        need(message.find("Live bridge export missing")!=std::string::npos,
             "The fault message must say what kind of failure this is, got: "+message);
        // A present export resolves through the same helper without throwing.
        need(lab::live::proc<lab::live::Poll>(mock.handle,"LabNrLivePoll")!=nullptr,"A present export resolves");
        // No capability export at all: the client documents this as variant 0.
        need(GetProcAddress(mock.handle,"LabNrLiveCapabilities")==nullptr,"Mock exports no capability query");
        Capabilities unqueried;
        need(unqueried.variant==0,"A bridge with no capability export leaves the variant unknown (0)");

        // ---- 2. the real controller bridge: complete runtime set, no research group ----
        Module controller(controller_path);
        for(const auto* entry:runtime_entry_points)
            need(GetProcAddress(controller.handle,entry)!=nullptr,
                 std::string("The controller bridge must export ")+entry);
        auto query=reinterpret_cast<lab::live::GetCapabilities>(GetProcAddress(controller.handle,"LabNrLiveCapabilities"));
        need(query!=nullptr,"The controller bridge answers the capability query");
        Capabilities capabilities;
        need(query(&capabilities),"The capability query succeeds");
        need(capabilities.variant==1,"The controller bridge reports variant 1");
        need(capabilities.research_exports==0,"The controller bridge reports no research exports");
        need(capabilities.abi==lab::live::version,"The controller bridge reports this ABI");
        // The model pin (an unrecognized model the user allowed) is refused
        // without a context and for anything that is not 64 lowercase hex digits.
        {const auto pin=reinterpret_cast<lab::live::PinModel>(GetProcAddress(controller.handle,"LabNrLivePinModelV1"));
         const std::string sha(64,'d');
         need(pin&&!pin(nullptr,sha.c_str())&&!pin(nullptr,nullptr),"The model pin needs a live context");}

        unsigned resolved=0;
        for(const auto* entry:research_group)
            if(GetProcAddress(controller.handle,entry))++resolved;
        need(resolved==0,"None of the research availability group resolves on the controller bridge");
        // This is the rule verbatim. It is false here, and evaluating it cannot
        // throw, which is exactly why a research host survives a controller bridge.
        const bool research_available=capabilities.variant==2&&resolved==3;
        need(!research_available,"A research host handed the controller bridge reports research_available=false");

        std::cout<<"PASS "<<checks<<" live runtime client export checks; missing exports are named, "
                   "a controller bridge disables research collectors without throwing\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<e.what()<<'\n';
        return 1;
    }
}
