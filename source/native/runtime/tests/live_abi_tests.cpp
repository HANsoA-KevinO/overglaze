// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Live ABI boundary between the runtime and research bridge variants (ABI24 adds the game's exposure to
// Frame and auto exposure's source to Status; ABI23 adds frame regions; ABI22 adds
// Skin/AutoMask to Settings; ABI21 adds the render-queue handoff the late-load panel needs).
//
//   no arguments        layout only: the runtime structures stay trivially
//                       copyable, bounded, and free of every research type.
//   controller <dll>    that bridge exports the 15 runtime entry points and
//                       none of the research ones.
//   research <dll>      that bridge exports both sets.
//
// The export table is read out of the file on disk, so no bridge is loaded and
// no NGX, D3D12 or model dependency is touched.
#include "lab_nr_live_api.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}

// The runtime entry points every bridge variant must provide.
const char* runtime_entry_points[]={
    "LabNrLiveStart","LabNrLiveCapabilities","LabNrLiveEnableBindingPreservation",
    "LabNrLiveGameBindingEntryV1","LabNrLiveGameBindingExitV1","LabNrLivePoll","LabNrLivePollBindings",
    "LabNrLiveRequest","LabNrLiveApply","LabNrLiveConfigure","LabNrLiveEnter",
    "LabNrLiveBoundaryReturned","LabNrLiveStop","LabNrLiveReject",
    "LabNrLiveRenderQueue", // ABI21
    "LabNrLiveBindingPolicyV1"}; // optional: late-attach unknown-indirect policy

template<class T> T read_at(const std::vector<char>& image,std::size_t offset){
    if(offset+sizeof(T)>image.size())throw std::runtime_error("Truncated PE image");
    T value{};std::memcpy(&value,image.data()+offset,sizeof(T));return value;
}
// Exported names of a PE64 file, resolved through its own section table.
std::set<std::string> exported_names(const std::filesystem::path& file){
    std::ifstream in(file,std::ios::binary);
    if(!in)throw std::runtime_error("Cannot read "+file.string());
    std::vector<char> image((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    const auto dos=read_at<IMAGE_DOS_HEADER>(image,0);
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE)throw std::runtime_error("Not a PE file");
    const auto headers=read_at<IMAGE_NT_HEADERS64>(image,static_cast<std::size_t>(dos.e_lfanew));
    if(headers.Signature!=IMAGE_NT_SIGNATURE||headers.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        throw std::runtime_error("Not a 64-bit PE image");
    const auto sections=static_cast<std::size_t>(dos.e_lfanew)+sizeof(IMAGE_NT_HEADERS64);
    const auto to_offset=[&](DWORD rva)->std::size_t{
        for(unsigned i=0;i<headers.FileHeader.NumberOfSections;++i){
            const auto s=read_at<IMAGE_SECTION_HEADER>(image,sections+i*sizeof(IMAGE_SECTION_HEADER));
            if(rva>=s.VirtualAddress&&rva<s.VirtualAddress+(std::max)(s.Misc.VirtualSize,s.SizeOfRawData))
                return s.PointerToRawData+(rva-s.VirtualAddress);
        }
        throw std::runtime_error("RVA outside every section");
    };
    const auto& directory=headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    std::set<std::string> names;
    if(!directory.VirtualAddress||!directory.Size)return names;
    const auto exports=read_at<IMAGE_EXPORT_DIRECTORY>(image,to_offset(directory.VirtualAddress));
    const auto table=to_offset(exports.AddressOfNames);
    for(DWORD i=0;i<exports.NumberOfNames;++i){
        const auto name=to_offset(read_at<DWORD>(image,table+i*sizeof(DWORD)));
        names.emplace(image.data()+name);
    }
    return names;
}
}

int main(int argc,char** argv){try{
    using namespace lab::live;
    // The runtime ABI carries no collector type. A research structure that
    // found its way back into Frame/Status would change these sizes.
    static_assert(std::is_trivially_copyable_v<Frame>&&std::is_trivially_copyable_v<Status>);
    static_assert(sizeof(Frame)==176&&sizeof(Status)==1152&&sizeof(Capabilities)==92, // ABI24: Frame game exposure, Status exposure source and counts
        "Live ABI24 runtime layout changed; review every host and bump the version");
    static_assert(version==24);
    static_assert(exposure_note_count==19&&static_cast<unsigned>(ExposureSource::game)==2,"ABI24 exposure enums");
    {   // A frame source that says nothing about exposure says "no texture", at
        // pre-exposure and scale 1: the runtime then stays on its meter.
        Frame f;need(!f.exposure&&f.exposure_note==static_cast<unsigned>(ExposureNote::no_texture)&&f.pre_exposure==1.f&&f.exposure_scale==1.f,
            "Frame exposure defaults");
        Status s;need(s.exposure_source==static_cast<unsigned>(ExposureSource::manual)&&!s.game_exposure_valid&&s.exposure_notes[0]==0,"Status exposure defaults");}
    need(research_entry_point_count==15,"Research entry point list (15, including the chain capture)");
    Capabilities capabilities;
    need(capabilities.size==sizeof(Capabilities)&&capabilities.abi==version&&!capabilities.variant,"Capabilities default to an unknown variant");

    if(argc==1){std::cout<<"PASS "<<checks<<" live ABI24 layout checks; runtime structures free of research collectors\n";return 0;}
    need(argc==3,"Usage: lab_live_abi_tests [controller|research <bridge dll>]");
    const std::string variant=argv[1];
    const auto names=exported_names(std::filesystem::canonical(argv[2]));
    for(const auto* entry:runtime_entry_points)need(names.count(entry)==1,std::string("Missing runtime entry point ")+entry);
    unsigned research_present=0;
    for(const auto* entry:research_entry_points)research_present+=names.count(entry);
    if(variant=="controller"){
        need(research_present==0,"The controller bridge must export no research entry point");
        // Nothing beyond the runtime set, so no collector can be reached by name.
        for(const auto& name:names){
            bool known=false;
            for(const auto* entry:runtime_entry_points)known=known||name==entry;
            need(known,"Unexpected controller bridge export: "+name);
        }
        std::cout<<"PASS "<<checks<<" checks; controller bridge exports only the "<<names.size()<<" runtime entry points\n";
    }else{
        need(variant=="research","Unknown variant");
        need(research_present==research_entry_point_count,"The research bridge must export every research entry point");
        std::cout<<"PASS "<<checks<<" checks; research bridge exports the runtime set plus "<<research_present<<" research entry points\n";
    }
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
