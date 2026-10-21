// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The panel hotkey: names, the allowed keys and why the others are refused, the
// settings document, the polled press rule the launcher and the root proxy use,
// the rebinding step the panel and the desktop program use, and the settings
// file (missing, malformed, valid, written, watched). No key is ever pressed:
// key state is a fake.
#include "lab_panel_hotkey_file.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
namespace fs=std::filesystem;
using namespace lab::hotkey;
unsigned checks=0;
void need(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
std::string text(const Binding& b){char out[kMaxNameBytes+1]{};return format(b,out,sizeof out)?out:"<no name>";}
bool parses(const char* s,Binding expected){Binding b;return parse(s,b)&&b==expected;}
bool rejects(const char* s){Binding b;return !parse(s,b);}
bool document(const std::string& s,Binding* out=nullptr){Binding b;const bool ok=parse_document(s.data(),s.size(),b);if(ok&&out)*out=b;return ok;}
// A fake keyboard for GetAsyncKeyState / GetKeyState.
struct Keys {
    std::map<int,bool> down;
    SHORT operator()(int vk)const{const auto it=down.find(vk);return it!=down.end()&&it->second?SHORT(-32768):SHORT(0);} // bit 15: down
};
fs::path temp_base(){
    std::wstring v(32768,L'\0');const auto n=GetEnvironmentVariableW(L"OVERGLAZE_TEST_TEMP",v.data(),DWORD(v.size()));
    return n&&n<v.size()?fs::path(v.substr(0,n)):fs::temp_directory_path();
}
void put(const fs::path& file,const std::string& bytes){fs::create_directories(file.parent_path());std::ofstream f(file,std::ios::binary|std::ios::trunc);f<<bytes;}
std::string slurp(const fs::path& file){std::ifstream f(file,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});}
template<class Ready> bool within(unsigned milliseconds,Ready ready){
    const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(std::chrono::steady_clock::now()<until){if(ready())return true;std::this_thread::sleep_for(std::chrono::milliseconds(20));}
    return ready();
}
}

int main(){
    try{
        // ---- names: every key with a name round-trips with every modifier set.
        unsigned named=0;
        for(unsigned vk=0;vk<256;++vk)for(unsigned m=0;m<8;++m){
            const Binding b{vk,(m&1)!=0,(m&2)!=0,(m&4)!=0};char name[kMaxNameBytes+1]{};
            if(!format(b,name,sizeof name)){need(name[0]=='\0',"no name, empty text");continue;}
            ++named;Binding back;need(parse(name,back)&&back==b,std::string("round trip of ")+name);
        }
        need(named==(24+26+10+10+5)*8,"F1-F24, A-Z, 0-9, Num0-Num9 and five named keys have names: "+std::to_string(named));
        need(text(kDefault)=="Insert"&&name(kDefault)=="Insert","the default is Insert");
        need(text({VK_F10,true,true,true})=="Ctrl+Shift+Alt+F10","modifiers in the order Ctrl, Shift, Alt");
        need(text({VK_NUMPAD5,true,false,false})=="Ctrl+Num5"&&text({VK_NEXT,false,false,false})=="PageDown"&&text({VK_SCROLL,false,false,false})=="ScrollLock","named keys");
        need(name({VK_CAPITAL,false,false,false})=="Insert","a key without a name shows the default's name");
        need(parses("ctrl + f10",{VK_F10,true,false,false})&&parses(" Alt+Shift+k ",{'K',false,true,true})&&parses("scrolllock",{VK_SCROLL,false,false,false}),"case and blanks around + are ignored; modifiers in any order");
        need(parses("F24",{VK_F24})&&parses("num0",{VK_NUMPAD0})&&parses("9",{'9'}),"F24, num0, a digit");
        for(const auto* bad:{"","+","Ctrl+","+F10","Ctrl++F10","Ctrl+Ctrl+F10","F10+Ctrl","Ctrl","F0","F25","F01","Num10","Esc","CapsLock","Win+F1","Ctrl+F10+F11","Control+F10",
                             "Insert Insert","AB","\xE4\xB8\xAD"})
            need(rejects(bad),std::string("refused name: ")+bad);
        {char tiny[4]{'x','x','x','x'};need(!format({VK_F10,true,false,false},tiny,sizeof tiny)&&tiny[0]=='\0',"a name that does not fit is not cut short");}

        // ---- allowed and refused keys
        const struct {Binding b;Refusal r;} table[]{
            {{VK_INSERT},Refusal::none},{{VK_F1},Refusal::none},{{VK_F24},Refusal::none},{{VK_F10,true},Refusal::none},{{VK_F9,false,true},Refusal::none},
            {{VK_F10,false,false,true},Refusal::none},{{VK_PRIOR},Refusal::none},{{VK_NEXT,true},Refusal::none},{{VK_PAUSE},Refusal::none},{{VK_SCROLL,false,true},Refusal::none},
            {{'K',false,false,true},Refusal::none},{{'B',true},Refusal::none},{{'1',true,true},Refusal::none},{{VK_NUMPAD5,true},Refusal::none},{{'Z',false,false,true},Refusal::none},
            {{VK_ESCAPE},Refusal::escape},{{VK_ESCAPE,true,true},Refusal::escape},{{VK_RETURN},Refusal::enter},{{VK_RETURN,false,false,true},Refusal::enter},
            {{VK_SPACE},Refusal::space},{{VK_SPACE,true},Refusal::space},{{VK_TAB},Refusal::tab},{{VK_TAB,false,false,true},Refusal::tab},
            {{'A'},Refusal::plain_character},{{'A',false,true},Refusal::plain_character},{{'7'},Refusal::plain_character},{{VK_NUMPAD1},Refusal::plain_character},
            {{'A',true},Refusal::text_shortcut},{{'C',true},Refusal::text_shortcut},{{'V',true},Refusal::text_shortcut},{{'X',true},Refusal::text_shortcut},
            {{'Y',true},Refusal::text_shortcut},{{'Z',true,true},Refusal::text_shortcut},
            {{'Q',true,false,true},Refusal::altgr},{{'2',true,false,true},Refusal::altgr},
            {{VK_F4,false,false,true},Refusal::alt_f4},{{VK_F4,true,false,true},Refusal::alt_f4},
            {{VK_LBUTTON},Refusal::mouse},{{VK_RBUTTON},Refusal::mouse},{{VK_MBUTTON},Refusal::mouse},{{VK_XBUTTON1},Refusal::mouse},{{VK_XBUTTON2},Refusal::mouse},
            {{VK_LWIN},Refusal::windows_key},{{VK_RWIN,true},Refusal::windows_key},
            {{VK_CONTROL,true},Refusal::modifier_only},{{VK_SHIFT},Refusal::modifier_only},{{VK_MENU},Refusal::modifier_only},{{VK_LCONTROL},Refusal::modifier_only},{{VK_RMENU},Refusal::modifier_only},
            {{VK_LEFT},Refusal::text_editing},{{VK_DOWN,true},Refusal::text_editing},{{VK_BACK},Refusal::text_editing},{{VK_DELETE,true},Refusal::text_editing},
            {{VK_HOME},Refusal::text_editing},{{VK_END,false,false,true},Refusal::text_editing},
            {{VK_PAUSE,true},Refusal::break_key},{{VK_SCROLL,true},Refusal::break_key},
            {{VK_CAPITAL},Refusal::unsupported},{{VK_NUMLOCK},Refusal::unsupported},{{VK_SNAPSHOT},Refusal::unsupported},{{VK_OEM_1,true},Refusal::unsupported},
            {{VK_OEM_PLUS,false,false,true},Refusal::unsupported},{{VK_APPS},Refusal::unsupported},{{VK_ADD,true},Refusal::unsupported},{{0},Refusal::unsupported}};
        for(const auto& row:table)need(check(row.b)==row.r,"check of vk "+std::to_string(row.b.vk)+" ctrl "+std::to_string(row.b.ctrl)+" shift "+std::to_string(row.b.shift)+" alt "+std::to_string(row.b.alt));
        for(unsigned r=unsigned(Refusal::modifier_only);r<=unsigned(Refusal::unsupported);++r)need(std::strlen(refusal_text(Refusal(r)))>0,"every refusal says why");
        need(std::strlen(refusal_text(Refusal::none))==0,"no refusal, no text");
        need(pack(Binding{VK_F10,true,false,true})==(VK_F10|0x100u|0x400u)&&unpack(pack(Binding{'K',false,true,true}))==Binding{'K',false,true,true},"pack / unpack");

        // ---- the settings document
        for(const auto& b:{kDefault,Binding{VK_F10,true},Binding{'K',false,true,true},Binding{VK_NUMPAD0,true}}){
            char doc[512]{};const auto n=write_document(b,doc,sizeof doc);need(n>0&&doc[n]=='\0',"written");
            Binding back;need(parse_document(doc,n,back)&&back==b,"document round trip of "+text(b));}
        {char doc[512]{};write_document({VK_F10,true},doc,sizeof doc);
         need(std::string(doc)=="{\n  \"kind\": \"overglaze-panel-hotkey\",\n  \"version\": 1,\n  \"key\": \"Ctrl+F10\"\n}\n","the documented format");
         char small[16]{};need(write_document({VK_F10,true},small,sizeof small)==0,"a document that does not fit is not written");
         need(write_document({VK_CAPITAL},doc,sizeof doc)==0,"a key without a name is never written");}
        Binding got;
        need(document(R"({"key":"Alt+K","version":1,"kind":"overglaze-panel-hotkey"})",&got)&&got==Binding{'K',false,false,true},"any member order, no blanks");
        need(document("\xEF\xBB\xBF{ \"kind\" : \"overglaze-panel-hotkey\" ,\r\n\t\"version\":1, \"key\":\"pageup\" }\r\n",&got)&&got==Binding{VK_PRIOR},"a UTF-8 BOM, CRLF and tabs");
        for(const auto* bad:{
                "",R"({})",R"({"kind":"overglaze-panel-hotkey","version":1})",R"({"kind":"overglaze-panel-hotkey","key":"F10"})",R"({"version":1,"key":"F10"})",
                R"({"kind":"overglaze-panel-hotkey","version":2,"key":"F10"})",R"({"kind":"overglaze-panel-hotkey","version":"1","key":"F10"})",
                R"({"kind":"overglaze-panel-hotkey","version":1.0,"key":"F10"})",R"({"kind":"overglaze-panel-hotkey","version":10,"key":"F10"})",
                R"({"kind":"overglaze-panel-hotkey","version":01,"key":"F10"})",R"({"kind":"overglaze-panel-hotkey","version":1,"key":"A"})",
                R"({"kind":"overglaze-panel-hotkey","version":1,"key":"Esc"})",R"({"kind":"overglaze-panel-hotkey","version":1,"key":"Alt+F4"})",
                R"({"kind":"overglaze-panel-hotkey","version":1,"key":"F10","extra":1})",R"({"kind":"overglaze-panel-hotkey","version":1,"key":"F10","key":"F9"})",
                R"({"kind":"overglaze-panel-hotkey","version":1,"key":"F10",})",R"({"kind":"overglaze-panel-hotkey","version":1,"key":"F10"} x)",
                R"({"kind":"overglaze-panel-hotkey","version":1,"key":"F\u0031"})",R"({"kind":"overglaze-overlay-preferences","version":1,"key":"F10"})",
                R"({"Kind":"overglaze-panel-hotkey","version":1,"key":"F10"})",R"([{"kind":"overglaze-panel-hotkey","version":1,"key":"F10"}])",
                R"({"kind":"overglaze-panel-hotkey","version":1,"key":45})",R"({"kind":"overglaze-panel-hotkey","version":1,"key":"F10")",
                R"({kind:"overglaze-panel-hotkey","version":1,"key":"F10"})","{broken"})
            need(!document(bad),std::string("malformed: ")+bad);
        {std::string big=R"({"kind":"overglaze-panel-hotkey","version":1,"key":"F10")";big+=std::string(kMaxFileBytes,' ')+"}";need(!document(big),"larger than 4 KiB");}

        // ---- a string member out of a larger JSON document (the proxy's output_root)
        {const std::string install=R"({"loader":{"subdir":"overglaze"},"note":"output_root","output_root":"G:\\Games\\\u91c9\u5149 \"x\"\/data","x":1})";
         char out[128]{};need(string_field(install.data(),install.size(),"output_root",out,sizeof out)&&std::string(out)=="G:\\Games\\\xE9\x87\x89\xE5\x85\x89 \"x\"/data","escapes and \\u decode to UTF-8; a value spelled like the field is skipped");
         const std::string pair=R"({"output_root":"\ud83d\ude00"})";need(string_field(pair.data(),pair.size(),"output_root",out,sizeof out)&&std::string(out)=="\xF0\x9F\x98\x80","a surrogate pair");
         for(const auto* bad:{R"({"output_root":1})",R"({"output_root":"\ud83d"})",R"({"output_root":"a\qb"})",R"({"output_root":"open)",R"({"other":"x"})"}){
             const std::string s=bad;need(!string_field(s.data(),s.size(),"output_root",out,sizeof out),std::string("no string field: ")+bad);}
         char tiny[4]{};need(!string_field(install.data(),install.size(),"output_root",tiny,sizeof tiny),"a value that does not fit");}

        // ---- the polled press (launcher, root proxy)
        {Keys k;PressWatch w;const Binding ctrl_f10{VK_F10,true};
         k.down[VK_F10]=k.down[VK_CONTROL]=true;need(!w.poll(ctrl_f10,k),"held when watching starts: not a press");
         k.down[VK_F10]=false;need(!w.poll(ctrl_f10,k),"released");
         k.down[VK_F10]=true;need(w.poll(ctrl_f10,k),"pressed with Ctrl held");need(!w.poll(ctrl_f10,k),"held: one press only");
         k.down[VK_F10]=false;w.poll(ctrl_f10,k);k.down[VK_CONTROL]=false;k.down[VK_F10]=true;need(!w.poll(ctrl_f10,k),"F10 without Ctrl is not the binding");
         k.down[VK_F10]=false;w.poll(ctrl_f10,k);k.down[VK_CONTROL]=k.down[VK_SHIFT]=true;k.down[VK_F10]=true;need(!w.poll(ctrl_f10,k),"an extra Shift is not the binding");
         k.down[VK_F10]=false;w.poll(ctrl_f10,k);k.down[VK_SHIFT]=false;k.down[VK_LWIN]=true;k.down[VK_F10]=true;need(!w.poll(ctrl_f10,k),"nothing while a Windows key is held");
         Keys plain;PressWatch insert;plain.down[VK_INSERT]=false;insert.poll(kDefault,plain);plain.down[VK_INSERT]=true;need(insert.poll(kDefault,plain),"Insert alone");
         plain.down[VK_INSERT]=false;insert.poll(kDefault,plain);plain.down[VK_CONTROL]=plain.down[VK_INSERT]=true;need(!insert.poll(kDefault,plain),"Ctrl+Insert is not Insert");
         need(modifiers_match(Binding{VK_F9,false,true},Keys{{{VK_SHIFT,true}}})&&!modifiers_match(Binding{VK_F9,false,true},Keys{}),"modifiers_match");}

        // ---- the rebinding step (panel, desktop program)
        {Binding chosen;Refusal why;Keys none;
         need(capture_step(VK_CONTROL,Keys{{{VK_CONTROL,true}}},chosen,why)==CaptureStep::wait&&capture_step(VK_LSHIFT,none,chosen,why)==CaptureStep::wait,"a modifier waits for its key");
         need(capture_step(VK_ESCAPE,none,chosen,why)==CaptureStep::cancel,"Esc alone cancels");
         need(capture_step(VK_ESCAPE,Keys{{{VK_SHIFT,true}}},chosen,why)==CaptureStep::refused&&why==Refusal::escape,"Shift+Esc is refused, not a cancel");
         need(capture_step('A',none,chosen,why)==CaptureStep::refused&&why==Refusal::plain_character,"a letter alone is refused");
         need(capture_step(VK_F10,Keys{{{VK_CONTROL,true}}},chosen,why)==CaptureStep::chosen&&chosen==Binding{VK_F10,true}&&why==Refusal::none,"Ctrl+F10 is chosen");
         need(capture_step(VK_F10,Keys{{{VK_LWIN,true}}},chosen,why)==CaptureStep::refused&&why==Refusal::windows_key,"with a Windows key held");
         need(capture_step(0x1FF,none,chosen,why)==CaptureStep::refused&&why==Refusal::unsupported,"out of the virtual-key range");}

        // ---- the settings file
        const auto root=temp_base()/("overglaze-panel-hotkey-"+std::to_string(GetCurrentProcessId()));
        std::error_code ignored;fs::remove_all(root,ignored);fs::create_directories(root);
        const auto file=settings_file(root);
        need(file==root/L"settings"/L"panel-hotkey-v1.json","<data>\\settings\\panel-hotkey-v1.json");
        {const auto r=read(root);need(r.binding==kDefault&&r.source==Source::defaulted&&r.note.empty()&&!r.transient,"missing: Insert");}
        write(root,{VK_F10,true});
        {const auto r=read(root);need(r.binding==Binding{VK_F10,true}&&r.source==Source::file,"written, read back");}
        need(slurp(file).find("\"key\": \"Ctrl+F10\"")!=std::string::npos,"the file names the key");
        for(const auto& entry:fs::directory_iterator(root/L"settings"))need(entry.path().filename()==L"panel-hotkey-v1.json","no temporary file left behind");
        {bool refused=false;try{write(root,{'A'});}catch(const std::exception& e){refused=std::string(e.what())==refusal_text(Refusal::plain_character);}
         need(refused&&read(root).binding==Binding{VK_F10,true},"a refused binding is never written; the old file stays");}
        put(file,"{broken");
        {const auto r=read(root);need(r.binding==kDefault&&r.source==Source::malformed&&!r.note.empty()&&!r.transient,"malformed: Insert, with a note");}
        put(file,R"({"kind":"overglaze-panel-hotkey","version":1,"key":"Ctrl+V"})");
        need(read(root).source==Source::malformed,"a refused binding in the file is malformed");
        put(file,std::string(kMaxFileBytes+1,' '));need(read(root).source==Source::malformed,"too large");
        fs::remove(file);
        // An unreadable file (held without sharing) is transient, not malformed.
        {write(root,{VK_F9,false,true});const HANDLE held=CreateFileW(file.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);need(held!=INVALID_HANDLE_VALUE,"hold the file");
         const auto r=read(root);CloseHandle(held);need(r.transient&&r.binding==kDefault&&!r.note.empty(),"held by another program: transient, with the reason");}
        fs::remove(file);

        // ---- the live setting
        {Settings memory;need(memory.current()==kDefault&&memory.source()==Source::memory,"no data root: Insert, this process only");
         need(memory.set({'A'})==Refusal::plain_character&&memory.current()==kDefault&&memory.revision()==0,"a refused set changes nothing");
         need(memory.set({VK_F10,true})==Refusal::none&&memory.current()==Binding{VK_F10,true}&&memory.revision()==1&&memory.current_name()=="Ctrl+F10","an allowed set applies at once");
         memory.close();memory.close();need(!fs::exists(file),"a memory setting writes nothing");}
        {Settings live(root);
         need(live.current()==kDefault&&live.source()==Source::defaulted&&live.error().empty(),"no file yet: Insert");
         need(live.set({VK_F8,true,true})==Refusal::none&&live.current()==Binding{VK_F8,true,true},"set applies at once");
         need(within(3000,[&]{return read(root).binding==Binding{VK_F8,true,true};}),"and the worker writes the file");
         const auto before=live.revision();
         write(root,{'K',false,false,true}); // the desktop program, or a game's panel, changes it
         need(within(3000,[&]{return live.current()==Binding{'K',false,false,true};})&&live.revision()>before&&live.source()==Source::file,"a changed file is picked up");
         put(file,"not json");
         need(within(3000,[&]{return live.current()==kDefault&&live.source()==Source::malformed;})&&!live.error().empty(),"a malformed file means Insert, and says so");
         write(root,{VK_PRIOR});
         need(within(3000,[&]{return live.current()==Binding{VK_PRIOR}&&live.error().empty();}),"a valid file again");
         fs::remove(file);
         need(within(3000,[&]{return live.current()==kDefault&&live.source()==Source::defaulted;}),"a removed file means Insert");
         need(live.set({VK_F7})==Refusal::none,"set again");live.close();
         need(read(root).binding==Binding{VK_F7},"close() finishes the pending write");}
        {Settings again(root);need(again.current()==Binding{VK_F7}&&again.source()==Source::file,"a new process reads the saved binding");}
        fs::remove_all(root,ignored);
        std::cout<<"PASS "<<checks<<" panel hotkey checks\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}
}
