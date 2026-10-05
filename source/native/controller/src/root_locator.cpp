// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_root_locator.hpp"
#include "lab_platform.hpp"
#include "lab_windows_path.hpp"
#include "lab_local_path.hpp"
#include <fstream>

namespace lab::root {
namespace fs=std::filesystem;
namespace {
// Every refusal names its kind in brackets, so callers and tests can tell a
// reparse point from a network path without parsing prose.
[[noreturn]] void fail(const char* kind,const std::string& why){throw std::runtime_error(std::string("Lab 根目录无法确定 [root:")+kind+"]："+why);}
std::string text(const fs::path& p){return utf8(p.wstring());}
bool named(const fs::path& p,const wchar_t* name){return winpath::same_spelling(p.filename(),name);}
bool build_directory(const fs::path& p){const auto n=p.filename().wstring();
    return n.size()>=6&&CompareStringOrdinal(n.c_str(),6,L"_build",6,TRUE)==CSTR_EQUAL;}
bool drive_letter(wchar_t c){return (c>=L'A'&&c<=L'Z')||(c>=L'a'&&c<=L'z');}
}

const char* Location::source_name()const noexcept{
    switch(source){case Source::override_file:return "override-file";case Source::app_layout:return "app-layout";case Source::build_layout:return "build-layout";}
    return "unknown";}

// The rules themselves live in core (lab_local_path.hpp) since host contract V4,
// because the in-game installation contract applies them to the data root its
// install record holds. Same kinds, same "[root:<kind>]" spelling here.
void require_local_fixed(const fs::path& p,bool must_exist,const DriveType& drive){
    localpath::require_local_fixed(p,must_exist,drive,"Lab 根目录无法确定","root");
}

Location resolve_from(const fs::path& executable,const DriveType& drive){
    require_local_fixed(executable,true,drive);
    std::error_code ec;
    if(!fs::is_regular_file(executable,ec))fail("path","不是程序文件："+text(executable));
    const auto dir=executable.parent_path();
    Location out;out.override_file=dir/kOverrideFile;
    fs::path root;
    const auto attributes=GetFileAttributesW(out.override_file.c_str());
    const auto error=attributes==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
    if(attributes==INVALID_FILE_ATTRIBUTES&&error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)
        fail("override","无法读取覆盖配置（Win32 "+std::to_string(error)+"）："+text(out.override_file));
    if(attributes!=INVALID_FILE_ATTRIBUTES){
        // Present means meant: a broken override is reported, never skipped in
        // favour of the layout, or the program would quietly use another root.
        if(attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))fail("override","覆盖配置不是普通文件："+text(out.override_file));
        const auto size=fs::file_size(out.override_file,ec);
        if(ec||size>16*1024)fail("override","覆盖配置无法读取或超过 16 KiB："+text(out.override_file));
        json j;
        try{std::ifstream in(out.override_file,std::ios::binary);j=json::parse(in);}catch(const std::exception&){fail("override","覆盖配置不是有效 JSON："+text(out.override_file));}
        if(!j.is_object()||j.value("schema","")!=kOverrideSchema||!j.contains("lab_root")||!j.at("lab_root").is_string())
            fail("override",std::string("覆盖配置需要 schema \"")+kOverrideSchema+"\" 和字符串 lab_root："+text(out.override_file));
        for(auto it=j.begin();it!=j.end();++it)if(it.key()!="schema"&&it.key()!="lab_root"&&it.key()!="note")
            fail("override","覆盖配置含未知字段 "+it.key()+"："+text(out.override_file));
        root=fs::path(wide(j.at("lab_root").get<std::string>()));
        out.source=Source::override_file;
    }else if(named(dir,L"app")){root=dir.parent_path();out.source=Source::app_layout;}
    else if(build_directory(dir)&&named(dir.parent_path(),L"data")){root=dir.parent_path().parent_path();out.source=Source::build_layout;}
    else fail("layout","程序不在 <根>\\app\\ 或 <根>\\data\\_build*\\ 下，旁边也没有 overglaze-root.json："+text(executable));
    if(root.empty()||!root.has_relative_path())fail("drive-root","Lab 根目录不能是整块磁盘的根："+text(root));
    require_local_fixed(root,true,drive);
    if(!fs::is_directory(root,ec))fail("missing","Lab 根目录不是目录："+text(root));
    out.root=root;out.data=root/L"data";
    return out;
}

fs::path self_executable(){
    std::wstring b(MAX_PATH,L'\0');
    for(;;){const auto n=GetModuleFileNameW(nullptr,b.data(),static_cast<DWORD>(b.size()));
        if(!n)fail("self","GetModuleFileNameW 失败（Win32 "+std::to_string(GetLastError())+"）");
        if(n<b.size()){b.resize(n);break;}
        if(b.size()>=32768)fail("self","程序路径过长");b.resize(b.size()*2);}
    // A long-path spelling of a drive path is the same location; \\?\UNC\ is not
    // stripped and is refused like any other network path.
    if(b.size()>6&&b.compare(0,4,L"\\\\?\\")==0&&drive_letter(b[4])&&b[5]==L':')b.erase(0,4);
    return fs::path(b);
}
Location resolve_self(){return resolve_from(self_executable());}
}
