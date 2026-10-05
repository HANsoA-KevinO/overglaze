// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_fixture_origin.hpp"
#include <iostream>
int main(){try {
    const std::filesystem::path root(L"D:\\Overglaze\\data");
    const std::string hash(64,'a');lab::json identity={{"path","D:\\Overglaze\\data\\fixture-1\\lab_reshade_present_harness.exe"},{"sha256",hash}};
    if(!lab::valid_reshade_fixture_identity(identity,hash,root))throw std::runtime_error("Valid test identity rejected");
    for(const char* path:{"D:\\Steam\\steamapps\\common\\Cyberpunk 2077\\bin\\x64\\Cyberpunk2077.exe",
        "D:\\Overglaze\\data\\fixture-1\\Cyberpunk2077.exe",
        "D:\\Overglaze\\data-other\\fixture-1\\lab_reshade_present_harness.exe",
        "D:\\Overglaze\\data\\..\\fixture-1\\lab_reshade_present_harness.exe",
        "D:\\Overglaze\\data\\lab_reshade_present_harness.exe",
        "fixture-1\\lab_reshade_present_harness.exe"}){
        auto bad=identity;bad["path"]=path;if(lab::valid_reshade_fixture_identity(bad,hash,root))throw std::runtime_error("Invalid fixture path accepted");}
    if(lab::valid_reshade_fixture_identity(identity,std::string(64,'b'),root) || lab::valid_reshade_fixture_identity(identity,std::string(64,'G'),root)
        || lab::valid_reshade_fixture_identity(lab::json::object(),hash,root) || lab::valid_reshade_fixture_identity(identity,"",root))throw std::runtime_error("Missing/changed hash accepted");
    if(lab::valid_reshade_fixture_identity(identity,hash,L"relative\\data") || lab::valid_reshade_fixture_identity(identity,hash,L"G:\\")
        || lab::valid_reshade_fixture_identity(identity,hash,L"D:\\other\\data"))throw std::runtime_error("V4: an unusable or different data root accepted");
    std::cout<<"PASS: exact workspace fixture executable and lowercase SHA-256, rejects game/relative/traversal/prefix-confusion identities\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
