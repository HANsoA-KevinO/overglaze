// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_local_path.hpp"
#include "lab_platform.hpp"

namespace lab::localpath {
namespace fs=std::filesystem;
namespace {
bool drive_letter(wchar_t c){return (c>=L'A'&&c<=L'Z')||(c>=L'a'&&c<=L'z');}
std::string text(const fs::path& p){return utf8(p.wstring());}
}

void require_local_fixed(const fs::path& p,bool must_exist,const DriveType& drive,const char* title,const char* tag){
    // Every refusal names its kind in brackets, so callers and tests can tell a
    // reparse point from a network path without parsing prose.
    const auto fail=[&](const char* kind,const std::string& why){throw std::runtime_error(std::string(title)+" ["+tag+":"+kind+"]："+why);};
    const auto& s=p.native();
    if(s.empty()||s.size()>32000||s.find(L'\0')!=std::wstring::npos)fail("path","路径为空或过长："+text(p));
    // UNC, \\?\, \\.\ and //server: decided on the spelling alone, before any
    // file system call could reach out to a network share.
    if(s.size()>=2&&(s[0]==L'\\'||s[0]==L'/')&&(s[1]==L'\\'||s[1]==L'/'))fail("network","不接受网络路径、UNC 或设备路径："+text(p));
    if(!(s.size()>=3&&drive_letter(s[0])&&s[1]==L':'&&(s[2]==L'\\'||s[2]==L'/')))fail("path","需要带盘符的本地绝对路径："+text(p));
    if(s.find(L':',2)!=std::wstring::npos)fail("path","路径不能含替代数据流："+text(p));
    if(!p.is_absolute()||p!=p.lexically_normal())fail("path","路径需要规范形式（不含 . 或 ..）："+text(p));
    for(const auto& part:p.relative_path()){const auto& n=part.native();
        if(!n.empty()&&(n.back()==L'.'||n.back()==L' '))fail("path","路径各级名称末尾不能是空格或句点："+text(p));}
    const std::wstring drive_root{s[0],L':',L'\\'};
    const unsigned type=drive?drive(drive_root):GetDriveTypeW(drive_root.c_str());
    if(type==DRIVE_REMOTE)fail("network","网络盘不受支持："+text(drive_root));
    if(type!=DRIVE_FIXED)fail("drive","只支持本地固定磁盘（盘类型 "+std::to_string(type)+"）："+text(drive_root));
    // Every component that exists must be an ordinary directory entry: a
    // junction, symbolic link or cloud placeholder anywhere on the way is refused.
    for(fs::path cur=p;;){
        const auto attributes=GetFileAttributesW(cur.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES){
            const auto error=GetLastError();
            if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)fail("access","无法读取路径属性（Win32 "+std::to_string(error)+"）："+text(cur));
            if(cur==p&&must_exist)fail("missing","路径不存在："+text(p));
        }else if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)fail("reparse","路径含重解析点（联接、符号链接或云占位）："+text(cur));
        const auto parent=cur.parent_path();if(parent==cur||parent.empty())break;cur=parent;
    }
}
}
