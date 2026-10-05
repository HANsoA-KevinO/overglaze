// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The public Streamline export-table resolution, kept apart from the research
// track's streamline_tests.cpp.
// Provider-clean: no CommandLog, no QueueLog, no Streamline DLL is loaded, and
// kernel32 stands in as a real signed module.
#include "lab_streamline.hpp"
#include <iostream>
#include <string>

namespace {
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
}

int main(){try{
    // The boundary is resolved from the PUBLIC export table, gated on
    // the address lying inside the signed module's executable image. No
    // Streamline DLL is loaded here; kernel32 stands in as a real module.
    namespace pub=lab::slpublic;
    HMODULE k32=GetModuleHandleW(L"kernel32.dll");require(k32!=nullptr,"kernel32 handle for export-resolution test");
    auto* sleep_fn=pub::verified_export(k32,"Sleep");
    require(sleep_fn!=nullptr,"A real executable export resolves");
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(sleep_fn,&info,sizeof(info))&&info.AllocationBase==k32,"Resolved export lies inside its own module image");
    require(pub::verified_export(k32,"LabNoSuchExportName")==nullptr,"Missing export refused");
    require(pub::verified_export(nullptr,"Sleep")==nullptr&&pub::verified_export(k32,nullptr)==nullptr,"Null arguments refused");
    // A forwarded export resolves into ANOTHER module; the image-containment
    // rule must refuse it. Only assert the implication, so the test stays
    // correct whether or not this particular name is a forwarder here.
    if(auto* forwarded=reinterpret_cast<void*>(GetProcAddress(k32,"HeapAlloc"))){
        MEMORY_BASIC_INFORMATION f{};
        if(VirtualQuery(forwarded,&f,sizeof(f))&&f.AllocationBase!=k32)
            require(pub::verified_export(k32,"HeapAlloc")==nullptr,"Export forwarded outside the module is refused");
    }
    const auto none=pub::resolve_public_api(k32);
    require(!none.complete()&&std::string(none.tag_abi())=="none","A module without Streamline exports is incomplete");
    // Tag-ABI selection, matching the install target-array contract.
    auto* a=reinterpret_cast<void*>(1);auto* b=reinterpret_cast<void*>(2);auto* c=reinterpret_cast<void*>(3);
    auto* legacy=reinterpret_cast<void*>(4);auto* per_frame=reinterpret_cast<void*>(5);auto* native=reinterpret_cast<void*>(6);
    pub::PublicApi both;both.token=a;both.evaluate=b;both.constants=c;both.native_interface=native;
    both.legacy_tag=legacy;both.frame_tag=per_frame;
    require(both.complete()&&!both.frame_tagging()&&both.extra_frame_tag()==per_frame&&both.targets()[3]==legacy&&
            std::string(both.tag_abi())=="both-public-abis","SL 2.12 style: hook both public tag ABIs (007)");
    auto only_legacy=both;only_legacy.frame_tag=nullptr;
    require(only_legacy.complete()&&!only_legacy.frame_tagging()&&only_legacy.extra_frame_tag()==nullptr&&
            only_legacy.targets()[3]==legacy&&std::string(only_legacy.tag_abi())=="legacy-tags-only","SL 2.7 style: legacy tags only (2077, Alan Wake 2)");
    auto only_frame=both;only_frame.legacy_tag=nullptr;
    require(only_frame.complete()&&only_frame.frame_tagging()&&only_frame.extra_frame_tag()==nullptr&&
            only_frame.targets()[3]==per_frame&&std::string(only_frame.tag_abi())=="frame-tags-only","Per-frame tagging only");
    auto no_tags=both;no_tags.legacy_tag=nullptr;no_tags.frame_tag=nullptr;
    require(!no_tags.complete(),"No tagging ABI at all is refused");
    auto no_native=both;no_native.native_interface=nullptr;
    require(!no_native.complete(),"Missing slGetNativeInterface is refused");
    const auto report=pub::public_api_report(both);
    require(report["resolution"]=="public-export-table"&&report["tag_abi"]=="both-public-abis"&&report["complete"]==true&&
            report["slGetNativeInterface"]==reinterpret_cast<std::uint64_t>(native),"Public API report states resolution and addresses");
    require(pub::public_api_report(none)["slGetFeatureFunction"].is_null(),"Absent export reported as null, not zero");
    std::cout<<"PASS "<<checks<<" public export-resolution and tag-ABI checks; no Streamline module loaded" << std::endl;
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
