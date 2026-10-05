// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Out-of-game root resolution (lab_root_locator.hpp):
// the published layout, a track build directory, the override file, and every
// refusal -- reparse points, network paths, non-fixed disks, malformed paths and
// overrides. All fixtures live in OVERGLAZE_TEST_TEMP or the user's temp
// directory; the real Lab tree is only resolved (not written) at the very end.
#include "lab_root_locator.hpp"
#include "lab_platform.hpp"
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
using lab::root::Source;
namespace {
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
// The refusal must happen AND name the expected kind ("[root:<kind>]").
template<class F>void refused(F f,const char* kind,const std::string& why){
    std::string got="(accepted)";
    try{f();}catch(const std::exception& e){got=e.what();}
    const bool ok=got.find(std::string("[root:")+kind+"]")!=std::string::npos;
    if(!ok)std::cerr<<why<<": expected [root:"<<kind<<"], got "<<got<<'\n';
    need(ok,why);}
void put(const fs::path& p,const std::string& s){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary|std::ios::trunc);f<<s;if(!f)throw std::runtime_error("fixture write");}
void exe(const fs::path& p){put(p,"synthetic program; never executed");}
std::string override_json(const lab::json& root){return lab::json{{"schema",lab::root::kOverrideSchema},{"lab_root",root}}.dump(2);}
std::string override_json(const fs::path& root){return override_json(lab::json(lab::utf8(root.wstring())));}
fs::path temp_base(){std::wstring v(32768,L'\0');auto n=GetEnvironmentVariableW(L"OVERGLAZE_TEST_TEMP",v.data(),DWORD(v.size()));
    if(!n||n>=v.size()){n=GetTempPathW(DWORD(v.size()),v.data());need(n&&n<v.size(),"temp path");}
    v.resize(n);return fs::canonical(fs::path(v));}
// A directory junction needs no privilege; mklink is part of every Windows.
void junction(const fs::path& link,const fs::path& target){
    std::wstring command=L"cmd.exe /d /c mklink /J \""+link.wstring()+L"\" \""+target.wstring()+L"\" >nul";
    STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
    need(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)!=FALSE,"start mklink");
    lab::Handle thread(pi.hThread),process(pi.hProcess);need(WaitForSingleObject(process.value,20000)==WAIT_OBJECT_0,"mklink finished");
    DWORD code=1;GetExitCodeProcess(process.value,&code);
    const auto attributes=GetFileAttributesW(link.c_str());
    need(code==0&&attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"junction fixture created");}
}
int wmain(){fs::path root,link;try{
    const auto base=temp_base();lab::root::require_local_fixed(base);
    root=base/lab::wide("overglaze-root-test-"+lab::uuid());need(fs::create_directory(root),"new unique fixture root");
    // ---- the published layout: <root>\app\<program>.exe
    const auto a=root/L"A",a_exe=a/L"app"/L"tool.exe";exe(a_exe);
    {const auto r=lab::root::resolve_from(a_exe);
     need(r.root==a&&r.data==a/L"data"&&r.source==Source::app_layout&&std::string(r.source_name())=="app-layout"&&r.override_file==a/L"app"/lab::root::kOverrideFile,"app layout");}
    {const auto upper=root/L"A2"/L"APP"/L"Tool.exe";exe(upper);need(lab::root::resolve_from(upper).root==root/L"A2","app layout, any case");}
    // ---- a track build directory: <root>\data\_build*\<program>.exe
    const auto b=root/L"B";
    for(const auto* dir:{L"_build_controller",L"_build_research",L"_BUILD",L"_build"}){const auto p=b/L"data"/dir/L"tool.exe";exe(p);
        const auto r=lab::root::resolve_from(p);need(r.root==b&&r.data==b/L"data"&&r.source==Source::build_layout&&std::string(r.source_name())=="build-layout","build layout");}
    // ---- no recognisable layout: refused, never guessed
    const auto c_exe=root/L"C"/L"bin"/L"tool.exe";exe(c_exe);
    refused([&]{lab::root::resolve_from(c_exe);},"layout","a program outside app\\ and data\\_build* is refused");
    {const auto p=root/L"C"/L"notdata"/L"_build_x"/L"tool.exe";exe(p);refused([&]{lab::root::resolve_from(p);},"layout","a _build directory outside data\\ is not a build layout");}
    {const auto p=root/L"C"/L"data"/L"build"/L"tool.exe";exe(p);refused([&]{lab::root::resolve_from(p);},"layout","only _build* under data\\ is a build layout");}
    refused([&]{lab::root::resolve_from(root/L"A"/L"app"/L"missing.exe");},"missing","a program that does not exist");
    // ---- the override file beside the program wins over any layout
    const auto e=root/L"E";fs::create_directories(e);
    const auto a_override=a/L"app"/lab::root::kOverrideFile,c_override=root/L"C"/L"bin"/lab::root::kOverrideFile;
    put(a_override,override_json(e));
    {const auto r=lab::root::resolve_from(a_exe);need(r.root==e&&r.data==e/L"data"&&r.source==Source::override_file&&std::string(r.source_name())=="override-file"&&r.override_file==a_override,"override beats the app layout");}
    put(c_override,override_json(e));
    {const auto r=lab::root::resolve_from(c_exe);need(r.root==e&&r.source==Source::override_file,"override gives a root where no layout applies");}
    put(c_override,lab::json{{"schema",lab::root::kOverrideSchema},{"lab_root",lab::utf8(e.wstring())},{"note","override for this test"}}.dump());
    need(lab::root::resolve_from(c_exe).root==e,"a note is allowed");
    fs::remove(a_override);need(lab::root::resolve_from(a_exe).source==Source::app_layout,"without the file, the layout again");
    // A present but broken override is an error, never a silent fallback to the layout.
    const auto broken=[&](const std::string& text,const char* kind,const char* why){put(a_override,text);refused([&]{lab::root::resolve_from(a_exe);},kind,why);};
    broken("{not json","override","malformed JSON");
    broken(lab::json{{"schema","something-else"},{"lab_root",lab::utf8(e.wstring())}}.dump(),"override","wrong schema");
    broken(lab::json{{"schema",lab::root::kOverrideSchema}}.dump(),"override","no lab_root");
    broken(lab::json{{"schema",lab::root::kOverrideSchema},{"lab_root",7}}.dump(),"override","lab_root not a string");
    broken(lab::json{{"schema",lab::root::kOverrideSchema},{"lab_root",lab::utf8(e.wstring())},{"data_root","D:\\elsewhere"}}.dump(),"override","unknown field");
    broken(override_json(lab::json("relative\\lab")),"path","relative lab_root");
    broken(override_json(e/L".."/L"E"),"path","non-normal lab_root");
    broken(override_json(root/L"does-not-exist"),"missing","lab_root that does not exist");
    broken(override_json(root.root_path()),"drive-root","a whole drive as the Lab root");
    broken(override_json(lab::json("\\\\server\\share\\Lab")),"network","UNC lab_root");
    broken(std::string(17*1024,' ')+override_json(e),"override","oversize override");
    fs::remove(a_override);fs::create_directory(a_override);refused([&]{lab::root::resolve_from(a_exe);},"override","an override that is a directory");fs::remove(a_override);
    need(lab::root::resolve_from(a_exe).root==a,"fixture restored");
    // ---- reparse points: a junction anywhere on the way is refused
    link=root/L"J";junction(link,a);
    refused([&]{lab::root::resolve_from(link/L"app"/L"tool.exe");},"reparse","a program reached through a junction");
    put(c_override,override_json(link));refused([&]{lab::root::resolve_from(c_exe);},"reparse","an override root reached through a junction");
    refused([&]{lab::root::require_local_fixed(link/L"app",false);},"reparse","the guard alone, existence not required");
    // ---- network paths, decided on the spelling before any I/O
    refused([&]{lab::root::resolve_from(L"\\\\server\\share\\Lab\\app\\tool.exe");},"network","UNC program path");
    refused([&]{lab::root::require_local_fixed(L"//server/share/Lab");},"network","forward-slash UNC");
    refused([&]{lab::root::require_local_fixed(L"\\\\?\\C:\\Lab");},"network","\\\\?\\ device spelling");
    refused([&]{lab::root::require_local_fixed(L"\\\\.\\C:\\Lab");},"network","\\\\.\\ device spelling");
    // ---- non-fixed disks (and a mapped network drive), by drive type
    for(const unsigned type:{unsigned(DRIVE_REMOTE)})refused([&]{lab::root::resolve_from(a_exe,[type](const std::wstring&){return type;});},"network","a mapped network drive");
    for(const unsigned type:{unsigned(DRIVE_REMOVABLE),unsigned(DRIVE_CDROM),unsigned(DRIVE_RAMDISK),unsigned(DRIVE_UNKNOWN),unsigned(DRIVE_NO_ROOT_DIR)})
        refused([&]{lab::root::resolve_from(a_exe,[type](const std::wstring&){return type;});},"drive","a non-fixed disk (type "+std::to_string(type)+")");
    {std::wstring asked;const auto r=lab::root::resolve_from(a_exe,[&](const std::wstring& d){asked=d;return unsigned(DRIVE_FIXED);});
     need(r.root==a&&asked.size()==3&&asked[1]==L':'&&asked[2]==L'\\',"a fixed disk passes; the drive root is what is asked");}
    need(GetDriveTypeW(a.root_path().c_str())==DRIVE_FIXED&&lab::root::resolve_from(a_exe).root==a,"the fixture's real disk is fixed and accepted");
    // ---- path shape
    refused([&]{lab::root::require_local_fixed(L"relative\\Lab",false);},"path","relative");
    refused([&]{lab::root::require_local_fixed(a/L"app"/L".."/L"app",false);},"path","..");
    refused([&]{lab::root::require_local_fixed(fs::path(a.wstring()+L"\\app\\tool.exe:stream"),false);},"path","alternate data stream");
    refused([&]{lab::root::require_local_fixed(fs::path(a.wstring()+L"\\app."),false);},"path","trailing dot");
    refused([&]{lab::root::require_local_fixed(L"",false);},"path","empty");
    // ---- the running program: this test sits in a track build directory
    {const auto self=lab::root::self_executable();const auto r=lab::root::resolve_self();
     need(r.source==Source::override_file||(r.source==Source::build_layout&&r.data==self.parent_path().parent_path()),"resolve_self from the build directory");
     need(fs::is_directory(r.root)&&fs::is_directory(r.data),"the real Lab root and data exist");}
    // Remove the junction itself first (never what it points to), then the fixture.
    need(RemoveDirectoryW(link.c_str())!=FALSE&&fs::is_regular_file(a_exe),"junction removed, its target intact");link.clear();
    need(root.parent_path()==base&&root.filename().wstring().starts_with(L"overglaze-root-test-"),"cleanup confinement");
    const auto removed=fs::remove_all(root);
    std::cout<<lab::json{{"passed",true},{"checks",checks},{"fixture_files_removed",removed},{"lab_tree_written",false}}.dump(2)<<'\n';return 0;
}catch(const std::exception& e){if(!link.empty())RemoveDirectoryW(link.c_str());std::cerr<<e.what()<<"\nfixture="<<lab::utf8(root.wstring())<<'\n';return 1;}}
