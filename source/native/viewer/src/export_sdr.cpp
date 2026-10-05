// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_preview_export.hpp"
#include <iostream>
// CPU-only export of existing working-color stages. Does not connect to games,
// load NR, reconstruct a missing final frame or alter source files. The export
// lands in the data root the capture itself lives in: a capture is
// <data>\<run>\<pair>\manifest.json, and the export root check requires that
// directory to be named data on a local fixed disk.
int wmain(int argc,wchar_t** argv){try{
    if(argc!=3)throw std::runtime_error("Usage: overglaze_export_sdr <manifest> <new-receipt.json>");
    const std::filesystem::path receipt=argv[2];
    if(!receipt.is_absolute()||std::filesystem::exists(receipt))throw std::runtime_error("New receipt required");
    lab::RawCapture raw(argv[1]);lab::viewer::Display display;display.mapping=3;
    const auto data_root=std::filesystem::canonical(raw.path()).parent_path().parent_path().parent_path();
    const auto output=lab::export_sdr_pair(raw,0,raw.display_pair()?1:3,display,data_root);
    const lab::json result={{"complete",true},{"purpose","user-requested-SDR-viewing"},{"source_manifest",lab::utf8(raw.path().wstring())},
        {"source_manifest_sha256",raw.manifest_hash()},{"directory",lab::utf8(output.wstring())},
        {"recipe_sha256",lab::sha256(output/"display-recipe.json")},{"tool",lab::module_identity(nullptr)},
        {"nr_calls",0},{"game_calls",0},{"originals_modified",false},{"final_game_display_reproduced",false}};
    const auto text=result.dump(2);lab::Handle file(CreateFileW(receipt.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    lab::check(file.valid(),"Create export receipt");DWORD written=0;
    lab::check(WriteFile(file.value,text.data(),DWORD(text.size()),&written,nullptr)&&written==text.size()&&FlushFileBuffers(file.value),"Write export receipt");
    std::cout<<text<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
