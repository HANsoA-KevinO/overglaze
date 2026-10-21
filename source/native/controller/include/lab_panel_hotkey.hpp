// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The in-game panel's hotkey: one global setting for every game, the launcher
// and the root proxy, stored as <data>\settings\panel-hotkey-v1.json:
//
//   {
//     "kind": "overglaze-panel-hotkey",
//     "version": 1,
//     "key": "Ctrl+F10"
//   }
//
// "key" is one key with optional Ctrl / Shift / Alt, spelled as format() spells
// it (the parser ignores case and blanks around '+'). A missing file, or one
// that is malformed or names a refused binding, means the default, Insert.
//
// Everything in this header is allocation-free, exception-free and touches no
// file, because the root proxy (dxgi.dll in a game's directory, static CRT,
// kernel32 only) parses the same document with the same rules as the host, the
// launcher and the desktop program. File I/O lives in panel_hotkey.cpp, which
// the proxy never links.
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace lab::hotkey {
struct Binding {
    unsigned vk=VK_INSERT;bool ctrl=false,shift=false,alt=false;
    constexpr bool operator==(const Binding&) const=default;
};
inline constexpr Binding kDefault{};
inline constexpr const char* kKind="overglaze-panel-hotkey";
inline constexpr const wchar_t* kFileName=L"panel-hotkey-v1.json"; // under <data>\settings
inline constexpr std::size_t kMaxFileBytes=4096;
inline constexpr std::size_t kMaxNameBytes=48;

// Packed for atomics: vk in the low byte, then Ctrl, Shift, Alt.
constexpr std::uint32_t pack(const Binding& b) noexcept {return (b.vk&0xFFu)|(b.ctrl?0x100u:0u)|(b.shift?0x200u:0u)|(b.alt?0x400u:0u);}
constexpr Binding unpack(std::uint32_t v) noexcept {return {v&0xFFu,(v&0x100u)!=0,(v&0x200u)!=0,(v&0x400u)!=0};}

// Why a binding cannot be the panel hotkey. The panel owns the keyboard while
// it is open, so a key it needs for its own controls (navigation, typing a
// value) would close it instead; a key Windows or the game needs is not ours.
enum class Refusal : unsigned {
    none,
    modifier_only,   // Ctrl, Shift or Alt alone
    mouse,           // a mouse button
    windows_key,     // the Windows key, alone or held
    escape,          // closes the panel, cancels a rebinding
    enter,           // activates the focused control
    space,           // activates the focused control
    tab,             // moves between controls; Alt+Tab is Windows'
    text_editing,    // arrows, Backspace, Delete, Home, End: editing a value
    plain_character, // a letter or digit without Ctrl or Alt types text
    text_shortcut,   // Ctrl+A / C / V / X / Y / Z edit a value
    altgr,           // Ctrl+Alt+letter or digit types characters on many layouts
    alt_f4,          // closes the game
    break_key,       // Ctrl+Pause / Ctrl+Scroll Lock arrive as Break
    unsupported,     // every other key
};

constexpr bool is_modifier(unsigned vk) noexcept {return vk==VK_SHIFT||vk==VK_CONTROL||vk==VK_MENU||(vk>=VK_LSHIFT&&vk<=VK_RMENU);}
constexpr bool is_mouse(unsigned vk) noexcept {return vk==VK_LBUTTON||vk==VK_RBUTTON||vk==VK_MBUTTON||vk==VK_XBUTTON1||vk==VK_XBUTTON2;}
constexpr bool is_letter(unsigned vk) noexcept {return vk>='A'&&vk<='Z';}
constexpr bool is_digit(unsigned vk) noexcept {return vk>='0'&&vk<='9';}
constexpr bool is_numpad_digit(unsigned vk) noexcept {return vk>=VK_NUMPAD0&&vk<=VK_NUMPAD9;}
constexpr bool is_function(unsigned vk) noexcept {return vk>=VK_F1&&vk<=VK_F24;}

constexpr Refusal check(const Binding& b) noexcept {
    const unsigned k=b.vk;
    if(is_modifier(k))return Refusal::modifier_only;
    if(is_mouse(k))return Refusal::mouse;
    if(k==VK_LWIN||k==VK_RWIN)return Refusal::windows_key;
    if(k==VK_ESCAPE)return Refusal::escape;
    if(k==VK_RETURN)return Refusal::enter;
    if(k==VK_SPACE)return Refusal::space;
    if(k==VK_TAB)return Refusal::tab;
    if(k==VK_BACK||k==VK_DELETE||k==VK_HOME||k==VK_END||(k>=VK_LEFT&&k<=VK_DOWN))return Refusal::text_editing;
    if(is_letter(k)||is_digit(k)||is_numpad_digit(k)){
        if(!b.ctrl&&!b.alt)return Refusal::plain_character;
        if(b.ctrl&&b.alt)return Refusal::altgr;
        if(b.ctrl&&(k=='A'||k=='C'||k=='V'||k=='X'||k=='Y'||k=='Z'))return Refusal::text_shortcut;
        return Refusal::none;
    }
    if(is_function(k))return k==VK_F4&&b.alt?Refusal::alt_f4:Refusal::none;
    if(k==VK_PAUSE||k==VK_SCROLL)return b.ctrl?Refusal::break_key:Refusal::none;
    if(k==VK_INSERT||k==VK_PRIOR||k==VK_NEXT)return Refusal::none;
    return Refusal::unsupported;
}
constexpr bool allowed(const Binding& b) noexcept {return check(b)==Refusal::none;}

// Shown under the capture control (UTF-8).
constexpr const char* refusal_text(Refusal r) noexcept {
    switch(r){
    case Refusal::none:return "";
    case Refusal::modifier_only:return "Ctrl、Shift、Alt 要和另一个键一起按。";
    case Refusal::mouse:return "鼠标按键不能用作面板快捷键。";
    case Refusal::windows_key:return "Windows 键由系统使用，不能用作面板快捷键。";
    case Refusal::escape:return "Esc 用于收起面板和取消修改，不能用作面板快捷键。";
    case Refusal::enter:return "Enter 用于确认面板上的控件，不能用作面板快捷键。";
    case Refusal::space:return "空格用于确认面板上的控件，不能用作面板快捷键。";
    case Refusal::tab:return "Tab 用于在面板上切换控件（Alt+Tab 属于系统），不能用作面板快捷键。";
    case Refusal::text_editing:return "方向键、Backspace、Delete、Home、End 用于在面板里输入数值，不能用作面板快捷键。";
    case Refusal::plain_character:return "字母和数字要搭配 Ctrl 或 Alt，否则在面板里输入数值时会误触。";
    case Refusal::text_shortcut:return "Ctrl+A / C / V / X / Y / Z 是面板输入框的编辑快捷键，不能用作面板快捷键。";
    case Refusal::altgr:return "Ctrl+Alt 加字母或数字在部分键盘布局上会输入字符，不能用作面板快捷键。";
    case Refusal::alt_f4:return "Alt+F4 用于关闭游戏，不能用作面板快捷键。";
    case Refusal::break_key:return "Ctrl+Pause、Ctrl+Scroll Lock 会变成 Break 键，不能用作面板快捷键。";
    case Refusal::unsupported:return "这个键不能用作面板快捷键。可用 F1–F24、Insert、Pause、Scroll Lock、Page Up、Page Down，或 Ctrl / Alt 加字母、数字。";
    }
    return "";
}

namespace detail {
constexpr char lower(char c) noexcept {return c>='A'&&c<='Z'?char(c-'A'+'a'):c;}
constexpr bool iequal(const char* a,std::size_t n,const char* b) noexcept {
    std::size_t i=0;for(;i<n&&b[i];++i)if(lower(a[i])!=lower(b[i]))return false;
    return i==n&&!b[i];
}
constexpr bool equal(const char* a,std::size_t n,const char* b) noexcept {
    std::size_t i=0;for(;i<n&&b[i];++i)if(a[i]!=b[i])return false;
    return i==n&&!b[i];
}
struct Writer {
    char* out;std::size_t cap,size=0;bool ok=true;
    constexpr void put(char c) noexcept {if(size+1<cap)out[size++]=c;else ok=false;}
    constexpr void put(const char* s) noexcept {while(*s)put(*s++);}
    constexpr std::size_t finish() noexcept {if(cap)out[ok?size:0]='\0';return ok?size:0;}
};
struct Named {unsigned vk;const char* name;};
inline constexpr Named kNamed[]{{VK_INSERT,"Insert"},{VK_PAUSE,"Pause"},{VK_SCROLL,"ScrollLock"},{VK_PRIOR,"PageUp"},{VK_NEXT,"PageDown"}};
// One key token: F1..F24, A..Z, 0..9, Num0..Num9 or a named key.
constexpr bool parse_key(const char* s,std::size_t n,unsigned& vk) noexcept {
    if(n==1){const char c=s[0];
        if(c>='a'&&c<='z'){vk=unsigned(c-'a'+'A');return true;}
        if(c>='A'&&c<='Z'){vk=unsigned(c);return true;}
        if(c>='0'&&c<='9'){vk=unsigned(c);return true;}
        return false;}
    if((s[0]=='F'||s[0]=='f')&&(n==2||n==3)&&s[1]>='1'&&s[1]<='9'){
        unsigned v=unsigned(s[1]-'0');
        if(n==3){if(s[2]<'0'||s[2]>'9')return false;v=v*10+unsigned(s[2]-'0');}
        if(v<1||v>24)return false;vk=VK_F1+v-1;return true;}
    if(n==4&&iequal(s,3,"num")&&s[3]>='0'&&s[3]<='9'){vk=VK_NUMPAD0+unsigned(s[3]-'0');return true;}
    for(const auto& k:kNamed)if(iequal(s,n,k.name)){vk=k.vk;return true;}
    return false;
}
constexpr bool blank(char c) noexcept {return c==' '||c=='\t';}
}

// The key's own name, as format() spells it; false for a key without one. Only
// F1-F24, A-Z, 0-9, Num0-Num9, Insert, Pause, ScrollLock, PageUp and PageDown
// have names: every other key is refused by check() anyway.
constexpr bool key_name(unsigned vk,char* out,std::size_t cap) noexcept {
    detail::Writer w{out,cap};
    if(is_function(vk)){const unsigned n=vk-VK_F1+1;w.put('F');if(n>=10)w.put(char('0'+n/10));w.put(char('0'+n%10));}
    else if(is_letter(vk)||is_digit(vk))w.put(char(vk));
    else if(is_numpad_digit(vk)){w.put("Num");w.put(char('0'+(vk-VK_NUMPAD0)));}
    else{const char* name=nullptr;for(const auto& k:detail::kNamed)if(k.vk==vk)name=k.name;
        if(!name){(void)w.finish();return false;}w.put(name);}
    return w.finish()!=0;
}
// "Ctrl+Shift+Alt+Key" in that order; false (and an empty out) when the key has no name.
constexpr bool format(const Binding& b,char* out,std::size_t cap) noexcept {
    char key[16]{};if(!key_name(b.vk,key,sizeof key)){if(cap)out[0]='\0';return false;}
    detail::Writer w{out,cap};
    if(b.ctrl)w.put("Ctrl+");if(b.shift)w.put("Shift+");if(b.alt)w.put("Alt+");w.put(key);
    return w.finish()!=0;
}
// The inverse of format(): modifiers first, each once, then exactly one key.
// Case and blanks around '+' are ignored. Refusals are NOT checked here.
constexpr bool parse(const char* s,std::size_t n,Binding& out) noexcept {
    if(!s||n==0||n>kMaxNameBytes)return false;
    Binding b{0,false,false,false};bool have_key=false;std::size_t start=0;
    for(std::size_t i=0;i<=n;++i){
        if(i<n&&s[i]!='+')continue;
        std::size_t a=start,z=i;while(a<z&&detail::blank(s[a]))++a;while(z>a&&detail::blank(s[z-1]))--z;
        if(a==z||have_key)return false; // an empty token, or anything after the key
        const char* t=s+a;const std::size_t len=z-a;
        if(i<n){ // a modifier: something follows it
            bool* flag=detail::iequal(t,len,"ctrl")?&b.ctrl:detail::iequal(t,len,"shift")?&b.shift:detail::iequal(t,len,"alt")?&b.alt:nullptr;
            if(!flag||*flag)return false;*flag=true;
        }else{if(!detail::parse_key(t,len,b.vk))return false;have_key=true;}
        start=i+1;
    }
    if(!have_key)return false;
    out=b;return true;
}
constexpr bool parse(const char* s,Binding& out) noexcept {std::size_t n=0;if(s)while(s[n]&&n<=kMaxNameBytes)++n;return parse(s,n,out);}

// The document format() describes at the top, with a trailing newline.
// Returns the byte count, 0 when it does not fit or the key has no name.
constexpr std::size_t write_document(const Binding& b,char* out,std::size_t cap) noexcept {
    char name[kMaxNameBytes+1]{};if(!format(b,name,sizeof name)){if(cap)out[0]='\0';return 0;}
    detail::Writer w{out,cap};
    w.put("{\n  \"kind\": \"");w.put(kKind);w.put("\",\n  \"version\": 1,\n  \"key\": \"");w.put(name);w.put("\"\n}\n");
    return w.finish();
}

namespace detail {
struct Scan {
    const char* p;const char* end;
    constexpr void ws() noexcept {while(p<end&&(*p==' '||*p=='\t'||*p=='\r'||*p=='\n'))++p;}
    constexpr bool take(char c) noexcept {ws();if(p<end&&*p==c){++p;return true;}return false;}
    // A string without escapes or control characters (all three values are plain ASCII).
    constexpr bool plain_string(const char*& s,std::size_t& n) noexcept {
        if(!take('"'))return false;s=p;
        while(p<end&&*p!='"'){const unsigned char c=static_cast<unsigned char>(*p);if(c<0x20||c=='\\'||c>=0x80)return false;++p;}
        if(p>=end)return false;n=std::size_t(p-s);++p;return true;
    }
};
}
// The settings document, strictly: one object with exactly "kind", "version"
// (the integer 1) and "key" in any order, an optional UTF-8 BOM, nothing after
// it, and a binding check() allows. Anything else is malformed: false.
constexpr bool parse_document(const char* text,std::size_t n,Binding& out) noexcept {
    if(!text||n>kMaxFileBytes)return false;
    detail::Scan s{text,text+n};
    if(n>=3&&static_cast<unsigned char>(text[0])==0xEF&&static_cast<unsigned char>(text[1])==0xBB&&static_cast<unsigned char>(text[2])==0xBF)s.p+=3;
    if(!s.take('{'))return false;
    bool kind=false,version=false,key=false;Binding b{};
    for(bool first=true;;first=false){
        if(!first&&!s.take(','))break;
        const char* name=nullptr;std::size_t length=0;if(!s.plain_string(name,length)||!s.take(':'))return false;
        if(detail::equal(name,length,"kind")){
            const char* v=nullptr;std::size_t m=0;if(kind||!s.plain_string(v,m)||!detail::equal(v,m,kKind))return false;kind=true;
        }else if(detail::equal(name,length,"version")){
            if(version)return false;s.ws();if(s.p>=s.end||*s.p!='1')return false;++s.p;
            if(s.p<s.end&&((*s.p>='0'&&*s.p<='9')||*s.p=='.'||*s.p=='e'||*s.p=='E'))return false;version=true;
        }else if(detail::equal(name,length,"key")){
            const char* v=nullptr;std::size_t m=0;if(key||!s.plain_string(v,m)||!parse(v,m,b))return false;key=true;
        }else return false;
    }
    if(!s.take('}'))return false;s.ws();
    if(s.p!=s.end||!kind||!version||!key||!allowed(b))return false;
    out=b;return true;
}

// The value of the first member called `field` whose value is a string, decoded
// from JSON escapes into UTF-8 (\uXXXX and surrogate pairs included). For the
// root proxy, which needs only "output_root" from the installation it sits
// beside; the host validates that installation in full once it is loaded.
constexpr bool string_field(const char* text,std::size_t n,const char* field,char* out,std::size_t cap) noexcept {
    std::size_t f=0;while(field[f])++f;
    for(std::size_t i=0;i+f+2<=n;++i){
        if(text[i]!='"')continue;
        std::size_t k=0;while(k<f&&text[i+1+k]==field[k])++k;
        if(k!=f||text[i+1+f]!='"')continue;
        detail::Scan s{text+i+f+2,text+n};
        if(!s.take(':')||!s.take('"'))continue;
        detail::Writer w{out,cap};
        auto hex=[&](unsigned& v)->bool{v=0;for(int d=0;d<4;++d){if(s.p>=s.end)return false;const char c=*s.p++;
            v<<=4;if(c>='0'&&c<='9')v|=unsigned(c-'0');else if(c>='a'&&c<='f')v|=unsigned(c-'a'+10);else if(c>='A'&&c<='F')v|=unsigned(c-'A'+10);else return false;}return true;};
        while(s.p<s.end&&*s.p!='"'){
            char c=*s.p++;
            if(static_cast<unsigned char>(c)<0x20)return false;
            if(c!='\\'){w.put(c);continue;}
            if(s.p>=s.end)return false;c=*s.p++;
            switch(c){
            case '"':case '\\':case '/':w.put(c);break;
            case 'b':w.put('\b');break;case 'f':w.put('\f');break;case 'n':w.put('\n');break;case 'r':w.put('\r');break;case 't':w.put('\t');break;
            case 'u':{unsigned u=0;if(!hex(u))return false;
                if(u>=0xD800&&u<=0xDBFF){unsigned low=0;if(s.p+1>=s.end||s.p[0]!='\\'||s.p[1]!='u')return false;s.p+=2;if(!hex(low)||low<0xDC00||low>0xDFFF)return false;u=0x10000+((u-0xD800)<<10)+(low-0xDC00);}
                else if(u>=0xDC00&&u<=0xDFFF)return false;
                if(u<0x80)w.put(char(u));
                else if(u<0x800){w.put(char(0xC0|(u>>6)));w.put(char(0x80|(u&0x3F)));}
                else if(u<0x10000){w.put(char(0xE0|(u>>12)));w.put(char(0x80|((u>>6)&0x3F)));w.put(char(0x80|(u&0x3F)));}
                else{w.put(char(0xF0|(u>>18)));w.put(char(0x80|((u>>12)&0x3F)));w.put(char(0x80|((u>>6)&0x3F)));w.put(char(0x80|(u&0x3F)));}
                break;}
            default:return false;
            }
        }
        if(s.p>=s.end)return false;
        return w.finish()!=0;
    }
    if(cap)out[0]='\0';return false;
}

// Is the binding held now? The key down, Ctrl / Shift / Alt exactly as the
// binding says, and no Windows key. `state` answers like GetKeyState /
// GetAsyncKeyState (bit 15 = down) for a virtual key.
template<class KeyState> bool modifiers_match(const Binding& b,KeyState&& state) noexcept {
    auto held=[&](int vk){return (static_cast<unsigned short>(state(vk))&0x8000u)!=0;};
    return held(VK_CONTROL)==b.ctrl&&held(VK_SHIFT)==b.shift&&held(VK_MENU)==b.alt&&!held(VK_LWIN)&&!held(VK_RWIN);
}
template<class KeyState> bool held(const Binding& b,KeyState&& state) noexcept {
    return (static_cast<unsigned short>(state(int(b.vk)))&0x8000u)!=0&&modifiers_match(b,state);
}
// Polled press detection for the processes that wait OUTSIDE a game's message
// loop (the Steam launcher, the root proxy's on-Insert worker): true once per
// transition to held(). A binding already held when watching starts is not a
// press, so starting the game with the key down does not inject at once.
struct PressWatch {
    bool was_down=true;
    template<class KeyState> bool poll(const Binding& b,KeyState&& state) noexcept {
        const bool down=held(b,state);const bool pressed=down&&!was_down;was_down=down;return pressed;
    }
};
// Rebinding (the 修改 control in the panel and in the desktop program): what
// one key-down means while the control waits for a key. A modifier alone keeps
// waiting for the key that goes with it; Esc with no modifier cancels; any
// other key becomes `chosen`, with the modifiers `state` reports held, and is
// either allowed or refused with `why`.
enum class CaptureStep {wait,cancel,refused,chosen};
template<class KeyState> CaptureStep capture_step(unsigned vk,KeyState&& state,Binding& chosen,Refusal& why) noexcept {
    why=Refusal::none;
    if(is_modifier(vk))return CaptureStep::wait;
    auto down=[&](int k){return (static_cast<unsigned short>(state(k))&0x8000u)!=0;};
    chosen=Binding{vk&0xFFu,down(VK_CONTROL),down(VK_SHIFT),down(VK_MENU)};
    if(vk==VK_ESCAPE&&!chosen.ctrl&&!chosen.shift&&!chosen.alt)return CaptureStep::cancel;
    why=vk>0xFFu?Refusal::unsupported:down(VK_LWIN)||down(VK_RWIN)?Refusal::windows_key:check(chosen);
    return why==Refusal::none?CaptureStep::chosen:CaptureStep::refused;
}
}
