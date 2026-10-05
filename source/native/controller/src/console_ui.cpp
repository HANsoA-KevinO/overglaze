// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_console_ui.hpp"
#include "lab_pipe.hpp"
#include "lab_nr_panel.hpp"
#include "lab_nr_settings.hpp"
#include "lab_root_locator.hpp"
#include <algorithm>
#include <charconv>
#include <shellapi.h>

namespace lab {
namespace {
constexpr ImU32 bg=IM_COL32(14,17,20,255), surface=IM_COL32(32,37,43,255);
constexpr ImU32 panel=IM_COL32(23,28,34,255), rail=IM_COL32(20,25,30,255);
constexpr ImU32 edge=IM_COL32(49,54,60,255), white=IM_COL32(235,237,240,255);
constexpr ImU32 muted=IM_COL32(158,164,173,255), dim=IM_COL32(111,118,127,255);
constexpr ImU32 green=IM_COL32(118,185,0,255), amber=IM_COL32(210,174,113,255);
ImVec4 color(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
struct Canvas {
    ConsoleView& v;
    float s;
    // Custom drawing shares the scrolling origin with the widgets.
    ImVec2 origin={ImGui::GetWindowPos().x-ImGui::GetScrollX(),ImGui::GetWindowPos().y-ImGui::GetScrollY()};
    ImDrawList* d=ImGui::GetWindowDrawList();
    ImVec2 p(float x,float y) const { return {origin.x+x*s,origin.y+y*s}; }
    void at(float x,float y) { ImGui::SetCursorPos({x*s,y*s}); }
    void box(float x,float y,float w,float h,ImU32 c=surface,float r=5) {
        d->AddRectFilled(p(x,y),p(x+w,y+h),c,r*s);
    }
    void line(float x,float y,float w,ImU32 c=edge) { d->AddLine(p(x,y),p(x+w,y),c,s); }
    void text(float x,float y,const std::string& value,float size=13,ImU32 c=white,ImFont* font=nullptr,float wrap=0) {
        at(x,y); font=font?font:v.body;
        ImGui::PushFont(font,size*((font==v.body||font==v.bold)?1.2f:1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,color(c));
        if(wrap) ImGui::PushTextWrapPos((x+wrap)*s);
        ImGui::TextUnformatted(value.c_str());
        if(wrap) ImGui::PopTextWrapPos();
        ImGui::PopStyleColor(); ImGui::PopFont();
    }
    void fit(float x,float y,std::string value,float w,float size=13,ImU32 c=white,ImFont* font=nullptr) {
        font=font?font:v.body;
        ImGui::PushFont(font,size*((font==v.body||font==v.bold)?1.2f:1.f));
        if(ImGui::CalcTextSize(value.c_str()).x>w*s) {
            while(!value.empty() && ImGui::CalcTextSize((value+"...").c_str()).x>w*s) {
                auto last=value.size()-1;
                while(last && (static_cast<unsigned char>(value[last])&0xc0)==0x80) --last;
                value.resize(last);
            }
            value+="...";
        }
        ImGui::PopFont(); text(x,y,value,size,c,font);
    }
    void caps(float x,float y,const char* value,ImU32 c=white,float size=12) {
        ImGui::PushFont(v.display,size);
        for(const char* a=value;*a;++a) {
            char letter[2]={*a,0};
            d->AddText(ImGui::GetFont(),ImGui::GetFontSize(),p(x,y),c,letter);
            x+=ImGui::CalcTextSize(letter).x/s+1.55f;
        }
        ImGui::PopFont();
    }
    void record(const char* id) {
        auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        v.controls[id]={a.x,a.y,b.x-a.x,b.y-a.y};
        const auto wp=ImGui::GetWindowPos(),ws=ImGui::GetWindowSize();
        v.control_clips[id]={wp.x,wp.y,ws.x,ws.y};
    }
    bool hit(const char* id,float x,float y,float w,float h,bool enabled=true) {
        at(x,y); ImGui::BeginDisabled(!enabled);
        ImGui::PushStyleColor(ImGuiCol_Button,{0,0,0,0});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,{0,0,0,0});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,{0,0,0,0});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
        const bool result=ImGui::Button((std::string("##")+id).c_str(),{w*s,h*s});
        ImGui::PopStyleVar(); ImGui::PopStyleColor(3); record(id); ImGui::EndDisabled();
        return result;
    }
    void focus(float x,float y,float w,float h) {
        if(ImGui::GetIO().NavVisible && ImGui::IsItemFocused()) d->AddRect(p(x-2,y-2),p(x+w+2,y+h+2),green,5*s);
    }
    void hint(const char* value) {
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12*s,10*s});
            ImGui::BeginTooltip(); ImGui::PushTextWrapPos(270*s); ImGui::TextUnformatted(value);
            ImGui::PopTextWrapPos(); ImGui::EndTooltip(); ImGui::PopStyleVar();
        }
    }
    bool button(const char* id,float x,float y,float w,const char* label,bool selected=false,bool enabled=true,float h=36) {
        const bool result=hit(id,x,y,w,h,enabled);
        const bool hover=enabled&&ImGui::IsItemHovered();
        box(x,y,w,h,selected?IM_COL32(43,55,24,255):hover?IM_COL32(49,54,59,255):surface);
        d->AddRect(p(x,y),p(x+w,y+h),selected?IM_COL32(100,144,21,255):edge,5*s);
        focus(x,y,w,h);
        ImGui::PushFont(v.body,15.6f); const float tw=ImGui::CalcTextSize(label).x/s; ImGui::PopFont();
        text(x+(w-tw)/2,y+(h-17)/2-1,label,13,enabled?(selected?green:white):dim);
        return result;
    }
    bool check(const char* id,float x,float y,const char* label,bool checked,bool enabled,float width=215,bool unknown=false) {
        const bool result=hit(id,x,y,width,27,enabled); focus(x,y,width,27);
        box(x,y+4,16,16,checked?green:IM_COL32(28,33,39,255),3);
        d->AddRect(p(x,y+4),p(x+16,y+20),checked?green:dim,3*s);
        if(checked) {
            d->AddLine(p(x+3.5f,y+12),p(x+7,y+15.5f),bg,2*s);
            d->AddLine(p(x+7,y+15.5f),p(x+13,y+8),bg,2*s);
        } else if(unknown) line(x+4,y+12,8,muted);
        text(x+28,y+2,label,13,enabled?white:muted,v.bold);
        return result;
    }
    bool tab(const char* id,float x,float y,float w,const char* label,bool active,float h=40) {
        const bool clicked=hit(id,x,y,w,h); focus(x,y,w,h);
        if(ImGui::IsItemHovered()) box(x,y,w,h,surface,0);
        text(x+9,y+(h-17)/2,label,13,active?white:muted);
        if(active) line(x+9,y+h-1,w-18,green);
        return clicked;
    }

};
std::string state_text(const std::string& state) {
    if(state=="idle") return "待命";
    if(state=="armed") return "已准备";
    if(state=="running") return "运行中";
    if(state=="paused") return "已暂停";
    if(state=="cancelled") return "已取消";
    return "未连接";
}
std::string mode_text(const json& state) {
    const auto mode=state.value("nr_mode",std::string("unknown"));
    if(mode=="off") return "OFF";
    if(mode=="on") return "ON";
    if(mode=="compute-only") return "仅计算（不写回）";
    if(mode=="compute_bypass") return "仅计算";
    return "—";
}
bool synthetic(const ConsoleView& v) {
    return v.connected && v.status.value("origin","")=="synthetic"
        && v.status.value("capabilities",json::object()).value("synthetic_control",false);
}
bool writable(const ConsoleView& v) {
    const auto owner=v.status.value("control_owner",std::string());
    return synthetic(v) && !v.pending && (owner.empty()||owner==v.client_id);
}
bool idle(const ConsoleView& v) {
    const auto state=v.status.value("state","");
    return state=="idle"||state=="cancelled";
}
// The data root this program resolves from its own location (<root>\app\,
// <root>\data\_build*\ or an overglaze-root.json beside it).
std::filesystem::path runs_root(){return lab::root::resolve_self().data;}
void open_runs(ConsoleView& v, const std::string& directory="") {
    try {
        auto root=std::filesystem::weakly_canonical(runs_root());
        auto path=directory.empty()?root:std::filesystem::weakly_canonical(wide(directory));
        auto relative=path.lexically_relative(root);
        if(relative.empty() || relative.native().starts_with(L"..") || !std::filesystem::is_directory(path))
            throw std::runtime_error("Refusing to open a directory outside the Lab run root");
        if(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)
            throw std::runtime_error("Cannot open run directory");
    } catch(const std::exception& e) { v.error=e.what(); }
}
std::string source_text(const ConsoleView& v) {
    if(!v.connected) return "未连接";
    const auto runtime=v.status.value("nr_runtime",json());
    if(runtime.is_object()&&runtime.value("input_origin","")=="ui-test-no-NR")return "界面协议自测 · 不执行 NR / 不保存图像";
    const auto origin=v.status.value("origin","");
    if(origin=="game") return v.status.value("capabilities",json::object()).value("manual_capture",false)?"游戏工作台 · 按需采集":"深度研究观察器 · 只读";
    if(origin=="d3d12-harness") return "D3D12 实测程序 · 非游戏";
    if(origin=="synthetic") return "协议自测 · 不执行 NR";
    if(origin=="synthetic-input-real-nr") return "原版 NR 独立测试 · 非游戏";
    const auto host=v.status.value("host",json());
    if(origin=="game-rr-experimental-nr"||(host.is_object()&&host.value("backend","")=="standalone-d3d12"&&host.value("origin","")=="game"))
        return "2077 · 独立 NR 控制";
    if(host.is_object()&&host.value("backend","")=="standalone-d3d12"&&host.value("origin","")=="synthetic")return "界面协议自测 · 不执行 NR";
    return "空后端 · 未接观察器";
}
void sidebar(Canvas& c,float w,float h) {
    const char* names[]={"工作台","采集","管线取证","诊断"};
    const char* ids[]={"nav.workspace","nav.capture","nav.observer","nav.diagnostics"};
    const int pages[]={0,1,2,3};
    c.caps(10,10,"WORKSPACE",muted,10);
    for(int i=0;i<4;++i) {
        const float y=45.f+i*49;
        if(c.hit(ids[i],0,y,w,42)) c.v.page=pages[i];
        const bool active=c.v.page==pages[i];
        if(active||ImGui::IsItemHovered()) c.box(0,y,w,42,active?IM_COL32(38,53,27,255):surface,3);
        if(active) c.box(0,y+12,2,18,green,0);
        c.focus(0,y,w,42);
        c.text(14,y+12,names[i],14,active?white:muted);
    }
    c.line(0,263,w);
    if(c.button("results.open",0,285,w,"打开实验目录",false,true,34)) open_runs(c.v);
    c.text(10,h-62,"本机 · 离线研究",11,dim);
    c.text(10,h-37,"DEVELOPMENT 0.7",10,dim,c.v.mono);
}
void header(Canvas& c,float width,const std::function<void(const char*,json)>& send) {
    auto& v=c.v;
    c.box(0,0,width,72,panel,0);
    c.caps(22,28,"OVERGLAZE",green,15);
    const auto obs=v.status.value("observer",json());
    std::string target=v.connected?"PID "+std::to_string(v.pid):"尚未选择本机后端";
    if(obs.is_object() && obs.contains("application"))
        target=utf8(std::filesystem::path(wide(obs.value("application",""))).filename().wstring())+"  /  "+target;
    else if(const auto host=v.status.value("host",json());host.is_object()&&host.contains("executable")&&host["executable"].is_object())
        target=utf8(std::filesystem::path(wide(host["executable"].value("path",""))).filename().wstring())+"  /  "+target;
    c.fit(260,17,target,width-540,14,white);
    c.text(260,43,source_text(v),11,synthetic(v)?amber:muted);
    if(v.connected) {
        if(c.button("session.refresh",width-217,23,92,v.pending?"请求中…":"刷新状态",false,!v.pending,30)) send("GetStatus",json::object());
        if(c.button("session.disconnect",width-116,23,92,"断开",false,!v.pending,30)) {
            v.automatic_connection=false;
            v.connected=false; v.status=json::object(); v.scene_confirmed=false; v.nr_scene_confirmed=false; v.error.clear();
        }
    }
    c.line(0,72,width);
}
bool nr_workspace(Canvas& c,float w,const std::function<void(const char*,json)>& send){
    auto& v=c.v;const auto state=nr_panel(v.status,v.connected,v.pending,v.client_id,v.nr_scene_confirmed);
    if(!state.present)return false;
    const auto nr=v.status.value("nr_frame_control",json());
    const auto runtime=v.status.value("nr_runtime",json());
    if(runtime.is_object()&&runtime.value("rebuild_serial",0ULL)!=v.nr_rebuild_serial){
        v.nr_rebuild_serial=runtime.value("rebuild_serial",0ULL);v.nr_settings_dirty=false;v.nr_settings_revision=UINT64_MAX;
    }
    if(!state.ready&&!state.automatic)c.caps(0,307,"NEURAL RENDERING",muted,11);
    if(state.failed){
        c.text(0,341,"NR 暂不可用",25,amber,v.bold);
        c.text(0,389,state.error.empty()?"后端已停止，未调度新的 NR 工作。":state.error,13,amber,nullptr,w);
        const auto y=std::max(439.f,ImGui::GetCursorPosY()/c.s+25);
        if(c.button("nr.diagnostics",0,y,150,"查看诊断"))v.page=3;
        c.text(0,y+63,"错误已保留。不会自动重试、重启游戏或把未知状态显示成 NR OFF。",12,muted,nullptr,w);
        c.at(0,y+115);ImGui::Dummy({1,1});return true;
    }
    if(state.automatic&&!state.ready){
        c.caps(0,117,"NEURAL RENDERING",muted,11);
        const bool wanted=state.requested=="on";
        c.text(0,148,state.rebuilding?"NR 暂时旁路":wanted?"NR 准备中":"NR OFF · 待命",27,white,v.bold);
        if(c.button("nr.live.off",w-268,142,126,"关闭 NR",!wanted,state.can_toggle,42))send("SetNrMode",{{"mode","off"}});
        if(c.button("nr.live.on",w-132,142,132,"开启 NR",wanted,state.can_toggle,42))send("SetNrMode",{{"mode","on"}});
        const auto phase=runtime.is_object()?runtime.value("state",std::string()):"";
        c.text(0,215,state.rebuilding?(wanted?"资源正在重新就绪，完成后恢复你选择的开启状态。":"已取消自动恢复，重新就绪后保持关闭。"):
            wanted?"正在等待完整游戏输入并准备 NR，无需额外操作。":"游戏已连接。开启 NR 时才准备模型，不自动运行或采样。",13,muted,nullptr,w);
        c.line(0,283,w);
        c.text(0,316,phase=="draining"?"等待上一份 GPU 工作安全完成":phase=="preparing"?"正在初始化模型资源":wanted?"等待可用输入":"按需使用",16,white,v.bold);
        c.text(0,361,"你可以随时关闭以取消开启请求。只有经过验证的输入才会交给 NR。",13,muted,nullptr,w);
        if(state.other_owner)c.text(0,405,"另一个控制端正在使用，当前只读。",12,amber,nullptr,w);
        if(!v.error.empty())c.text(0,449,v.error,12,amber,nullptr,w);
        c.at(0,500);ImGui::Dummy({1,1});return true;
    }
    if(!state.ready){
        if(state.rebuilding){
            const bool gap=runtime.is_object()&&runtime.value("rebuild_reason","")=="frame-continuity-interrupted";
            c.text(0,341,gap?"帧序列中断 · 正在重新准备":"画面尺寸变化 · 正在重新准备",25,white,v.bold);
            const auto phase=runtime.is_object()?runtime.value("state",std::string()):"";
            c.text(0,389,phase=="draining"?"NR 已暂停，等待旧 GPU 工作安全结束。":"正在确认新资源与队列，并重建 NR Feature。",13,muted,nullptr,w);
            c.text(0,448,"参数请求值会保留。准备完成后保持关闭，请重新点击「开启 NR」。",13,muted,nullptr,w);
            c.text(0,500,"不会自动采图；重建不等于已验证模型内部的历史重置语义。",12,dim,nullptr,w);
            c.at(0,560);ImGui::Dummy({1,1});return true;
        }
        c.text(0,341,state.preparing?"正在准备 NR":state.confirmable?"尚未准备 NR":"等待后端反馈",25,white,v.bold);
        c.text(0,389,"进到实际存档后再准备。准备只检查接入并初始化，不会开启效果或采样。",13,muted,nullptr,w);
        if(state.confirmable){
            if(c.check("nr.scene.confirmed",0,439,"我已进入实际存档，不是播片或菜单",v.nr_scene_confirmed,!v.pending&&!state.other_owner,w))v.nr_scene_confirmed=!v.nr_scene_confirmed;
            // Recompute after the checkbox click; the backend independently validates the request.
            const auto current=nr_panel(v.status,v.connected,v.pending,v.client_id,v.nr_scene_confirmed);
            if(c.button("nr.prepare.start",0,491,150,"准备 NR",current.can_prepare,current.can_prepare,42))send("PrepareNr",{{"scene_confirmed",true}});
            c.hint("先确认已进入存档。人工确认不是程序自动识别；准备不会开启 NR。");
            c.text(0,560,state.other_owner?"另一个控制端正在使用此后端，请等待它断开。":"启动期间不会自动准备，也不会创建采样日志。",12,state.other_owner?amber:dim,nullptr,w);
        }else{
            const auto phase=runtime.is_object()?runtime.value("state",std::string()):"";
            const char* detail=phase=="probing-queue"?"正在确认游戏提交队列":phase=="preparing"?"正在初始化原版 NR":"等待下一次完整、匹配的游戏帧";
            c.text(0,448,detail,14,muted,nullptr,w);
            c.text(0,500,"准备完成后才开放开关；目前不会执行 NR。",12,dim,nullptr,w);
        }
        c.at(0,607);ImGui::Dummy({1,1});return true;
    }
    const bool game=v.status.value("origin",std::string())=="game-rr-experimental-nr";
    c.caps(0,117,"NEURAL RENDERING",muted,11);
    c.text(0,148,state.observed=="on"?"NR ON":state.observed=="off"?"NR OFF":"已准备",27,state.observed=="on"?green:white,v.bold);
    if(c.button("nr.live.off",w-268,142,126,"关闭 NR",(state.automatic?state.requested:state.observed)=="off",state.can_toggle,42))send("SetNrMode",{{"mode","off"}});
    if(c.button("nr.live.on",w-132,142,132,"开启 NR",(state.automatic?state.requested:state.observed)=="on",state.can_toggle,42))send("SetNrMode",{{"mode","on"}});
    const bool rebuilt=runtime.is_object()&&runtime.value("rebuild_serial",0ULL)>0;
    c.text(0,203,rebuilt&&!state.automatic&&state.observed!="on"?"NR 已重新准备，保持关闭。开启后重新确认参数实际值。":game?"在下一次可用游戏帧生效，无需重启。":"合成测试后端 · 仅验证当前测试，不控制 2077。",13,muted,nullptr,w);
    if(state.other_owner)c.text(0,239,"另一个控制端正在使用，当前只读。",12,amber,nullptr,w);
    else if(state.phase=="requested"||state.phase=="applying")c.text(0,239,"请求已送出 · 等待对应帧的实际执行回执",12,muted,nullptr,w);
    else if(nr.contains("applied_frame")&&!nr["applied_frame"].is_null())c.text(0,239,"执行回执 · 帧 "+nr["applied_frame"].dump()+" / 配置 "+nr["applied_revision"].dump(),12,muted,nullptr,w);
    const auto caps=v.status.value("capabilities",json::object());float y=286;
    if(caps.value("nr_settings",false)&&runtime.is_object()){
        c.line(0,y,w);y+=30;c.text(0,y,"模型参数",16,white,v.bold);c.text(w-238,y+3,"松开滑块后应用 · 实验范围 0–1",11,dim);y+=45;
        const auto settings=runtime.value("settings",json::object());const auto revision=settings.value("requested_revision",0ULL);
        const auto session=v.status.value("session_id",std::string());
        if(session!=v.nr_settings_session){v.nr_settings_session=session;v.nr_settings_revision=UINT64_MAX;v.nr_settings_dirty=false;}
        if(v.nr_settings_revision!=revision&&!v.nr_settings_dirty&&!ImGui::IsAnyItemActive()){
            const auto requested=settings.value("requested",json::object());v.nr_tone=requested.value("tone",1.f);v.nr_structure=requested.value("structure",1.f);v.nr_settings_revision=revision;}
        const bool editable=state.can_toggle&&state.observed=="on"&&(state.phase=="idle"||state.phase=="applied");
        const auto observed=settings.value("observed",json());
        for(unsigned i=0;i<2;++i){const char* id=i?"parameter.structure":"parameter.tone";const char* key=i?"structure":"tone";
            c.text(0,y+6,i?"Structure · 结构":"Tone · 色调",13,muted);c.at(174,y);ImGui::SetNextItemWidth((w-315)*c.s);ImGui::BeginDisabled(!editable);
            ImGui::PushFont(v.mono,15);if(ImGui::SliderFloat((std::string("##")+id).c_str(),i?&v.nr_structure:&v.nr_tone,0.f,lab::nr::Settings::max_tone_structure,"%.2f",ImGuiSliderFlags_AlwaysClamp))v.nr_settings_dirty=true;
            c.record(id);ImGui::PopFont();ImGui::EndDisabled();
            c.text(w-122,y+6,observed.is_object()?"实际 "+observed.value(key,json()).dump():"实际值待确认",11,dim);y+=54;
        }
        if(v.nr_settings_dirty&&editable&&!ImGui::IsAnyItemActive()){send("SetNrSettings",{{"tone",v.nr_tone},{"structure",v.nr_structure}});v.nr_settings_dirty=false;}
        c.text(0,y,state.observed!="on"?"开启 NR 后才能调节并获得实际读取回执。":"实际值来自本次 Evaluate 的 DLL 读取；不表示已确认模型内部作用位置。",11,dim,nullptr,w);y+=42;
    }
    c.line(0,y,w);y+=28;
    if(caps.value("capture_pair",false)){
        c.text(0,y,"同帧画面",16,white,v.bold);y+=37;
        c.text(0,y,"保存 RR 原图、NR 输入、NR 原始输出与 HDR 回填。不会自动连续采集。",12,muted,nullptr,w);y+=42;
        if(c.button("nr.capture.page",0,y,150,"打开画面采集",false,!v.pending,36))v.page=1;
        c.text(168,y+9,"FP16 原始数据 · 不是显示器截图或独立 OFF 性能基线",11,dim,nullptr,w-168);y+=60;
    }
    c.text(0,y,"Mask、模型切换与完整 P0 取证尚未开放；不提供未验证的旋钮。",11,dim,nullptr,w);
    if(!v.error.empty()){y+=35;c.text(0,y,v.error,12,amber,nullptr,w);}
    c.at(0,y+55);ImGui::Dummy({1,1});return true;
}
void workspace(Canvas& c,float w,const std::function<void(const char*,json)>& send) {
    auto& v=c.v;
    c.text(0,0,"Overglaze NR 工作台",22,white,v.bold);
    const auto nr_state=nr_panel(v.status,v.connected,v.pending,v.client_id,v.nr_scene_confirmed);
    c.text(0,47,(nr_state.ready||nr_state.automatic)?"游戏已连接。这里控制效果，采集页面保存画面，诊断页面查看接入状态。":v.automatic_connection?"自动连接已安装插件的游戏。NR 默认关闭，不会自动采样。":"选择已启动的 Lab 后端。连接不会启动游戏、安装插件或改变 NR。",13,muted,nullptr,w);
    c.line(0,91,w);
    if(nr_state.ready||nr_state.automatic){nr_workspace(c,w,send);return;}
    if(v.automatic_connection&&!v.connected&&v.sessions.size()<2){
        c.text(0,139,"等待游戏启动",27,white,v.bold);
        c.text(0,200,"从 Steam 或游戏快捷方式正常启动已安装插件的游戏即可。",14,muted,nullptr,w);
        c.text(0,246,"不需要填写进程号或手动准备 NR。插件连接后，这里会显示开关。",13,muted,nullptr,w);
        c.text(0,310,"未适配的游戏不会自动注入。",12,dim,nullptr,w);c.at(0,370);ImGui::Dummy({1,1});return;
    }
    c.text(0,114,"后端进程 PID",13,white,v.bold);
    if(c.button("session.discover",w-104,106,104,"发现后端",false,!v.pending,34)) {
        try { v.sessions=discover_lab_processes(); v.error.clear(); }
        catch(const std::exception& e) { v.error=e.what(); }
        ImGui::OpenPopup("local-sessions");
    }
    ImGui::SetNextWindowPos(c.p(0,201),ImGuiCond_Appearing);
    ImGui::SetNextWindowSize({w*c.s,0},ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{16*c.s,14*c.s});
    v.discovery_open=ImGui::BeginPopup("local-sessions");
    if(v.discovery_open) {
        if(v.sessions.empty()) {
            ImGui::TextUnformatted("未发现运行中的 Lab 后端");
            ImGui::TextDisabled("普通游戏 PID 不能代替 Lab 后端。");
        }
        for(DWORD pid:v.sessions) {
            ImGui::BeginDisabled(v.pending);
            if(ImGui::Selectable(("Lab 后端 / PID "+std::to_string(pid)).c_str())) {
                v.pid=static_cast<int>(pid); snprintf(v.pid_text,sizeof(v.pid_text),"%lu",pid);
                v.status=json::object(); v.connected=false; v.scene_confirmed=false; v.nr_scene_confirmed=false;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    c.at(0,155); ImGui::PushFont(v.mono,16); ImGui::BeginDisabled(v.pending);
    ImGui::SetNextItemWidth((w-119)*c.s);
    if(ImGui::InputTextWithHint("##pid","PID",v.pid_text,sizeof(v.pid_text),ImGuiInputTextFlags_CharsDecimal)) {
        v.pid=0; std::from_chars(v.pid_text,v.pid_text+strlen(v.pid_text),v.pid);
        v.connected=false; v.status=json::object(); v.error.clear(); v.scene_confirmed=false; v.nr_scene_confirmed=false;
    }
    c.record("session.pid"); ImGui::EndDisabled(); ImGui::PopFont();
    if(c.button("session.connect",w-104,155,104,v.pending?"连接中…":v.connected?"重新握手":"连接",
        v.pid>0&&!v.pending,v.pid>0&&!v.pending,36)) send("Hello",json::object());
    if(!v.error.empty()) c.text(0,211,v.error,12,amber,nullptr,w);
    else c.text(0,211,v.connected?(nr_panel(v.status,true,v.pending,v.client_id,v.nr_scene_confirmed).present?"已连接。下方准备与开关按当前后端的实际能力开放。":"已连接。采集操作在左侧「采集」，底层研究记录在「管线取证」。"):"连接新版工作台后保持待机，不随游戏启动持续写入事件日志。",12,muted,nullptr,w);
    c.line(0,281,w);
    if(nr_workspace(c,w,send))return;
    c.caps(0,307,"AVAILABLE NOW",muted,11);
    c.text(0,343,"按需采集 · 不重启游戏",16,white,v.bold);
    c.text(0,378,"新版工作台提供可重复开始 / 停止的 CPU 回调节奏记录。未点击开始时，不创建采集文件。",13,muted,nullptr,w);
    c.text(0,432,"NR 控制与画面采集",16,white,v.bold);
    c.text(0,467,"游戏内 NR 开关、参数与同帧图像还未接通。独立 NR 调用成功不等于这些功能可用；不会用占位滑杆代替真实控制。",13,muted,nullptr,w);
    c.at(0,540); ImGui::Dummy({1,1});
}
void capture_view(Canvas& c,float w,const std::function<void(const char*,json)>& send) {
    auto& v=c.v;const auto cap=v.status.value("capture",json());
    if(v.preview_open){
        c.text(0,0,"同帧对照",22,white,v.bold);
        if(c.button("preview.back",w-120,0,120,"返回采集",false,true,34))v.preview_open=false;
        const auto provenance=v.preview_manifest.value("provenance",json::object());
        const bool synthetic=provenance.value("origin",std::string()).starts_with("synthetic");
        c.text(0,47,std::string(synthetic?"合成测试数据 · ":"采集数据 · ")+"帧 "+v.preview_manifest.value("frame",json()).dump()+" / 调用 "+v.preview_manifest.value("call",json()).dump()+" · 四份原始文件的 SHA-256 已校验",12,synthetic?amber:muted,nullptr,w);
        c.line(0,90,w);
        if(c.button("preview.nr",0,113,210,"NR 输入 / 原始输出",v.preview_pair==0))v.preview_pair=0;
        if(c.button("preview.hdr",223,113,210,"RR 原图 / HDR 回填",v.preview_pair==1))v.preview_pair=1;
        c.text(0,174,v.preview_pair==0?"sRGB 编码值预览 · 超范围值仅在预览中裁切，原件不变。":"两侧使用相同的 x/(1+x) + sRGB 显示压缩；这不是显示器 HDR 回放。",12,muted,nullptr,w);
        const float side=(w-20)/2,ih=side*v.preview_height/std::max(1u,v.preview_width);
        const float span=1.f/v.preview_zoom;const float u=std::clamp(v.preview_x-span/2,0.f,1.f-span),t=std::clamp(v.preview_y-span/2,0.f,1.f-span);
        const unsigned left=v.preview_pair?0:1,right=v.preview_pair?3:2;
        c.text(0,222,v.preview_pair?"01  RR 原图":"02  实际 NR 输入",13,white);c.text(side+20,222,v.preview_pair?"04  HDR 回填":"03  NR 原始输出",13,white);
        c.at(0,255);ImGui::Image(ImTextureRef(v.preview_textures[left]),{side*c.s,ih*c.s},{u,t},{u+span,t+span});c.record("preview.left");
        c.at(side+20,255);ImGui::Image(ImTextureRef(v.preview_textures[right]),{side*c.s,ih*c.s},{u,t},{u+span,t+span});c.record("preview.right");
        float y=255+ih+27;c.text(0,y+7,"同步放大",12,muted);c.at(100,y);ImGui::SetNextItemWidth((w-100)*c.s);ImGui::SliderFloat("##preview.zoom",&v.preview_zoom,1,8,"%.1f x");c.record("preview.zoom");y+=47;
        c.text(0,y+7,"观察位置",12,muted);c.at(100,y);ImGui::SetNextItemWidth(((w-120)/2)*c.s);ImGui::SliderFloat("##preview.x",&v.preview_x,0,1,"X %.2f");c.record("preview.x");
        c.at(110+(w-120)/2,y);ImGui::SetNextItemWidth(((w-120)/2)*c.s);ImGui::SliderFloat("##preview.y",&v.preview_y,0,1,"Y %.2f");c.record("preview.y");y+=58;
        c.text(0,y,"原始 "+std::to_string(v.preview_original_width)+" × "+std::to_string(v.preview_original_height)+" / 预览 "+std::to_string(v.preview_width)+" × "+std::to_string(v.preview_height)+" · 放大的是预览；紫色表示非有限数值。",11,dim,nullptr,w);y+=34;
        c.fit(0,y,v.preview_source,w,11,dim);c.at(0,y+55);ImGui::Dummy({1,1});return;
    }
    const auto caps=v.status.value("capabilities",json::object());
    if(v.connected&&caps.value("capture_pair",false)){
        const auto pair=v.status.value("frame_pair",json::object());const auto phase=pair.value("state","");
        const auto np=nr_panel(v.status,v.connected,v.pending,v.client_id,v.nr_scene_confirmed);
        const bool busy=phase=="requested"||phase=="preparing"||phase=="armed"||phase=="recording"||phase=="pending-gpu"||phase=="writing"||phase=="cancelling";
        c.text(0,0,"画面采集",22,white,v.bold);
        c.text(0,47,"只在你请求时保存下一组同帧数据。开关和参数仍在工作台，采集不会替你开启 NR。",13,muted,nullptr,w);c.line(0,91,w);
        c.caps(0,119,"SAME CALL / FOUR STAGES",muted,11);
        const char* label=phase=="complete"?"已保存一组同帧画面":phase=="failed"?"本次采集失败":phase=="writing"?"正在保存原始纹理":phase=="pending-gpu"?"等待 GPU 完成":busy?"已请求 · 等待下一帧":phase=="cancelled"?"已取消":"待机 · 不采集画面";
        c.text(0,156,label,25,phase=="complete"?green:white,v.bold);
        const char* labels[]{"01  RR 原图","02  NR 输入","03  NR 原始输出","04  HDR 回填"};
        const char* descriptions[]{"颜色转换前","真正送入模型的纹理","没有亮度／颜色重建","采用旁路 HDR 重建后"};
        for(unsigned i=0;i<4;++i){const float y=224+i*56.f;c.text(0,y,labels[i],14,white);c.text(212,y+2,descriptions[i],12,muted);c.line(0,y+39,w);}
        const auto runtime=v.status.value("nr_runtime",json::object());const auto bytes=runtime.value("width",0ULL)*runtime.value("height",0ULL)*32;
        c.text(0,469,"本次约 "+std::to_string(bytes/1048576)+" MiB · 单次运行上限 20 GiB · 至少保留 30 GiB 空闲",12,dim,nullptr,w);
        if(c.button("capture.pair",0,517,156,"保存下一组同帧",false,np.can_toggle&&np.observed=="on"&&!busy,40))send("CapturePair",json::object());
        if(c.button("capture.pair.cancel",170,517,110,"取消",false,!v.pending&&!np.other_owner&&busy,40))send("CancelCapturePair",json::object());
        c.text(300,529,np.observed!="on"?"请先在工作台开启 NR。":"保存无损 FP16，PNG 预览不代替原始证据。",11,muted,nullptr,w-300);
        float y=590;
        if(phase=="complete"){
            c.text(0,y,"实际帧 "+pair.value("frame",json()).dump()+" / 调用 "+pair.value("call",json()).dump()+" / "+pair.value("files",json()).dump()+" 份原始纹理",13,white);y+=37;
            const auto manifest=pair.value("manifest",std::string());
            if(c.button("capture.pair.preview",0,y,156,v.preview_loading?"读取并校验中…":"查看同帧对照",true,!manifest.empty()&&!v.preview_loading,36))send("LoadFramePair",json::object());
            if(c.button("capture.pair.open",170,y,140,"打开原始数据",false,!manifest.empty(),36))open_runs(v,utf8(std::filesystem::path(wide(manifest)).parent_path().wstring()));
            c.fit(330,y+8,manifest,w-330,11,dim);y+=62;
        }
        if(!pair.value("error",std::string()).empty()){c.text(0,y,pair.value("error",std::string()),12,amber,nullptr,w);y=ImGui::GetCursorPosY()/c.s+27;}
        if(!v.preview_error.empty()){c.text(0,y,v.preview_error,12,amber,nullptr,w);y=ImGui::GetCursorPosY()/c.s+27;}
        c.text(0,y,"这四份属于同一次 NR 调用。没有捕获最终 Present，不是独立 OFF 基线，也不是完整历史回放包。",12,dim,nullptr,w);
        if(!v.error.empty()){y=ImGui::GetCursorPosY()/c.s+20;c.text(0,y,v.error,12,amber,nullptr,w);}
        c.at(0,y+60);ImGui::Dummy({1,1});return;
    }
    const bool supported=v.connected && v.status.value("capabilities",json::object()).value("manual_capture",false) && cap.is_object();
    c.text(0,0,"采集",22,white,v.bold);
    c.text(0,47,"需要记录时再开始。停止后保留结果，可以在同一游戏进程中再次采集。",13,muted,nullptr,w);
    if(!supported){c.line(0,90,w);c.text(0,119,"当前后端没有手动采集能力",16,white,v.bold);
        c.text(0,163,"请连接新版工作台。旧研究观察器仍是一轮一次的独立入口，不能把它的选样确认当作开始 / 停止。",13,muted,nullptr,w);
        if(c.button("capture.connect",0,230,140,"前往工作台",true))v.page=0;return;}
    const auto state=cap.value("state","");const bool busy=state=="recording" || state=="starting" || state=="stopping";
    const auto owner=v.status.value("control_owner",std::string());const bool writable=!v.pending && (owner.empty() || owner==v.client_id);
    c.line(0,90,w);c.caps(0,116,"CPU CALLBACK CADENCE",muted,11);
    const char* label=state=="recording"?"正在记录":state=="starting"?"正在准备":state=="stopping"?"正在收尾":state=="completed"?"已完成":state=="failed"?"采集失败":"待机 · 不写事件日志";
    c.text(0,149,label,24,state=="recording"?green:white,v.bold);
    c.text(0,196,"只测 CPU 呈现回调间隔；不是 GPU 耗时、屏幕实际帧率或完整 P0 证据。",12,muted,nullptr,w);
    c.text(0,246,"记录上限（秒）",13,white);c.at(175,237);ImGui::SetNextItemWidth(126*c.s);ImGui::BeginDisabled(busy);
    ImGui::InputInt("##capture-seconds",&v.capture_seconds,5,10);c.record("capture.seconds");
    v.capture_seconds=std::clamp(v.capture_seconds,1,120);ImGui::EndDisabled();
    c.text(324,246,"最多 120 秒 / 16 MiB / 65,536 次回调",12,dim);
    c.text(0,297,"本次目的",13,white);c.at(175,286);ImGui::SetNextItemWidth((w-175)*c.s);ImGui::BeginDisabled(busy);
    ImGui::InputText("##capture-question",v.capture_question,sizeof(v.capture_question));c.record("capture.question");ImGui::EndDisabled();
    if(c.check("capture.functional",0,339,"功能验证（验收后清理临时原件）",v.capture_functional,!busy,w))v.capture_functional=!v.capture_functional;
    c.text(0,377,v.capture_functional?"功能验证：保留验收摘要和必要诊断；不会把测试计数当作游戏结论。":"研究记录：保留原件与分析结果；不会因 P0 未通过而自动删除。",12,dim,nullptr,w);
    if(c.button("capture.start",0,429,150,"开始采集",!busy,writable&&cap.value("can_start",false)&&v.capture_question[0],40))
        send("StartCapture",{{"kind","present_callback_cadence"},{"duration_seconds",static_cast<unsigned>(v.capture_seconds)},
            {"purpose",v.capture_functional?"functional-verification":"research-evidence"},{"question",v.capture_question}});
    if(c.button("capture.stop",164,429,120,"停止采集",busy,writable&&busy&&state!="stopping",40))send("StopCapture",json::object());
    if(!v.error.empty())c.text(306,441,v.error,12,amber,nullptr,w-306);
    else c.text(306,441,busy?"断开控制端后，租约到期会停止记录。":"开始 / 停止只控制测量，不改变 NR。",12,muted,nullptr,w-306);
    c.line(0,505,w);
    const auto result=cap.value("last_result",json());
    if(result.is_object()){
        c.text(0,532,"最近一次结果",16,white,v.bold);
        c.text(0,576,"回调次数   "+result.value("recorded_callbacks",json()).dump(),14,white);
        c.text(w/2,576,"记录完整性   "+result.value("recording_integrity","unknown"),14,white);
        const auto stats=result.value("cpu_callback_interval_ms",json::object());
        c.text(0,619,"CPU 间隔中位数 / P95 / P99（ms）： "+stats.value("median",json()).dump()+" / "+stats.value("p95",json()).dump()+" / "+stats.value("p99",json()).dump(),12,muted,nullptr,w);
        if(c.button("capture.open",0,668,164,"打开结果与原始记录",false,true,35))open_runs(v,result.value("directory",""));
        c.fit(183,678,result.value("directory",""),w-183,11,dim);
        c.at(0,730);ImGui::Dummy({1,1});
    }else {c.text(0,532,busy?"已记录回调："+cap.value("recorded_callbacks",json()).dump():"尚无采集结果",14,muted);
        c.text(0,575,"画面捕获、NR 参数扫描和深度取证不包含在这项轻量记录中。",12,dim,nullptr,w);}
}
void observer_view(Canvas& c,float w) {
    auto& v=c.v;
    const auto obs=v.status.value("observer",json());
    c.text(0,0,"管线取证 · 研究模式",22,white,v.bold);
    if(!v.connected || !obs.is_object()) {
        c.text(0,52,v.connected?"这个后端没有提供观察数据。":"先连接观察器，再查看实际事件。",14,muted);
        c.text(0,90,"不会显示模拟计数、空图像对照框或未经采集的帧时间。",12,dim);
        if(c.button("observer.connect",0,139,124,"前往连接",true)) v.page=0;
        return;
    }
    c.text(0,47,obs.value("recording",false)?"正在记录 CPU 队列调用":"记录已停止 · 保留最后一次数据",13,muted);
    const auto age=GetTickCount64()>=v.received_ms?GetTickCount64()-v.received_ms:0;
    c.text(w-170,49,"响应距今 "+std::to_string(age/1000)+" 秒",11,age>3000?amber:dim);
    c.line(0,84,w);
    const char* labels[]={"提交调用","Signal","Wait","已写入事件","丢失事件"};
    const char* keys[]={"submit_calls","signal_calls","wait_calls","written","dropped"};
    for(int i=0;i<5;++i) {
        const float x=i*w/5;
        c.text(x,107,labels[i],12,muted);
        c.text(x,137,obs.contains(keys[i])?obs[keys[i]].dump():"未报告",23,i==4&&obs.value("dropped",0ULL)?amber:white,c.v.mono);
    }
    c.text(0,188,"这是 CPU 观察结果，不是 GPU 耗时，也不能据此确认 RR / NR 顺序。",12,dim,nullptr,w);
    c.line(0,225,w);
    c.text(0,249,"最近写入的事件",15,white,v.bold);
    c.text(w-215,253,"最多 24 条 · 完整记录在文件中",11,dim);
    c.at(0,291); ImGui::PushFont(v.body,14.4f);
    if(ImGui::BeginTable("observed-events",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("序号",ImGuiTableColumnFlags_WidthFixed,46*c.s);
        ImGui::TableSetupColumn("调用",ImGuiTableColumnFlags_WidthFixed,96*c.s);
        ImGui::TableSetupColumn("队列地址",0,1);
        ImGui::TableSetupColumn("CPU QPC",0,1);
        ImGui::TableSetupColumn("参数",0,1.1f);
        ImGui::TableHeadersRow();
        const auto recent=obs.value("recent",json::array());
        for(auto it=recent.rbegin();it!=recent.rend();++it) {
            const auto& row=*it;
            ImGui::TableNextRow(0,27*c.s);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.at("seq").dump().c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.value("event","").c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.value("queue_id","").c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.at("cpu_qpc").dump().c_str());
            ImGui::TableNextColumn();
            const auto detail=row.value("event","")=="queue_submit"?"lists "+row.value("list_count",json()).dump():
                "fence "+row.value("fence_id","")+" / "+row.value("value",json()).dump();
            ImGui::TextWrapped("%s",detail.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
    float y=ImGui::GetCursorPosY()/c.s+25;
    if(obs.value("recent",json::array()).empty()) { c.text(0,y,"尚未写入事件。",12,dim); y+=40; }
    c.text(0,y,"队列与命令列表目前仅记录地址；分配代次和 GPU 完成未覆盖。",12,dim,nullptr,w);
    y=ImGui::GetCursorPosY()/c.s+24;
    if(c.button("observer.open",0,y,164,"打开本次原始记录",false,true,35)) open_runs(v,obs.value("directory",""));
    c.fit(184,y+9,obs.value("directory",""),w-184,11,dim);
    c.at(0,y+60); ImGui::Dummy({1,1});
}
void selftest(Canvas& c,float w,float y,const std::function<void(const char*,json)>& send) {
    auto& v=c.v;
    c.text(0,y,"以下仅验证控制协议，不连接模型或游戏。",12,amber,nullptr,w); y+=42;
    const auto requested=v.status.value("requested",json::object()),observed=v.status.value("observed",json::object());
    const bool available=writable(v), change=available&&idle(v)&&!v.status.value("pending",false);
    c.text(0,y,"请求："+mode_text(requested)+"   协议回执："+mode_text(observed)+
        "   模拟生效帧："+v.status.value("applied_frame",json()).dump(),12,muted); y+=35;
    const char* ids[]={"mode.off","mode.on","mode.compute"};
    const char* modes[]={"off","on","compute_bypass"};
    const char* names[]={"OFF","ON","仅计算"};
    for(int i=0;i<3;++i)
        if(c.button(ids[i],i*104.f,y,96,names[i],requested.value("nr_mode","")==modes[i],change,32))
            send("ApplyConfig",{{"config",{{"nr_mode",modes[i]}}}});
    y+=51;
    if(c.check("scene.confirmed",0,y,"确认协议自测（非游戏场景）",v.scene_confirmed,available&&idle(v),w)) v.scene_confirmed=!v.scene_confirmed;
    y+=46;
    const auto state=v.status.value("state","");
    const bool arm=available&&idle(v)&&v.scene_confirmed&&!v.status.value("pending",false);
    if(c.button("run.arm",0,y,80,"准备",false,arm,32)) send("Arm",{{"scene_confirmed",true}});
    if(c.button("run.start",90,y,80,state=="paused"?"继续":"开始",false,available&&(state=="armed"||state=="paused"),32)) send("Start",json::object());
    if(c.button("run.pause",180,y,80,"暂停",false,available&&state=="running",32)) send("Pause",json::object());
    if(c.button("run.cancel",270,y,80,"取消",false,available&&(state=="running"||state=="paused"||state=="armed"),32)) send("Cancel",json::object());
    c.text(369,y+8,state_text(state),12,muted);
    c.at(0,y+58); ImGui::Dummy({1,1});
}
void diagnostics(Canvas& c,float w,const std::function<void(const char*,json)>& send) {
    auto& v=c.v; const auto obs=v.status.value("observer",json());
    c.text(0,0,"诊断",22,white,v.bold);
    c.text(0,47,"能力与证据边界",14,white,v.bold);
    const char* labels[]={"D3D12 队列事件","NGX Feature 与资源来源","GPU 完成与真实帧关联","游戏 NR 开关 / 参数 / Mask","同帧图像采集","P0 顺序门禁"};
    const auto np=nr_panel(v.status,v.connected,v.pending,v.client_id,v.nr_scene_confirmed);
    const bool runtime=v.connected&&v.status.value("nr_runtime",json()).is_object();
    const std::string values[]={obs.is_object()?(obs.value("probe_state","")=="failed"?"观察器失败 · 查看错误":"部分覆盖 · 可查看原始记录"):"没有事件取证数据",
        runtime?"运行器已连接 · 非完整血缘":"未接入",runtime?"NR 完成回执 · 非完整取证":"未接入",
        np.failed?"NR 不可用 · 查看错误":np.ready?(v.status.value("capabilities",json::object()).value("nr_settings",false)?"开关 / Tone / Structure · Mask 未接通":"开关可用 · 参数/Mask 未接通"):np.preparing?"准备中":np.confirmable?"尚未准备":"未接入",
        v.status.value("capabilities",json::object()).value("capture_pair",false)?"手动四阶段 FP16 · 非最终显示":"未接入","未通过"};
    for(int i=0;i<6;++i) {
        const float y=86.f+i*42;
        c.text(0,y,labels[i],13,muted); c.text(w-215,y,values[i],13,white); c.line(0,y+31,w);
    }
    float y=365;
    const auto error=v.error.empty()?v.status.value("last_error",std::string()):v.error;
    c.text(0,y,"最近错误",14,white,v.bold); y+=35;
    c.text(0,y,error.empty()?"无错误报告":error,12,error.empty()?muted:amber,nullptr,w);
    y=ImGui::GetCursorPosY()/c.s+25;
    if(obs.is_object()) {
        const auto reason=obs.value("stop_reason","");
        if(!reason.empty()) { c.text(0,y,"记录停止原因："+reason,12,reason=="requested"?muted:amber,nullptr,w); y=ImGui::GetCursorPosY()/c.s+22; }
        c.text(0,y,"QPC 频率："+obs.value("qpc_frequency",json()).dump()+
            " Hz   列表截断："+obs.value("list_details_truncated",json()).dump()+
            "   未列出队列事件："+obs.value("queue_details_omitted",json()).dump(),12,muted,nullptr,w);
        y=ImGui::GetCursorPosY()/c.s+24;
    }
    c.at(0,y); ImGui::PushFont(v.body,15.6f);
    v.raw_expanded=ImGui::CollapsingHeader("原始协议状态 / JSON"); c.record("diagnostics.raw");
    if(v.raw_expanded) {
        if(ImGui::Button("复制 JSON")) ImGui::SetClipboardText(v.status.dump(2).c_str());
        ImGui::TextWrapped("%s",v.status.dump(2).c_str());
    }
    ImGui::PopFont(); y=ImGui::GetCursorPosY()/c.s+24;
    if(synthetic(v)) {
        c.at(0,y); ImGui::PushFont(v.body,15.6f);
        v.selftest_open=ImGui::CollapsingHeader("开发者协议自测 · 不执行 NR"); c.record("diagnostics.selftest");
        ImGui::PopFont(); y=ImGui::GetCursorPosY()/c.s+18;
        if(v.selftest_open) selftest(c,w,y,send);
    } else v.selftest_open=false;
}
}
ImGuiStyle console_style() {
    ImGui::StyleColorsDark(); ImGuiStyle style=ImGui::GetStyle();
    style.WindowPadding={0,0}; style.WindowBorderSize=0; style.ChildBorderSize=0;
    style.FramePadding={10,9}; style.ItemSpacing={8,8}; style.FrameRounding=4;
    style.PopupRounding=7; style.PopupBorderSize=1; style.ScrollbarSize=5; style.ScrollbarRounding=3;
    style.GrabRounding=3; style.DisabledAlpha=.55f;
    style.Colors[ImGuiCol_WindowBg]=color(bg); style.Colors[ImGuiCol_ChildBg]={0,0,0,0};
    style.Colors[ImGuiCol_PopupBg]=color(IM_COL32(26,31,37,255));
    style.Colors[ImGuiCol_Text]=color(white); style.Colors[ImGuiCol_TextDisabled]=color(muted);
    style.Colors[ImGuiCol_FrameBg]=color(IM_COL32(31,36,42,255));
    style.Colors[ImGuiCol_FrameBgHovered]=color(surface); style.Colors[ImGuiCol_FrameBgActive]=color(edge);
    style.Colors[ImGuiCol_CheckMark]=color(green); style.Colors[ImGuiCol_NavCursor]=color(green);
    style.Colors[ImGuiCol_SliderGrab]=color(green);style.Colors[ImGuiCol_SliderGrabActive]=color(IM_COL32(151,211,37,255));
    style.Colors[ImGuiCol_Button]=color(surface); style.Colors[ImGuiCol_ButtonHovered]=color(edge);
    style.Colors[ImGuiCol_ButtonActive]=color(IM_COL32(59,66,48,255));
    style.Colors[ImGuiCol_Header]=color(surface); style.Colors[ImGuiCol_HeaderHovered]=color(edge);
    style.Colors[ImGuiCol_HeaderActive]=color(IM_COL32(44,58,27,255));
    style.Colors[ImGuiCol_TitleBgActive]=color(bg); style.Colors[ImGuiCol_TitleBg]=color(bg);
    style.Colors[ImGuiCol_Border]=color(edge); style.Colors[ImGuiCol_Separator]=color(edge);
    style.Colors[ImGuiCol_TextSelectedBg]=color(IM_COL32(61,83,30,255));
    style.Colors[ImGuiCol_ScrollbarBg]={0,0,0,0}; style.Colors[ImGuiCol_ScrollbarGrab]=color(edge);
    return style;
}

void draw_console(ConsoleView& v,float dpi,const std::function<void(const char*,json)>& send) {
    if(!v.connected){v.nr_settings_dirty=false;v.nr_settings_revision=UINT64_MAX;v.nr_settings_session.clear();}
    v.controls.clear(); v.control_clips.clear();
    auto viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos); ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("Overglaze##console",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    const float width=viewport->Size.x/dpi,height=viewport->Size.y/dpi,left=202;
    Canvas root{v,dpi}; header(root,width,send);
    root.box(0,73,left,height-73,rail,0);
    root.d->AddLine(root.p(left,73),root.p(left,height),edge);
    root.at(15,94); ImGui::BeginChild("navigation",{(left-30)*dpi,(height-107)*dpi});
    Canvas nav{v,dpi}; sidebar(nav,left-30,height-107); ImGui::EndChild();
    root.at(left+32,105);
    if(v.last_page!=v.page) { ImGui::SetNextWindowScroll({0,0}); v.last_page=v.page; }
    ImGui::BeginChild("content",{(width-left-48)*dpi,(height-121)*dpi});
    Canvas main{v,dpi}; const float w=width-left-72;
    if(v.page==0) workspace(main,w,send);
    else if(v.page==1) capture_view(main,w,send);
    else if(v.page==2) observer_view(main,w);
    else diagnostics(main,w,send);
    ImGui::EndChild();
    ImGui::End();
}
}
