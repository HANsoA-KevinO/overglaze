// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_nr_settings.hpp"
#include <cstdio>
#include "lab_input_block.hpp"
#include <cmath>
#include "lab_overlay.hpp"
#include "lab_overlay_shader.hpp"
#include "lab_nr_panel.hpp"
#include "lab_product_ui.hpp"
#include "lab_ui_preferences.hpp"
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <array>
#include <deque>
#include <algorithm>

namespace lab {
using Microsoft::WRL::ComPtr;
namespace {
void demand(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("Overlay D3D12 error "+std::to_string(static_cast<unsigned>(r)));}
void barrier(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,0,a,b};c->ResourceBarrier(1,&v);
}
struct ContextScope{ImGuiContext* old;ContextScope(ImGuiContext* c):old(ImGui::GetCurrentContext()){ImGui::SetCurrentContext(c);}~ContextScope(){ImGui::SetCurrentContext(old);}};
ImGuiKey key(WPARAM k){if(k>='A'&&k<='Z')return ImGuiKey(ImGuiKey_A+k-'A');if(k>='0'&&k<='9')return ImGuiKey(ImGuiKey_0+k-'0');
    switch(k){case VK_TAB:return ImGuiKey_Tab;case VK_LEFT:return ImGuiKey_LeftArrow;case VK_RIGHT:return ImGuiKey_RightArrow;
    case VK_UP:return ImGuiKey_UpArrow;case VK_DOWN:return ImGuiKey_DownArrow;case VK_RETURN:return ImGuiKey_Enter;
    case VK_ESCAPE:return ImGuiKey_Escape;case VK_SPACE:return ImGuiKey_Space;case VK_BACK:return ImGuiKey_Backspace;
    case VK_DELETE:return ImGuiKey_Delete;case VK_HOME:return ImGuiKey_Home;case VK_END:return ImGuiKey_End;
    case VK_CONTROL:return ImGuiMod_Ctrl;case VK_SHIFT:return ImGuiMod_Shift;case VK_MENU:return ImGuiMod_Alt;default:return ImGuiKey_None;}}
using product::accent;
using product::muted;
constexpr ImVec4 warning{.91f,.70f,.43f,1};
bool panel_toggle(const char* id,bool& value,float scale){
    const ImVec2 size{48*scale,26*scale};
    const auto origin=ImGui::GetCursorScreenPos();
    const bool pressed=ImGui::InvisibleButton(id,size,ImGuiButtonFlags_EnableNav);
    if(pressed)value=!value;
    auto* draw=ImGui::GetWindowDrawList();
    const bool hovered=ImGui::IsItemHovered();
    const ImVec4 fill=value?accent:ImVec4(hovered?.26f:.20f,hovered?.30f:.24f,hovered?.31f:.25f,1);
    draw->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},ImGui::GetColorU32(fill),size.y*.5f);
    const float radius=9*scale;
    draw->AddCircleFilled({origin.x+(value?size.x-13*scale:13*scale),origin.y+size.y*.5f},radius,
        ImGui::GetColorU32(value?ImVec4(.10f,.15f,.10f,1):ImVec4(.79f,.82f,.79f,1)),24);
    if(ImGui::IsItemFocused())draw->AddRect({origin.x-3*scale,origin.y-3*scale},
        {origin.x+size.x+3*scale,origin.y+size.y+3*scale},ImGui::GetColorU32(accent),size.y*.5f,0,scale);
    return pressed;
}
}
struct GameOverlay::Impl {
    Controller& control;mutable std::recursive_mutex mutex;std::mutex input_mutex;
    std::atomic<bool> visible{false},stopped{false},failed{false};std::atomic<UINT> hotkey{VK_INSERT};
    // Bound to a chain (attach) and not detached since. The hotkey opens the
    // panel only while bound: an unbound panel would take the game's input and
    // draw nothing.
    std::atomic<bool> bound{false};
#ifdef LAB_OVERLAY_RESEARCH
    std::atomic<bool> capture_hidden{false},capture_override{false},cancel_capture{false};
#endif
    UiPreferencesFile preferences;
    HWND window=nullptr;WNDPROC original_proc=nullptr;IDXGISwapChain* chain_key=nullptr;
    struct Message{UINT id;WPARAM w;LPARAM l;};std::deque<Message> input;
    bool clear_input=false,input_block_installed=false;
    ImGuiContext* context=nullptr;ImFont* font=nullptr;bool renderer=false,ready=false;
    ImVector<ImWchar> glyph_ranges; // must outlive the font atlas build
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue,creation_queue;
    ComPtr<ID3D12DescriptorHeap> rtv,srv;ComPtr<ID3D12Resource> ui,scene;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12Fence> fence;std::unique_ptr<Handle> event;bool completion_unknown=false,input_hook_retained=false;
    struct Slot{ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;std::uint64_t completed=0;};
    // No back buffer is kept between presents (RE9 frame generation):
    // the current one is acquired in render(), its view written into the ring
    // slot of that frame, and the reference dropped once the draw is recorded.
    // Everything kept here is ours; nothing holds the game's chain alive.
    std::vector<Slot> slots;std::array<bool,64> descriptors{};
    std::uint64_t attaches=0,detaches=0,rebinds=0,reshapes=0;
    unsigned width=0,height=0,rtv_stride=0,srv_stride=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    DXGI_COLOR_SPACE_TYPE space=DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::uint64_t sequence=0,draws=0,skipped=0,last_tick=0,last_settings=0,settings_revision=0;
    float tone=1,structure=1,exposure_stops=0,paper_white=203,skin=1;bool settings_dirty=false,settings_loaded=false,exposure_auto=false,compare_split=false,automask=false;
    int model_style=0;
    std::string error;json controls=json::object();
    static inline std::atomic<Impl*> input_owner=nullptr;
    OverlayPreferences saved_model;bool settings_restored=false;
    std::string preference_error; // a refused save, shown in the panel; never thrown out of render
    Impl(Controller& c,std::filesystem::path path,std::filesystem::path data_root):control(c),preferences(std::move(path),OverlayPreferences{}.document(),{},std::move(data_root)){
        const auto saved=OverlayPreferences::parse(preferences.loaded());hotkey=saved.hotkey;paper_white=saved.white;saved_model=saved;}
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned i)const{auto h=srv->GetCPUDescriptorHandleForHeapStart();h.ptr+=static_cast<SIZE_T>(i)*srv_stride;return h;}
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned i)const{auto h=srv->GetGPUDescriptorHandleForHeapStart();h.ptr+=static_cast<UINT64>(i)*srv_stride;return h;}
    D3D12_CPU_DESCRIPTOR_HANDLE target(unsigned i)const{auto h=rtv->GetCPUDescriptorHandleForHeapStart();h.ptr+=static_cast<SIZE_T>(i)*rtv_stride;return h;}
    void visibility(bool value){
        std::lock_guard lock(input_mutex);visible=value;input.clear();clear_input=true;
        // Cursor clip ownership lives in input_block: released while the panel
        // is shown, the game's own clip re-applied when it closes.
        refresh_input_block();
    }
    // The game receives no mouse/keyboard while the panel is shown; a capture
    // that hides the panel hands input back until the hotkey reopens it.
#ifdef LAB_OVERLAY_RESEARCH
    void refresh_input_block(){input_block::activate(window,visible&&!stopped&&!(capture_hidden&&!capture_override));}
#else
    void refresh_input_block(){input_block::activate(window,visible&&!stopped);}
#endif
    static bool message_sink(HWND w,UINT m,WPARAM a,LPARAM b){auto* s=input_owner.load();if(!s||w!=s->window||!s->visible||s->stopped)return false;
        return procedure(w,m,a,b)==0;}
    static LRESULT CALLBACK procedure(HWND w,UINT m,WPARAM a,LPARAM b){
        auto* s=input_owner.load();if(!s||w!=s->window)return DefWindowProcW(w,m,a,b);
        if(m==WM_KILLFOCUS||(m==WM_ACTIVATEAPP&&!a)||m==WM_DESTROY)s->visibility(false);
        if(!s->stopped&&!s->failed&&s->bound&&(m==WM_KEYDOWN||m==WM_SYSKEYDOWN)&&a==s->hotkey.load()){
#ifdef LAB_OVERLAY_RESEARCH
            if(!(b&(1LL<<30))){if(s->capture_hidden&&s->visible){s->capture_override=true;s->visibility(true);}else{s->capture_override=false;s->visibility(!s->visible);}}return 0;}
#else
            if(!(b&(1LL<<30)))s->visibility(!s->visible);return 0;}
#endif
        if(!s->stopped&&s->bound&&(m==WM_KEYUP||m==WM_SYSKEYUP)&&a==s->hotkey.load())return 0;
        if(s->visible&&!s->stopped){
            // Keep the user's normal window-close command; do not trap Alt+F4.
            if(m==WM_SYSKEYDOWN&&a==VK_F4)return CallWindowProcW(s->original_proc,w,m,a,b);
#ifdef LAB_OVERLAY_RESEARCH
            if(m==WM_KEYDOWN&&a==VK_ESCAPE){if(s->capture_hidden||s->capture_override)s->cancel_capture=true;s->visibility(false);return 0;}
            if(s->capture_hidden&&!s->capture_override)return CallWindowProcW(s->original_proc,w,m,a,b);
#else
            if(m==WM_KEYDOWN&&a==VK_ESCAPE){s->visibility(false);return 0;}
#endif
            if(m==WM_INPUT)return DefWindowProcW(w,m,a,b); // Required raw-input cleanup, not game delivery.
            if((m>=WM_MOUSEFIRST&&m<=WM_MOUSELAST)||(m>=WM_KEYFIRST&&m<=WM_KEYLAST)){
                std::lock_guard lock(s->input_mutex);if(s->input.size()<512)s->input.push_back({m,a,b});else{s->clear_input=true;s->input.clear();}
                return 0;
            }
        }
        return CallWindowProcW(s->original_proc,w,m,a,b);
    }
    void dispatch(const char* method,const json& params){const auto r=control.handle_embedded(method,params,GetTickCount64());
        if(!r.value("ok",false))error=r.value("error",json::object()).value("message","Control rejected");else error.clear();}
    void feed_input(){auto& io=ImGui::GetIO();std::deque<Message> messages;
        {std::lock_guard lock(input_mutex);messages.swap(input);if(clear_input){io.ClearInputKeys();io.ClearInputMouse();clear_input=false;}}
        // DXGI can stretch buffers independently of the HWND client size. ImGui
        // draws in buffer pixels, while both Win32 sources report client pixels.
        RECT client{};const bool positioned=GetClientRect(window,&client)&&client.right>0&&client.bottom>0;
        auto position=[&](int x,int y){if(positioned)io.AddMousePosEvent(x*io.DisplaySize.x/client.right,y*io.DisplaySize.y/client.bottom);};
        POINT mouse{};if(input_block::real_cursor_position(&mouse)&&ScreenToClient(window,&mouse))position(mouse.x,mouse.y);
        for(auto m:messages){if(m.id>=WM_MOUSEFIRST&&m.id<=WM_MOUSELAST&&m.id!=WM_MOUSEWHEEL&&m.id!=WM_MOUSEHWHEEL)position(GET_X_LPARAM(m.l),GET_Y_LPARAM(m.l));switch(m.id){
        case WM_MOUSEMOVE:break;
        case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:io.AddMouseButtonEvent(0,true);break;case WM_LBUTTONUP:io.AddMouseButtonEvent(0,false);break;
        case WM_RBUTTONDOWN:case WM_RBUTTONDBLCLK:io.AddMouseButtonEvent(1,true);break;case WM_RBUTTONUP:io.AddMouseButtonEvent(1,false);break;
        case WM_MBUTTONDOWN:io.AddMouseButtonEvent(2,true);break;case WM_MBUTTONUP:io.AddMouseButtonEvent(2,false);break;
        case WM_MOUSEWHEEL:io.AddMouseWheelEvent(0,GET_WHEEL_DELTA_WPARAM(m.w)/float(WHEEL_DELTA));break;
        case WM_MOUSEHWHEEL:io.AddMouseWheelEvent(GET_WHEEL_DELTA_WPARAM(m.w)/float(WHEEL_DELTA),0);break;
        case WM_KEYDOWN:case WM_SYSKEYDOWN:case WM_KEYUP:case WM_SYSKEYUP:{const auto k=key(m.w);if(k!=ImGuiKey_None)io.AddKeyEvent(k,m.id==WM_KEYDOWN||m.id==WM_SYSKEYDOWN);break;}
        case WM_CHAR:if(m.w>0&&m.w<0x10000)io.AddInputCharacterUTF16(static_cast<ImWchar16>(m.w));break;
        default:break;}}
    }
    void style(){ImGui::GetStyle()=ImGuiStyle();product::apply_theme();auto& s=ImGui::GetStyle();
        s.WindowPadding={22,20};s.FramePadding={10,6};s.ItemSpacing={10,8};s.WindowBorderSize=1;
        s.Colors[ImGuiCol_WindowBg].w=.985f;}
    void mark(const char* name){const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();controls[name]={a.x,a.y,b.x-a.x,b.y-a.y};}
    // Before the first admitted frame the live context is not ready, so the
    // backend drops every rejection reason (LabNrLiveReject records only while
    // state==ready). The adapter snapshot still shows what the game actually
    // did and which hook is missing, so name the blocking stage here instead
    // of leaving the user with "waiting" alone. Read-only diagnosis: no
    // admission decision is made, relaxed or retried from this function.
    void waiting_diagnosis(const json& status){
        const ImVec4 warn{1,.69f,.2f,1};
        // Named refusals that would otherwise reach the JSON only: the SL
        // translation layer's last refusal, and the NGX
        // route's last skip. Each is one skipped frame, never a stop.
        {const auto rt=status.value("nr_runtime",json());
         if(rt.is_object()&&rt.value("binding_blocker",json()).is_string())
             ImGui::TextColored(warn,"翻译层跳过：%s",rt.value("binding_blocker",std::string()).c_str());
         const auto host=status.value("host",json());
         // The small ngx_admission key survives status clipping; the full
         // observation report (the largest key) is the first one dropped.
         auto adm=host.is_object()?host.value("ngx_admission",json()):json();
         if(!adm.is_object()){const auto ngx=host.is_object()?host.value("ngx_observation",json()):json();
             adm=ngx.is_object()?ngx.value("admission",json()):json();}
         if(adm.is_object()&&adm.value("receiver",false)){
             ImGui::TextDisabled("NGX 路线：已提交 %llu · 准入 %llu · 跳过 %llu",adm.value("offered",0ULL),adm.value("admitted",0ULL),adm.value("skipped",0ULL));
             // Frame generation and other NGX features: forwarded, never offered.
             if(const auto other=adm.value("ignored_other_features",0ULL)+adm.value("ignored_unobserved_without_output",0ULL))
                 ImGui::TextDisabled("NGX 路线：非超分／光线重建调用 %llu 次（如帧生成），原样放行，不计跳帧",other);
             const auto last=adm.value("last_skip",std::string());
             if(!last.empty())ImGui::TextColored(warn,"NGX 最近跳过：%s",last.c_str());}}
        const auto a=status.value("nr_adapter",json());
        if(!a.is_object()){ImGui::TextDisabled("诊断：适配器尚未发布状态。");return;}
        auto text=[](const json& j,const char* key)->std::string{
            if(!j.is_object())return {};const auto it=j.find(key);
            return it!=j.end()&&it->is_string()?it->get<std::string>():std::string{};};
        auto flag=[](const json& j,const char* key)->bool{
            if(!j.is_object())return false;const auto it=j.find(key);
            return it!=j.end()&&it->is_boolean()&&it->get<bool>();};
        auto count=[](const json& j,const char* key)->unsigned long long{
            if(!j.is_object())return 0;const auto it=j.find(key);
            return it!=j.end()&&it->is_number_integer()?it->get<unsigned long long>():0ULL;};
        auto brief=[&](const json& j,const char* key){auto v=text(j,key);return v.size()>12?v.substr(0,12):v;};
        const auto calls=a.value("api_calls",json::array());
        const unsigned long long evaluates=calls.is_array()&&calls.size()>3&&calls[3].is_number_integer()?calls[3].get<unsigned long long>():0ULL;
        // api_calls[3] counts every Evaluate, SR included, so RR is the rest.
        // (It used to be read as the RR count, which made the SR hint below
        // unreachable: any SR call made the count nonzero.)
        const auto sr=a.value("sr_evaluate",json());
        const auto upscales=count(sr,"evaluates");
        const auto rr_evaluates=evaluates>upscales?evaluates-upscales:0ULL;
        // The controller targets SR as well; a game on SR is then diagnosed
        // exactly like one on RR, against the SR options setter.
        const bool using_sr=!rr_evaluates&&upscales&&flag(a,"sr_target");
        const char* upscaler=using_sr?"SR":"RR";
        ImGui::TextDisabled("诊断：RR Evaluate %llu 次 · SR Evaluate %llu 次 · 候选 %llu · 早拒 %llu",rr_evaluates,upscales,count(a,"metadata_candidates"),count(a,"metadata_rejections"));
        if(using_sr)ImGui::TextDisabled("游戏在用 DLSS 超分（没有光线重建）：NR 接在超分输出之后。");
        if(!rr_evaluates&&!using_sr){
            if(upscales){
                ImGui::TextColored(warn,"游戏在用 SR（DLSS 超分）而不是 RR：已见 %llu 次 SR 调用。",upscales);
                ImGui::TextWrapped("这个宿主只在 RR 之后插入 NR（控制器宿主也接 SR）。若这款游戏支持光线重建，请在图形设置里开启光追／路径追踪并把“光线重建 / Ray Reconstruction”设为开。");
                const auto last=sr.is_object()?sr.value("last_tag_call",json()):json();
                const auto tags=last.is_object()?last.value("tags",json::array()):json::array();
                if(tags.is_array()&&!tags.empty()){std::string line;
                    for(const auto& t:tags)if(t.is_array()&&t.size()>=3)line+=(line.empty()?"":" · ")+std::to_string(t[0].get<unsigned>())+":"+std::to_string(t[1].get<unsigned>())+"x"+std::to_string(t[2].get<unsigned>());
                    if(!line.empty())ImGui::TextDisabled("最近一次 Tag（类型:宽x高）：%s",line.c_str());}
                return;
            }
            ImGui::TextColored(warn,"游戏还没有发出 RR（光线重建）或 SR（超分）调用。");
            ImGui::TextWrapped("NR 接在上采样器之后执行。请在游戏图形设置中开启 DLSS，并尽量把“光线重建 / Ray Reconstruction”设为开（需要光追／路径追踪）。不想超分可以选 DLAA。");
            return;
        }
        const auto options=a.value(using_sr?"sr_options":"rr_options",json()),inner=a.value("rr_inner",json()),restore=a.value("common_restore",json());
        const auto rr_module=brief(options.is_object()?options.value("module",json()):json(),"sha256");
        if(text(options,"host_state")!="observing-requests"){
            ImGui::TextColored(warn,"%s 选项挂钩未就绪（%s）。",upscaler,text(options,"host_state").c_str());
            const auto e=text(options,"error");if(!e.empty())ImGui::TextWrapped("%s",e.c_str());
            if(!rr_module.empty())ImGui::TextDisabled("当前 %s 模块 %s…",upscaler,rr_module.c_str());
            return;
        }
        // Public admission: a Streamline build with no reviewed inner profile admits on
        // public evidence; the private-hook requirements below do not apply.
        const bool public_mode=text(a,"admission_mode")=="public-evaluate-evidence-only";
        if(public_mode){
            ImGui::TextDisabled("公开准入模式：这一版 SL 没有经审阅的内部 profile，按公开证据准入。");
            if(!rr_module.empty())ImGui::TextDisabled("当前 %s 模块 %s…",upscaler,rr_module.c_str());
        }else{
            if(!flag(inner,"installed")){
                ImGui::TextColored(warn,"RR 内部回调未挂上：该 RR 模块没有已验证的 begin/end 配置。");
                const auto e=text(inner,"error");if(!e.empty())ImGui::TextWrapped("%s",e.c_str());
                if(!rr_module.empty())ImGui::TextDisabled("当前 RR 模块 %s…（请把这串发给开发：需要为它补 profile）",rr_module.c_str());
                return;
            }
            if(!flag(restore,"installed")){
                ImGui::TextColored(warn,"SL common 恢复挂钩未安装。");
                const auto e=text(restore,"error");if(!e.empty())ImGui::TextWrapped("%s",e.c_str());
                const auto m=brief(restore.is_object()?restore.value("module",json()):json(),"sha256");
                if(!m.empty())ImGui::TextDisabled("当前 common 模块 %s…",m.c_str());
                return;
            }
            const auto contract=a.value("host_rebind_contract",json());
            if(!flag(contract,"flags_readable")||(!flag(contract,"host_rebind_required")&&!flag(a.value("native_evaluate_contract",json()),"reviewed_profile"))){
                ImGui::TextColored(warn,"宿主重绑定契约不满足：这一版 SL 没有要求调用方恢复状态，暂不插入。");
                return;
            }
        }
        // Self-configuring hosts publish multiple_swapchains_blocking=false: two
        // chains are then shown, not treated as a reason to wait.
        if(a.contains("multiple_swapchains_blocking")?flag(a,"multiple_swapchains_blocking"):flag(a,"multiple_swapchains")){ImGui::TextColored(warn,"检测到多个交换链，暂不插入。");return;}
        if(flag(a,"multiple_swapchains"))ImGui::TextDisabled("检测到多个交换链（控制器不因此拒绝）。");
        const auto binding=text(a.value("latest",json()),"binding_result");
        ImGui::TextColored(warn,"有 %s 调用但未通过准入 · 最近绑定结果：%s",upscaler,binding.empty()?"-":binding.c_str());
        // Past a clean binding, which admission check refused the call.
        if(const auto lt=a.value("latest",json());lt.is_object())if(const auto adm=lt.value("admission",json());adm.is_object()&&adm.value("stage",json()).is_string()){
            const auto stage=adm.value("stage",std::string()),reason=adm.value("reason",json()).is_string()?adm.value("reason",std::string()):std::string();
            if(stage!="admitted"){ImGui::PushStyleColor(ImGuiCol_Text,warn);
                ImGui::TextWrapped("准入未通过：%s · %s · 命令列表 %s",stage.c_str(),reason.c_str(),adm.value("command",std::string("-")).c_str());ImGui::PopStyleColor();}
        }
        // The distribution, not just the latest: one launch should say which
        // cause dominates and, for an overlap, with what.
        if(const auto st=a.value("binding_stats",json());st.is_object()){
            std::vector<std::pair<unsigned long long,std::string>> top;
            if(const auto o=st.value("outcomes",json());o.is_object())for(auto it=o.begin();it!=o.end();++it)if(it->is_number_integer())top.push_back({it->get<unsigned long long>(),it.key()});
            std::sort(top.begin(),top.end(),[](const auto& x,const auto& y){return x.first>y.first;});
            std::string line;for(size_t i=0;i<top.size()&&i<4;++i)line+=(line.empty()?"":" · ")+top[i].second+" "+std::to_string(top[i].first);
            // Wrapped, not clipped: these lines are read off a screenshot.
            const auto dim=ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            if(!line.empty()){ImGui::PushStyleColor(ImGuiCol_Text,dim);ImGui::TextWrapped("绑定结果累计：%s",line.c_str());ImGui::PopStyleColor();}
            {std::string extra;
             const auto num=[&](const char* k){return st.value(k,json(0)).is_number_integer()?st.value(k,0ULL):0ULL;};
             extra="并发清缓存 "+std::to_string(num("concurrency_clears"))+" · 取令牌重叠(放行) "+std::to_string(num("benign_token_overlaps"));
             if(st.contains("tolerated_overlaps"))extra+=" · 其他重叠(放行) "+std::to_string(num("tolerated_overlaps"))+" · 同帧设值重叠(跳过) "+std::to_string(num("same_key_overlaps"))+
                 " · 锁等待 "+std::to_string(num("binding_lock_waits"))+" · 按帧标签无法归帧 "+std::to_string(num("unkeyed_tag_calls"));
             if(st.contains("presents_while_tags_fresh_tolerated"))extra+=" · Present 落在标签与评估之间 "+std::to_string(num("presents_while_tags_fresh_tolerated"));
             if(st.contains("present_expiries_with_fresh_tags"))extra+=" · Present 作废标签 "+std::to_string(num("present_expiries_with_fresh_tags"));
             if(const auto tc=st.value("tag_calls",json());tc.is_object())extra+=" · 标签调用 全局 "+std::to_string(tc.value("global",0ULL))+" / 按帧 "+std::to_string(tc.value("frame",0ULL));
             if(const auto td=st.value("tag_calls_dropped",json());td.is_object())extra+=" · 整次丢弃 setter失败 "+std::to_string(td.value("setter-failed",0ULL))+
                 " / 重叠 "+std::to_string(td.value("real-overlap",0ULL))+" / 结构 "+std::to_string(td.value("call-level-decode",0ULL))+" / 其他 "+std::to_string(td.value("other",0ULL));
             extra+=" · 单标签问题已隔离 "+std::to_string(num("tag_calls_isolated"));
             ImGui::PushStyleColor(ImGuiCol_Text,dim);ImGui::TextWrapped("%s",extra.c_str());ImGui::PopStyleColor();}
            std::string with;if(const auto w=st.value("overlap_with",json());w.is_object())for(auto it=w.begin();it!=w.end();++it)if(it->is_number_integer())with+=(with.empty()?"":" · ")+it.key()+" "+std::to_string(it->get<unsigned long long>());
            if(!with.empty()){ImGui::PushStyleColor(ImGuiCol_Text,dim);ImGui::TextWrapped("与之重叠的调用：%s",with.c_str());ImGui::PopStyleColor();}
            if(const auto rf=st.value("last_resource_failure",json());rf.is_object()){ImGui::PushStyleColor(ImGuiCol_Text,dim);
                ImGui::TextWrapped("最近缺资源：%s（%s）",rf.value("roles",json::array()).dump().c_str(),rf.value("causes",json::array()).dump().c_str());ImGui::PopStyleColor();}
            // Which roles the game declares OnlyValidNow (per Evaluate),
            // and what became of the tag-time copies of depth and motion.
            {std::string ovn;const auto add=[&](const std::string& part){ovn+=(ovn.empty()?"":" · ")+part;};
             if(const auto rl=st.value("role_lifecycles",json());rl.is_object())for(auto it=rl.begin();it!=rl.end();++it)if(it->is_object())
                 for(const char* source:{"global","frame","inline"})if(const auto n=(*it).value(source,json::object()).value("only-valid-now",0ULL))
                     add(it.key()+"/"+source+" "+std::to_string(n));
             if(const auto cp=a.value("only_valid_now_copies",json());cp.is_object()){const auto copied=cp.value("copied",json::object());
                 const auto d=copied.value("depth",0ULL),m=copied.value("motion",0ULL);if(d||m)add("已拷贝 深度 "+std::to_string(d)+" / 运动 "+std::to_string(m));
                 if(const auto b=cp.value("backoff",json());b.is_object()&&b.value("active",false))add("退避中（拷贝被拒用 "+std::to_string(cp.value("wasted_evaluates",0ULL))+" 次）");
                 if(const auto rf=cp.value("refused",json());rf.is_object())for(auto it=rf.begin();it!=rf.end();++it)if(it->is_number_integer())
                     add(it.key()+" "+std::to_string(it->get<unsigned long long>()));}
             if(!ovn.empty()){ImGui::PushStyleColor(ImGuiCol_Text,dim);ImGui::TextWrapped("OnlyValidNow：%s",ovn.c_str());ImGui::PopStyleColor();}}
        }
        // A resource the lease cannot hold is named with its full shape.
        if(const auto re=a.value("resource_entry",json());re.is_object()&&re.contains("failed_role")){
            std::string shapes;
            if(const auto rs=re.value("resources",json::array());rs.is_array())for(const auto& x:rs)
                shapes+=(shapes.empty()?"":" · ")+x.value("role",std::string("?"))+" "+std::to_string(x.value("width",0ULL))+"x"+std::to_string(x.value("height",0u))+
                    " mip"+std::to_string(x.value("mips",0u))+" 层"+std::to_string(x.value("array",0u))+" 采样"+std::to_string(x.value("samples",0u))+
                    " 格式"+std::to_string(x.value("format",0u))+" 维度"+std::to_string(x.value("dimension",0u));
            const auto dim=ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImGui::TextColored(warn,"资源租约拒绝：%s（%s）",re.value("status",std::string("")).c_str(),re.value("failed_role",std::string("")).c_str());
            ImGui::PushStyleColor(ImGuiCol_Text,dim);ImGui::TextWrapped("%s",shapes.c_str());ImGui::PopStyleColor();
        }
        // Observation for profile discovery: what the game actually tagged
        // (any feature, most recent public tag call), so one launch is enough
        // to correct a default-hypothesis viewport or depth type.
        const auto tagged=a.value("last_public_tag_call",json());
        if(tagged.is_object()){
            auto type_name=[](unsigned t)->std::string{switch(t){case 0:return "深度";case 1:return "运动矢量";case 2:return "无HUD颜色";
                case 3:return "输入颜色";case 4:return "输出颜色";case 49:return "线性深度";default:return std::to_string(t);}};
            std::string line;const auto tags=tagged.value("tags",json::array());
            // Type, extent, then the per-tag decode result: issues bits, resource type
            // (0 = 2D texture) and lifecycle, only where something is off.
            if(tags.is_array())for(const auto& t:tags)if(t.is_array()&&t.size()>=3){
                line+=(line.empty()?"":" · ")+type_name(t[0].get<unsigned>())+" "+std::to_string(t[1].get<unsigned>())+"x"+std::to_string(t[2].get<unsigned>());
                if(t.size()>=8&&(t[5].get<unsigned>()||t[6].get<unsigned>()||(t[4].is_boolean()&&t[4].get<bool>())))
                    line+="[问题 "+std::to_string(t[5].get<unsigned>())+" 资源类型 "+std::to_string(t[6].get<unsigned>())+(t[4].get<bool>()?" 空":"")+"]";}
            const auto vp=tagged.value("viewport",json());const std::string vp_text=vp.is_number()?std::to_string(vp.get<unsigned>()):std::string("-");
            {const auto dim=ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);ImGui::PushStyleColor(ImGuiCol_Text,dim);
             ImGui::TextWrapped("最近一次公开 Tag：viewport %s · 返回 %u · 整次问题 %u · %s",vp_text.c_str(),tagged.value("result",0u),tagged.value("call_issues",0u),line.empty()?"-":line.c_str());
             ImGui::PopStyleColor();}
            if(binding=="viewport-unsupported")ImGui::TextWrapped("游戏使用的 viewport 与 profile 选定的不同。请把上面这行发给开发。");
            else if(binding=="fresh-resource-missing-or-revoked")ImGui::TextWrapped("当 profile 选定的深度类型（深度／线性深度）或输出颜色 Tag 与游戏实际 Tag 不符时会出现此结果。请把上面这行发给开发。");
        }
    }
    void draw_panel(const json& status){
        auto& io=ImGui::GetIO();const float dpi=std::clamp(GetDpiForWindow(window)/96.f,1.f,2.5f);
        style();ImGui::GetStyle().ScaleAllSizes(dpi);ImGui::GetStyle().FontScaleDpi=dpi;
        ImGui::PushFont(font,16);
        const float panel_width=std::min(440.f*dpi,io.DisplaySize.x-32);
        ImGui::SetNextWindowPos({24*dpi,24*dpi},ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({panel_width,0},ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints({std::min(340.f,io.DisplaySize.x-32),0},{io.DisplaySize.x-16,io.DisplaySize.y-32});
        bool open=true;ImGui::Begin("Overglaze##panel",&open,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings);
        product::wordmark(dpi);
        ImGui::SameLine(ImGui::GetWindowWidth()-ImGui::GetStyle().WindowPadding.x-44*dpi);
        if(ImGui::SmallButton("收起"))visibility(false);mark("close");
        const char* key_name=hotkey==VK_F7?"F7":hotkey==VK_F8?"F8":hotkey==VK_F9?"F9":"Insert";
        ImGui::Spacing();
        const auto panel=nr_panel(status,true,false,"@embedded",false);bool on=nr::executes(panel.requested);
        const auto nr=status.value("nr_runtime",json());
        const char* state=panel.failed?"STOPPED":panel.observed=="on"?"ON":panel.observed=="compute-only"?"COMPUTE":panel.observed=="off"?"OFF":"WAIT";
        const char* observed=panel.failed?"本次开启未通过安全检查":panel.observed=="on"?"正在使用 NR 输出":panel.observed=="compute-only"?"仅计算 · 不写回游戏":panel.observed=="off"?"未执行 NR":"等待实际帧回执";
        const char* requested_mode=panel.requested=="on"?"ON":panel.requested=="compute-only"?"COMPUTE":panel.requested=="off"?"OFF":"未知";
        const ImVec4 state_color=panel.failed?warning:panel.observed=="on"?accent:muted;
        // Fixed-height status keeps the main controls in place while requested
        // and observed modes change independently. A request is never an ON receipt.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2{16*dpi,13*dpi});
        ImGui::BeginChild("nr_status",{0,114*dpi},ImGuiChildFlags_AlwaysUseWindowPadding,ImGuiWindowFlags_NoScrollbar);
        ImGui::AlignTextToFramePadding();product::eyebrow("NEURAL RENDERING");
        ImGui::SameLine(ImGui::GetWindowWidth()-ImGui::GetStyle().WindowPadding.x-48*dpi);
        ImGui::BeginDisabled(!panel.can_toggle);
        if(panel_toggle("启用 Neural Rendering",on,dpi))dispatch("SetNrMode",{{"mode",on?"on":"off"}});
        mark("nr_toggle");ImGui::EndDisabled();
        ImGui::SetItemTooltip("请求开启 / 关闭 NR。下方状态以实际帧回执为准。");
        ImGui::PushFont(font,25);ImGui::TextColored(state_color,"%s",state);ImGui::PopFont();
        ImGui::SameLine(0,12*dpi);ImGui::TextUnformatted(observed);
        ImGui::TextDisabled("请求 %s  ·  每次启动默认关闭",requested_mode);
        ImGui::EndChild();ImGui::PopStyleVar();
        if(panel.failed&&!panel.error.empty())ImGui::TextWrapped("%s",panel.error.c_str());
        if(panel.preparing||panel.rebuilding)ImGui::TextWrapped("正在等待可用帧 / 安全准备。无需重新启动游戏。");
        if(nr.is_object()&&nr.value("consecutive_skips",0u))
            ImGui::TextColored(warning,"连续 %u 帧未插入 · 原因见下方诊断",nr.value("consecutive_skips",0u));
        if(panel.other_owner)ImGui::TextWrapped("外部工具已显式接管；这里暂为只读。归还后恢复游戏内控制。");
        const auto requested=status.value("nr_settings_request",json());
        if(requested.is_object()&&(!settings_loaded||(!settings_dirty&&!ImGui::IsAnyItemActive()&&requested.value("revision",0ULL)!=settings_revision))){
            const auto v=requested.at("values");tone=v.value("tone",1.f);structure=v.value("structure",1.f);exposure_stops=v.value("exposure_stops",0.f);exposure_auto=v.value("exposure_auto",0u)!=0;compare_split=v.value("compare_split",0u)!=0;model_style=v.value("style",0);skin=v.value("skin",1.f);automask=v.value("automask",0u)!=0;settings_loaded=true;settings_revision=requested.value("revision",0ULL);}
        ImGui::Spacing();product::eyebrow("外观  /  APPEARANCE");
        const bool writable=!panel.other_owner&&!panel.failed;bool released=false;
        // Restore the last requested controls once per process as a staged
        // request (OFF allowed). The ON gate itself is never restored.
        if(saved_model.has_model&&!settings_restored&&writable&&requested.is_object()){settings_restored=true;
            dispatch("SetNrSettings",{{"tone",saved_model.tone},{"structure",saved_model.structure},{"style",saved_model.style},{"exposure_stops",saved_model.exposure_stops},{"exposure_auto",saved_model.exposure_auto},
                {"skin",saved_model.skin},{"automask",saved_model.automask}});}
        ImGui::BeginDisabled(!writable);
        const float style_width=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x*2)/3;
        for(int i=0;i<3;++i){if(i)ImGui::SameLine();const char* labels[]{"Style 0","Style 1","Style 2"};
            const bool selected=product::action(labels[i],{style_width,34*dpi},model_style==i);mark(labels[i]);
            ImGui::SetItemTooltip("模型 Style %d；尚未对应官方 A / B / C。",i);
            if(selected&&model_style!=i){model_style=i;settings_dirty=true;released=true;}}
        ImGui::Spacing();
        const float label_x=ImGui::GetCursorPosX()+92*dpi;
        // 0..2 since Live ABI22 (the model has no hard cap; RenoDX uses 1.85/2.00).
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Tone");ImGui::SameLine(label_x);ImGui::SetNextItemWidth(-1);
        if(ImGui::SliderFloat("##tone",&tone,0,nr::Settings::max_tone_structure,"%.2f",ImGuiSliderFlags_AlwaysClamp))settings_dirty=true;
        mark("tone");released|=ImGui::IsItemDeactivatedAfterEdit();ImGui::SetItemTooltip("局部色调强度 · 默认 1.00");
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Structure");ImGui::SameLine(label_x);ImGui::SetNextItemWidth(-1);
        if(ImGui::SliderFloat("##structure",&structure,0,nr::Settings::max_tone_structure,"%.2f",ImGuiSliderFlags_AlwaysClamp))settings_dirty=true;
        mark("structure");released|=ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("局部结构强度 · 默认 1.00；0 不等于关闭 NR。");
        // Skin (DLSSNR.SkinStructureStrength) only acts with the automatic mask
        // on, so moving it turns AutoMask on; the mask can be switched off again.
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Skin");ImGui::SameLine(label_x);ImGui::SetNextItemWidth(-1);
        if(ImGui::SliderFloat("##skin",&skin,0,nr::Settings::max_skin,automask?"%.2f":"%.2f · 未生效",ImGuiSliderFlags_AlwaysClamp)){settings_dirty=true;automask=true;}
        mark("skin");released|=ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("皮肤区域的 Structure 强度：0＝几乎不改皮肤，1＝默认。拖动会自动打开 AutoMask。");
        ImGui::SetCursorPosX(label_x);if(ImGui::Checkbox("AutoMask",&automask)){settings_dirty=true;released=true;}mark("automask");
        ImGui::SameLine();ImGui::TextDisabled("皮肤区域识别");
        ImGui::SetItemTooltip("Skin 只在 AutoMask 开启时生效；拖动 Skin 会自动打开它。");
        ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
        ImGui::AlignTextToFramePadding();product::eyebrow("输入曝光");
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x-128*dpi);
        if(ImGui::Checkbox("自动测光",&exposure_auto)){settings_dirty=true;released=true;}mark("exposure_auto");
        ImGui::SetItemTooltip("画面均值映射到中灰；开启后滑条调整自动测光的 EV 偏移。属于宿主颜色准备，不是模型参数。");
        ImGui::SetNextItemWidth(-1);
        if(ImGui::SliderFloat("##exposure",&exposure_stops,nr::Settings::min_exposure_stops,nr::Settings::max_exposure_stops,exposure_auto?"EV 偏移   %+.1f":"Exposure  %+.1f EV",ImGuiSliderFlags_AlwaysClamp))settings_dirty=true;
        mark("exposure");released|=ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("送入 NR 前的宿主曝光；回填时会除回。不改变游戏的曝光设置。");
        ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("分屏对照");
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x-48*dpi);
        if(panel_toggle("对比分屏：左半原图 / 右半 NR",compare_split,dpi)){settings_dirty=true;released=true;}mark("compare_split");
#ifdef LAB_OVERLAY_RESEARCH
        ImGui::SetItemTooltip("仅诊断视图；开启时不允许采集。");
#else
        ImGui::SetItemTooltip("仅诊断视图；不作为画质验收依据。");
#endif
        ImGui::TextDisabled("左侧原图  /  右侧 NR");
        ImGui::EndDisabled();
        if(settings_dirty&&writable&&(released||GetTickCount64()-last_settings>=100)){
            dispatch("SetNrSettings",{{"tone",tone},{"structure",structure},{"style",model_style},{"exposure_stops",exposure_stops},{"exposure_auto",exposure_auto?1:0},{"compare_split",compare_split?1:0},
                {"skin",skin},{"automask",automask?1:0}});last_settings=GetTickCount64();settings_dirty=false;}
        if(!on)ImGui::TextDisabled("关闭时可调整参数 · 下次开启后应用");
#ifdef LAB_OVERLAY_RESEARCH
        ImGui::Spacing();ImGui::Separator();ImGui::TextDisabled("CAPTURE");
        const auto pair=status.value("frame_pair",json());const std::string phase=pair.is_object()?pair.value("state",""):"";
        const bool busy=!phase.empty()&&phase!="idle"&&phase!="complete"&&phase!="failed"&&phase!="cancelled";
        ImGui::BeginDisabled(!writable||!nr::executes(panel.observed)||busy||!status.value("capabilities",json::object()).value("capture_pair",false));
        if(ImGui::Button("采集一组原始数据",{-1,42*dpi}))dispatch("CapturePair",json::object());mark("capture");ImGui::EndDisabled();
        if(phase=="requested")ImGui::TextWrapped("等待后端接收；最多 10 秒，超时不补采。");
        if(busy){ImGui::TextWrapped("本次仅一组，完成即停；Esc 取消。已提交的 GPU 工作需安全结束。");if(ImGui::Button("取消采集"))dispatch("CancelCapturePair",json::object());mark("cancel_capture");}
        if(phase=="complete")ImGui::TextColored({.60f,.8f,.3f,1},"已保存 · %llu MiB · 帧 %llu",pair.value("bytes",0ULL)/(1024*1024),pair.value("frame",0ULL));
        else if(phase=="failed"){
            if(pair.value("failure_code","")=="dispatch_timeout")ImGui::TextWrapped("本次已过期，没有启动复制；需要时请重新点击。");
            else ImGui::TextWrapped("采集失败，未自动重试：%s",pair.value("error",std::string("原因未提供")).c_str());
        }
        else if(phase=="cancelled")ImGui::TextDisabled("采集已取消，不会自动补采。");
        else if(pair.is_object()&&pair.contains("available")&&!pair.value("available",false))ImGui::TextWrapped("采集器未建成（NR 本身不受影响）：%s",pair.value("note",std::string("原因未提供")).c_str());
        else if(pair.is_object()&&pair.value("note","")=="guide-extent-outside-replay-contract-native-or-half")ImGui::TextWrapped("四色采集：RR 原图 / NR 输入 / NR 原始输出 / HDR 回填。Depth/MV %ux%u 不是颜色的原生或一半尺寸，不进离线回放包（不缩放）。",nr.is_object()?nr.value("guide_width",0u):0u,nr.is_object()?nr.value("guide_height",0u):0u);
        else if(pair.is_object()&&pair.value("note","")=="linear-depth-profile-four-colors-only")ImGui::TextDisabled("四色采集：RR 原图 / NR 输入 / NR 原始输出 / HDR 回填 · 线性深度配置不保存 Depth/MV");
        else if(pair.is_object()&&pair.value("note","")=="motion-resampled-by-lab-guides-not-captured")ImGui::TextDisabled("四色采集：RR 原图 / NR 输入 / NR 原始输出 / HDR 回填 · MV 经 Lab 重采样到 guide 网格，不保存 Depth/MV");
        else ImGui::TextDisabled("RR 原图 / NR 输入 / NR 原始输出 / HDR 回填");
#else
        // No raw-data capture in the controller panel; frame_pair is a research record.
        const bool busy=false;
#endif
        ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
        const bool diagnostics=ImGui::CollapsingHeader("高级与诊断");mark("diagnostics");
        if(diagnostics){
            ImGui::PushTextWrapPos(0);
            product::eyebrow("实际状态 / OBSERVED");
            ImGui::TextDisabled("请求：%s · 实际：%s",panel.requested.c_str(),panel.observed.c_str());
            if(panel.preparing||panel.rebuilding)waiting_diagnosis(status);
            if(nr.is_object()&&(nr.value("skipped_frames",0ULL)>0||nr.value("discarded_recordings",0ULL)>0)){
                const auto rejected=nr.value("rejected_call",json());
                ImGui::TextDisabled("累计跳过 %llu 帧 · 帧序中断 %llu 次 · 游戏丢弃 %llu 帧（均已重置历史）",
                    nr.value("skipped_frames",0ULL),nr.value("history_gaps",0ULL),nr.value("discarded_recordings",0ULL));
                ImGui::TextWrapped("最近原因：%s",rejected.is_object()?rejected.value("reason",std::string("-")).c_str():"-");}
            ImGui::Spacing();product::eyebrow("模型读取回执");
            if(nr.is_object()){const auto s=nr.value("settings",json::object());const auto v=s.value("observed",json());
                if(s.value("read_mask",0u)==3&&s.value("style_read",false)&&v.is_object()){
                    ImGui::TextDisabled("Tone %.2f · Structure %.2f · Style %u",v.value("tone",0.f),v.value("structure",0.f),v.value("style",0u));
                    ImGui::TextDisabled("宿主曝光 %+.1f EV%s · 帧 %llu",s.value("applied_exposure_stops",v.value("exposure_stops",0.f)),v.value("exposure_auto",0u)?"（自动）":"",s.value("frame",0ULL));
                    // Unread optional controls mirror the request; only *_read says the DLL read them.
                    const bool skin_read=s.value("skin_read",false),mask_read=s.value("automask_read",false);
                    char skin_text[16]="未读";if(skin_read)std::snprintf(skin_text,sizeof skin_text,"%.2f",v.value("skin",0.f));
                    if(skin_read||mask_read)ImGui::TextDisabled("Skin %s · AutoMask %s",skin_text,mask_read?(v.value("automask",0u)?"开":"关"):"未读");
                    else ImGui::TextDisabled("Skin / AutoMask：本代 Evaluate 未读取");}
                else ImGui::TextDisabled("尚无本代 Evaluate 读取回执");}
            else ImGui::TextDisabled("尚无本代 Evaluate 读取回执");
            // What this host has actually seen work, accumulated over the process.
            // Sticky on purpose: turning NR off does not un-prove a read-back.
            if(nr.is_object()){const auto caps=nr.value("capabilities",json::object());
                if(!caps.empty()){const auto seen=[&](const char* k){return caps.value(k,false)?"是":"未";};
                    ImGui::TextDisabled("已观察生效：Tone %s · Structure %s · Style %s · 曝光 %s · GPU 完成 %s · 证据等级 %s",
                        seen("can_set_tone"),seen("can_set_structure"),seen("can_set_style"),seen("can_set_exposure"),seen("can_observe_gpu"),
                        caps.value("evidence_level",std::string("L0")).c_str());
                    ImGui::TextDisabled("仅表示 NR DLL 在 Evaluate 期间回读过该值，不代表画质正确或与官方接入一致。");}}
            if(status.value("capabilities",json::object()).value("nr_compute_only",false)){
                ImGui::BeginDisabled(!panel.can_toggle||busy);
                if(ImGui::Button("仅计算，不写回游戏",{-1,0}))dispatch("SetNrMode",{{"mode","compute-only"}});mark("compute_only");
                ImGui::EndDisabled();ImGui::TextWrapped("诊断模式仍执行转换、NR 和私有合成；不代表零影响。上方开关可停止计算。");
            }
#ifdef LAB_OVERLAY_RESEARCH
            ImGui::TextWrapped("原始数据不会因面板显示而改变。研究顺序、颜色校准与游戏长期稳定性仍单独验收。");
#else
            ImGui::TextWrapped("\u9762\u677f\u53ea\u505a\u5b9e\u65f6\u63a7\u5236\uff1aNR \u5f00\u5173 \u00b7 Tone / Structure / Style \u00b7 \u4e3b\u673a\u66dd\u5149 \u00b7 \u5bf9\u6bd4\u5206\u5c4f \u00b7 \u4ec5\u8ba1\u7b97\u3002\u539f\u59cb\u6570\u636e\u91c7\u96c6\u4e0e\u79bb\u7ebf\u56de\u653e\u5728\u7814\u7a76\u89c2\u5bdf\u5668\u91cc\u3002");
            ImGui::TextWrapped("\u9762\u677f\u663e\u793a\u4e0d\u6539\u53d8\u6e38\u620f\u753b\u9762\u3002\u8272\u6821\u51c6\u4e0e\u6e38\u620f\u81ea\u8eab\u7a33\u5b9a\u6027\u5355\u72ec\u53e6\u7b97\u3002");
#endif
            const char* keys[]{"Insert","F7","F8","F9"};const UINT values[]{VK_INSERT,VK_F7,VK_F8,VK_F9};int selected=0;for(int i=0;i<4;++i)if(hotkey==values[i])selected=i;
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();product::eyebrow("面板设置");
            ImGui::TextUnformatted("快捷键");ImGui::SetNextItemWidth(-1);
            if(ImGui::Combo("##panel_hotkey",&selected,keys,4))hotkey=values[selected];mark("hotkey");
            ImGui::TextUnformatted("面板白电平");ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##panel_white",&paper_white,80,300,"%.0f nits");mark("paper_white");
            ImGui::Text("输出：%s",space==DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709?"scRGB":space==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020?"HDR10 / PQ":"SDR / sRGB");
            if(!panel.error.empty())ImGui::TextWrapped("%s",panel.error.c_str());if(!error.empty())ImGui::TextWrapped("%s",error.c_str());
            const auto saved_error=preferences.error();if(!saved_error.empty())ImGui::TextWrapped("界面设置未保存：%s",saved_error.c_str());
            if(!preference_error.empty())ImGui::TextWrapped("界面设置未保存：%s",preference_error.c_str());
            ImGui::PopTextWrapPos();}
        ImGui::Spacing();ImGui::TextDisabled("%s 收起  ·  Esc 返回游戏",key_name);
        {OverlayPreferences current;current.hotkey=hotkey.load();current.white=paper_white;
         if(settings_loaded){current.has_model=true;current.tone=tone;current.structure=structure;current.style=unsigned(model_style);current.exposure_stops=exposure_stops;current.exposure_auto=exposure_auto?1u:0u;current.skin=skin;current.automask=automask?1u:0u;}
         // A settings file must never take the panel down. A file range check
         // narrower than the slider's (-6..10 against -12..10) once made this
         // throw on every frame: render failed, the panel hid, and each Insert
         // re-failed on its first frame.
         try{preferences.save(current.document());preference_error.clear();}
         catch(const std::exception& e){preference_error=e.what();}}
        ImGui::End();ImGui::PopFont();if(!open)visibility(false);
    }
    bool wait(){if(!fence)return true;const auto done=fence->GetCompletedValue();if(done==UINT64_MAX)return false;if(done>=sequence)return true;
        if(!event||FAILED(fence->SetEventOnCompletion(sequence,event->value)))return false;return WaitForSingleObject(event->value,2000)==WAIT_OBJECT_0;}
    void release_gpu(){demand(wait(),"Overlay completion unknown; keep GPU resources alive");
        if(context){ContextScope scope(context);if(renderer)ImGui_ImplDX12_Shutdown();renderer=false;}
        slots.clear();ui.Reset();scene.Reset();pipeline.Reset();root.Reset();rtv.Reset();srv.Reset();fence.Reset();event.reset();
        width=height=0;sequence=0;ready=false;descriptors.fill(false);
    }
    void initialize(IDXGISwapChain3* chain){
        demand(queue!=nullptr,"Overlay queue unavailable after Resize");
        DXGI_SWAP_CHAIN_DESC1 desc{};hr(chain->GetDesc1(&desc));demand(desc.BufferCount>=2&&desc.BufferCount<=8&&desc.Width&&desc.Height,"Unsupported overlay swapchain");
        demand(desc.Width<=8192&&desc.Height<=8192&&static_cast<std::uint64_t>(desc.Width)*desc.Height<=24000000,"Overlay texture budget exceeded");
        demand(desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM||desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM||desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT,"Unsupported overlay target format");
        width=desc.Width;height=desc.Height;format=desc.Format;hr(queue->GetDevice(IID_PPV_ARGS(&device)));
        if(!context){context=ImGui::CreateContext();ContextScope scope(context);auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
            io.ConfigFlags=ImGuiConfigFlags_NavEnableKeyboard;io.MouseDrawCursor=true;io.ConfigInputTrickleEventQueue=false;
            // The common simplified set lacks 釉 (U+91C9), the first glyph of the
            // product name; add exactly the name's own glyphs to it.
            {ImFontGlyphRangesBuilder builder;builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());builder.AddText("釉光 · Overglaze");builder.BuildRanges(&glyph_ranges);}
            font=io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc",18,nullptr,glyph_ranges.Data);
            if(!font)font=io.Fonts->AddFontDefault();style();}
        ContextScope scope(context);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=desc.BufferCount+1;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)));
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=64;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srv)));
        rtv_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);srv_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        // RTVs 0..BufferCount-1 are a ring, one per slot, written per present;
        // the last one is the panel layer's.
        slots.resize(desc.BufferCount);for(unsigned i=0;i<desc.BufferCount;++i){hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slots[i].allocator)));
            hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slots[i].allocator.Get(),nullptr,IID_PPV_ARGS(&slots[i].commands)));hr(slots[i].commands->Close());}
        // The scene copy and the panel layer take the back buffer's shape; the
        // buffer is borrowed for that one read.
        D3D12_RESOURCE_DESC texture{};{ComPtr<ID3D12Resource> sample;hr(chain->GetBuffer(0,IID_PPV_ARGS(&sample)));texture=sample->GetDesc();}
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        texture.Flags=D3D12_RESOURCE_FLAG_NONE;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&scene)));
        texture.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;D3D12_CLEAR_VALUE clear{};clear.Format=texture.Format;
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,&clear,IID_PPV_ARGS(&ui)));device->CreateRenderTargetView(ui.Get(),nullptr,target(desc.BufferCount));
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        view.Format=format;device->CreateShaderResourceView(scene.Get(),&view,cpu(0));view.Format=texture.Format;device->CreateShaderResourceView(ui.Get(),&view,cpu(1));descriptors[0]=descriptors[1]=true;
        ImGui_ImplDX12_InitInfo info;info.Device=device.Get();info.CommandQueue=queue.Get();info.NumFramesInFlight=desc.BufferCount;info.RTVFormat=texture.Format;info.UserData=this;info.SrvDescriptorHeap=srv.Get();
        info.SrvDescriptorAllocFn=[](ImGui_ImplDX12_InitInfo* i,D3D12_CPU_DESCRIPTOR_HANDLE* c,D3D12_GPU_DESCRIPTOR_HANDLE* g){auto* s=static_cast<Impl*>(i->UserData);
            for(unsigned k=2;k<s->descriptors.size();++k)if(!s->descriptors[k]){s->descriptors[k]=true;*c=s->cpu(k);*g=s->gpu(k);return;}throw std::runtime_error("Overlay font descriptor budget exceeded");};
        info.SrvDescriptorFreeFn=[](ImGui_ImplDX12_InitInfo* i,D3D12_CPU_DESCRIPTOR_HANDLE c,D3D12_GPU_DESCRIPTOR_HANDLE){auto* s=static_cast<Impl*>(i->UserData);
            auto k=(c.ptr-s->cpu(0).ptr)/s->srv_stride;if(k>=2&&k<s->descriptors.size())s->descriptors[k]=false;};
        demand(ImGui_ImplDX12_Init(&info),"Cannot initialize overlay renderer");renderer=true;
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&range};params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,4};params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};ComPtr<ID3DBlob> blob,errors,vs,ps;
        hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
        hr(D3DCompile(overlay_shader,sizeof(overlay_shader)-1,nullptr,nullptr,nullptr,"VS","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vs,&errors));
        hr(D3DCompile(overlay_shader,sizeof(overlay_shader)-1,nullptr,nullptr,nullptr,"PS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&ps,&errors));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.SampleMask=UINT_MAX;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
        pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.StencilEnable=FALSE;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=format;pd.SampleDesc.Count=1;
        hr(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pipeline)));hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        event=std::make_unique<Handle>(CreateEventW(nullptr,FALSE,FALSE,nullptr));demand(event->valid(),"Overlay event unavailable");
        ready=true;
    }
    void render(IDXGISwapChain3* chain){
#ifdef LAB_OVERLAY_RESEARCH
        const auto status=control.status();const auto pair=status.value("frame_pair",json());
        bool busy=false;if(pair.is_object()){const auto phase=pair.value("state","");busy=!phase.empty()&&phase!="idle"&&phase!="complete"&&phase!="cancelled"&&phase!="failed";}
        if(!busy)capture_override=false;capture_hidden=busy&&!capture_override;refresh_input_block();
        if(capture_hidden){++skipped;return;}
#else
        const auto status=control.status();
#endif
        if(space!=DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709&&space!=DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709&&space!=DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
            throw std::runtime_error("Unknown swapchain color space; overlay not drawn");
        // The chain's shape, every present: a chain recreated at the old address,
        // or resized past our hook, is never drawn with the previous shape.
        DXGI_SWAP_CHAIN_DESC1 shape{};hr(chain->GetDesc1(&shape));
        if(ready&&(shape.Width!=width||shape.Height!=height||shape.Format!=format||shape.BufferCount!=slots.size())){release_gpu();++reshapes;}
        if(!ready)initialize(chain);auto& slot=slots[sequence%slots.size()];const auto complete=fence->GetCompletedValue();
        demand(complete!=UINT64_MAX,"Overlay device removed");if(complete<slot.completed){++skipped;return;}
        ContextScope scope(context);auto& io=ImGui::GetIO();io.DisplaySize={float(width),float(height)};const auto now=GetTickCount64();io.DeltaTime=last_tick?std::clamp((now-last_tick)/1000.f,.001f,.1f):1.f/60;last_tick=now;
        feed_input();ImGui_ImplDX12_NewFrame();ImGui::NewFrame();controls=json::object();draw_panel(status);ImGui::Render();
        hr(slot.allocator->Reset());hr(slot.commands->Reset(slot.allocator.Get(),nullptr));auto* c=slot.commands.Get();const auto index=chain->GetCurrentBackBufferIndex();demand(index<shape.BufferCount,"Overlay back buffer index out of range");
        // Borrowed for this present only; released when render() returns. The
        // view goes into this frame's ring slot (an RTV is read when it is bound).
        ComPtr<ID3D12Resource> back;hr(chain->GetBuffer(index,IID_PPV_ARGS(&back)));auto* buffer=back.Get();
        const unsigned ring=unsigned(sequence%slots.size());device->CreateRenderTargetView(buffer,nullptr,target(ring));
        barrier(c,buffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);barrier(c,scene.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        c->CopyResource(scene.Get(),buffer);barrier(c,scene.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(c,buffer,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);barrier(c,ui.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clear[4]{};auto ui_rtv=target(unsigned(slots.size()));c->ClearRenderTargetView(ui_rtv,clear,0,nullptr);c->OMSetRenderTargets(1,&ui_rtv,FALSE,nullptr);
        ID3D12DescriptorHeap* heaps[]{srv.Get()};c->SetDescriptorHeaps(1,heaps);ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(),c);
        barrier(c,ui.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);auto output=target(ring);c->OMSetRenderTargets(1,&output,FALSE,nullptr);
        c->SetGraphicsRootSignature(root.Get());c->SetPipelineState(pipeline.Get());c->SetGraphicsRootDescriptorTable(0,gpu(0));
        struct Constants{unsigned transfer;float white;float pad[2];}constants{space==DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709?1u:space==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020?2u:0u,paper_white,{0,0}};
        c->SetGraphicsRoot32BitConstants(1,4,&constants,0);D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};D3D12_RECT rect{0,0,LONG(width),LONG(height)};
        c->RSSetViewports(1,&viewport);c->RSSetScissorRects(1,&rect);c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->DrawInstanced(3,1,0,0);
        barrier(c,buffer,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);hr(c->Close());ID3D12CommandList* commands[]{c};queue->ExecuteCommandLists(1,commands);
        ++sequence;slot.completed=sequence;hr(queue->Signal(fence.Get(),sequence));++draws;
    }
};
GameOverlay::GameOverlay(Controller& c,std::filesystem::path path,std::filesystem::path data_root):p_(std::make_unique<Impl>(c,std::move(path),std::move(data_root))){}
GameOverlay::~GameOverlay(){stop();if(p_->completion_unknown||p_->input_hook_retained){(void)p_.release();return;}
    if(p_->context){auto* old=ImGui::GetCurrentContext();ImGui::DestroyContext(p_->context);ImGui::SetCurrentContext(old==p_->context?nullptr:old);p_->context=nullptr;}}
void GameOverlay::attach(IDXGISwapChain3* chain,ID3D12CommandQueue* queue){auto& s=*p_;std::lock_guard lock(s.mutex);
    demand(chain&&queue&&queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT,"Overlay needs a verified DIRECT queue");
    demand(!s.stopped,"Overlay has stopped");demand(!s.completion_unknown,"Overlay GPU completion unknown; not attached again");
    DXGI_SWAP_CHAIN_DESC d{};hr(chain->GetDesc(&d));demand(d.OutputWindow&&IsWindow(d.OutputWindow),"Overlay needs an HWND swapchain");
    if(s.chain_key==chain&&s.window==d.OutputWindow){if(s.queue.Get()!=queue){demand(!s.ready,"Queue replacement requires Resize completion first");s.queue=queue;}return;}
    if(s.chain_key){
        // A second live HWND is not admitted. Same-HWND chain recreation is a
        // new generation, never pointer-only reuse of the previous resources.
        if(s.window!=d.OutputWindow&&IsWindow(s.window))return;
        s.visibility(false);s.release_gpu();++s.rebinds;
        if(s.window!=d.OutputWindow){
            if(IsWindow(s.window))demand(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(s.window,GWLP_WNDPROC))==&Impl::procedure,"Input chain changed; cannot replace window");
            s.window=nullptr;
        }
    }else if(s.window&&s.window!=d.OutputWindow){
        // Detached from a chain on another window: that window gets its own
        // procedure back before the new one is subclassed.
        if(IsWindow(s.window)){
            demand(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(s.window,GWLP_WNDPROC))==&Impl::procedure,"Input chain changed; cannot move the panel to another window");
            SetWindowLongPtrW(s.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(s.original_proc));
        }
        s.window=nullptr;s.original_proc=nullptr;
    }
    auto* expected=static_cast<Impl*>(nullptr);demand(Impl::input_owner.compare_exchange_strong(expected,&s)||expected==&s,"Another overlay owns the input hook");
    const bool install_proc=s.window!=d.OutputWindow;s.chain_key=chain;s.window=d.OutputWindow;s.queue=s.creation_queue=queue;s.failed=false;
    s.space=d.BufferDesc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    s.input_block_installed=input_block::install(&Impl::message_sink);
    if(install_proc){s.original_proc=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(s.window,GWLP_WNDPROC));SetLastError(0);
        const auto prior=SetWindowLongPtrW(s.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&Impl::procedure));
        if(!prior&&GetLastError()!=0){expected=&s;Impl::input_owner.compare_exchange_strong(expected,nullptr);s.chain_key=nullptr;s.window=nullptr;throw std::runtime_error("Cannot attach overlay input");}}
    ++s.attaches;s.bound=true;
}
bool GameOverlay::detach()noexcept{auto& s=*p_;std::lock_guard lock(s.mutex);if(s.stopped)return false;
    s.bound=false;try{s.visibility(false);}catch(...){}
    if(!s.chain_key)return !s.completion_unknown;
    // Our own work on the old queue must have finished before its resources go;
    // when that is unknown they are kept and the panel is not attached again.
    try{s.release_gpu();}catch(const std::exception& e){s.completion_unknown=true;s.failed=true;s.error=e.what();return false;}
    s.chain_key=nullptr;s.queue.Reset();s.creation_queue.Reset();s.failed=false;s.error.clear();++s.detaches;return true;
}
void GameOverlay::color_space(IDXGISwapChain* chain,DXGI_COLOR_SPACE_TYPE color)noexcept{auto& s=*p_;std::lock_guard lock(s.mutex);if(chain==s.chain_key)s.space=color;}
bool GameOverlay::presentation_source(IDXGISwapChain* chain,ID3D12CommandQueue** out,DXGI_COLOR_SPACE_TYPE* color)noexcept{
    if(!out||!color)return false;*out=nullptr;auto& s=*p_;std::unique_lock lock(s.mutex,std::try_to_lock);
    if(!lock.owns_lock()||s.stopped||s.failed||chain!=s.chain_key||!s.queue)return false;
    *out=s.queue.Get();(*out)->AddRef();*color=s.space;return true;
}
void GameOverlay::present(IDXGISwapChain* chain)noexcept{auto& s=*p_;if(s.stopped)return;
#ifdef LAB_OVERLAY_RESEARCH
    // A queued cancellation must still be delivered while the panel is hidden.
    if(s.failed||(!s.visible&&!s.cancel_capture))return;
    std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock())return;
    if(chain!=s.chain_key)return;
    if(s.cancel_capture.exchange(false))try{s.dispatch("CancelCapturePair",json::object());}catch(...){}
    if(!s.visible)return;
#else
    if(s.failed||!s.visible)return;
    std::unique_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock())return;
    if(chain!=s.chain_key)return;
#endif
    try{ComPtr<IDXGISwapChain3> c;hr(chain->QueryInterface(IID_PPV_ARGS(&c)));s.render(c.Get());}catch(const std::exception& e){s.error=e.what();s.failed=true;s.visibility(false);s.control.diagnostic(s.error);}}
bool GameOverlay::before_resize(IDXGISwapChain* chain)noexcept{auto& s=*p_;std::lock_guard lock(s.mutex);if(chain!=s.chain_key)return true;
    try{s.release_gpu();s.failed=false;return true;}catch(const std::exception& e){s.error=e.what();s.failed=true;s.visibility(false);return false;}}
void GameOverlay::resized_queues(IDXGISwapChain3* chain,std::span<ID3D12CommandQueue* const> queues)noexcept{
    auto& s=*p_;std::lock_guard lock(s.mutex);if(chain!=s.chain_key)return;
    try{
        demand(!s.ready,"Queue replacement requires completed Resize preparation");
        DXGI_SWAP_CHAIN_DESC1 desc{};hr(chain->GetDesc1(&desc));
        demand(queues.size()==desc.BufferCount&&!queues.empty(),"Overlay resize queue identity unavailable");
        ComPtr<IUnknown> first,chain_device;hr(chain->GetDevice(IID_PPV_ARGS(&chain_device)));
        for(auto* queue:queues){
            demand(queue&&queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT,"Unsupported overlay resize queue");
            ComPtr<IUnknown> identity,device;hr(queue->QueryInterface(IID_PPV_ARGS(&identity)));hr(queue->GetDevice(IID_PPV_ARGS(&device)));
            demand(device.Get()==chain_device.Get(),"Overlay resize queue device mismatch");
            if(first)demand(first.Get()==identity.Get(),"Rotating Present queues are not supported by this overlay");else first=identity;
        }
        s.queue=queues.front();s.failed=false;s.error.clear();
    }catch(const std::exception& e){s.queue.Reset();s.error=e.what();s.failed=true;s.visibility(false);s.control.diagnostic(s.error);}
}
void GameOverlay::resized_default_queue(IDXGISwapChain3* chain)noexcept{
    auto& s=*p_;std::lock_guard lock(s.mutex);if(chain!=s.chain_key)return;
    // Plain ResizeBuffers (or ResizeBuffers1 with null Present queues) restores
    // the creation queue; retaining a previous explicit queue causes device loss.
    DXGI_SWAP_CHAIN_DESC1 desc{};std::array<ID3D12CommandQueue*,8> queues{};
    if(FAILED(chain->GetDesc1(&desc))||desc.BufferCount>queues.size()){resized_queues(chain,{});return;}
    queues.fill(s.creation_queue.Get());resized_queues(chain,{queues.data(),desc.BufferCount});
}
bool GameOverlay::bound()const noexcept{return p_->bound.load();}
void GameOverlay::show()noexcept{auto& s=*p_;if(s.stopped||s.failed||!s.bound)return;try{s.visibility(true);}catch(...){}}
void GameOverlay::stop()noexcept{auto& s=*p_;if(s.stopped.exchange(true))return;s.bound=false;s.visibility(false);s.preferences.close();std::lock_guard lock(s.mutex);
    if(s.window&&IsWindow(s.window)){
        if(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(s.window,GWLP_WNDPROC))==&Impl::procedure)SetWindowLongPtrW(s.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(s.original_proc));
        else s.input_hook_retained=true;
    }
    if(!s.input_hook_retained){auto* expected=&s;Impl::input_owner.compare_exchange_strong(expected,nullptr);}
    try{s.release_gpu();}catch(...){s.completion_unknown=true;} // Preserve allocations if GPU completion is unknown.
}
json GameOverlay::snapshot()const{const auto& s=*p_;std::lock_guard lock(s.mutex);return {{"attached",s.chain_key!=nullptr},{"visible",s.visible.load()},{"draws",s.draws},{"skipped",s.skipped},
    {"input_block_installed",s.input_block_installed},{"input_block_active",input_block::active()},
    {"color_space",unsigned(s.space)},{"failed",s.failed.load()},{"error",s.error},{"controls",s.controls},{"hidden_draws",0},{"automatic_sampling",false},
    {"bound",s.bound.load()},{"attaches",s.attaches},{"detaches",s.detaches},{"rebinds",s.rebinds},{"reshapes",s.reshapes},
    {"back_buffers","acquired per present, released after recording; none held between presents"},
#ifdef LAB_OVERLAY_RESEARCH
    {"capture_hidden",s.capture_hidden.load()},{"capture_override",s.capture_override.load()},{"hotkey",s.hotkey.load()}};}
#else
    {"hotkey",s.hotkey.load()}};}
#endif
}
