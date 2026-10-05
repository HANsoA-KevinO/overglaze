// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <imgui.h>

// Presentation only. No product-state inference, I/O or renderer changes.
namespace lab::product {
inline constexpr ImVec4 background{.047f,.059f,.067f,1};
inline constexpr ImVec4 surface{.071f,.086f,.098f,1};
inline constexpr ImVec4 raised{.102f,.122f,.137f,1};
inline constexpr ImVec4 border{.17f,.20f,.21f,1};
inline constexpr ImVec4 text{.91f,.93f,.90f,1};
inline constexpr ImVec4 muted{.56f,.61f,.62f,1};
inline constexpr ImVec4 accent{.714f,.847f,.549f,1};
inline constexpr ImVec4 amber{.91f,.72f,.43f,1};
inline void apply_theme() {
    ImGui::StyleColorsDark();
    auto& s=ImGui::GetStyle();
    s.WindowPadding={24,22};s.FramePadding={13,9};s.ItemSpacing={12,12};s.ItemInnerSpacing={8,6};
    s.WindowRounding=12;s.ChildRounding=10;s.FrameRounding=6;s.PopupRounding=10;
    s.GrabRounding=5;s.ScrollbarRounding=8;s.ScrollbarSize=10;
    s.WindowBorderSize=0;s.ChildBorderSize=1;s.PopupBorderSize=1;s.FrameBorderSize=0;
    s.DisabledAlpha=.46f;
    auto* c=s.Colors;
    c[ImGuiCol_Text]=text;c[ImGuiCol_TextDisabled]=muted;
    c[ImGuiCol_WindowBg]=background;c[ImGuiCol_ChildBg]=surface;c[ImGuiCol_PopupBg]=surface;
    c[ImGuiCol_TitleBg]=surface;c[ImGuiCol_TitleBgActive]=raised;c[ImGuiCol_TitleBgCollapsed]=background;
    c[ImGuiCol_Border]=border;c[ImGuiCol_Separator]=border;
    c[ImGuiCol_FrameBg]=raised;c[ImGuiCol_FrameBgHovered]={.15f,.18f,.18f,1};c[ImGuiCol_FrameBgActive]={.17f,.21f,.20f,1};
    c[ImGuiCol_Button]=raised;c[ImGuiCol_ButtonHovered]={.17f,.21f,.20f,1};c[ImGuiCol_ButtonActive]={.21f,.27f,.23f,1};
    c[ImGuiCol_Header]={.13f,.18f,.14f,1};c[ImGuiCol_HeaderHovered]={.15f,.21f,.17f,1};c[ImGuiCol_HeaderActive]={.20f,.28f,.19f,1};
    c[ImGuiCol_CheckMark]=accent;c[ImGuiCol_SliderGrab]=accent;c[ImGuiCol_SliderGrabActive]={.82f,.94f,.66f,1};
    c[ImGuiCol_ScrollbarBg]={0,0,0,0};c[ImGuiCol_ScrollbarGrab]={.23f,.27f,.28f,1};
    c[ImGuiCol_ScrollbarGrabHovered]={.34f,.39f,.39f,1};c[ImGuiCol_ScrollbarGrabActive]=muted;
    c[ImGuiCol_TextSelectedBg]={.36f,.48f,.25f,.5f};c[ImGuiCol_NavCursor]=accent;
    c[ImGuiCol_ModalWindowDimBg]={.01f,.02f,.025f,.78f};
}
inline void mark(ImDrawList* draw,ImVec2 p,float size) {
    const float u=size/64.f;
    const ImU32 green=ImGui::GetColorU32(accent),dim=IM_COL32(81,103,71,255);
    draw->AddRectFilled(p,{p.x+size,p.y+size},IM_COL32(16,20,24,255),14*u);
    draw->AddRect({p.x+11*u,p.y+21*u},{p.x+43*u,p.y+53*u},dim,9*u,0,6*u);
    draw->AddRectFilled({p.x+21*u,p.y+11*u},{p.x+53*u,p.y+43*u},IM_COL32(16,20,24,255),9*u);
    draw->AddRect({p.x+21*u,p.y+11*u},{p.x+53*u,p.y+43*u},green,9*u,0,6*u);
}
inline void wordmark(float scale=1) {
    const auto p=ImGui::GetCursorScreenPos();mark(ImGui::GetWindowDrawList(),p,40*scale);
    ImGui::Dummy({40*scale,40*scale});ImGui::SameLine(0,12*scale);
    ImGui::BeginGroup();ImGui::PushFont(nullptr,18);ImGui::TextUnformatted("OVERGLAZE");ImGui::PopFont();
    ImGui::TextDisabled("釉光  /  NEURAL RENDERING");ImGui::EndGroup();
}
inline void eyebrow(const char* label){ImGui::TextColored(muted,"%s",label);}
inline bool action(const char* label,ImVec2 size={},bool primary=false) {
    if(primary){ImGui::PushStyleColor(ImGuiCol_Button,accent);ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4{.79f,.91f,.63f,1});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4{.61f,.76f,.43f,1});ImGui::PushStyleColor(ImGuiCol_Text,background);}
    const bool clicked=ImGui::Button(label,size);if(primary)ImGui::PopStyleColor(4);return clicked;
}
inline void pill(const char* label,ImVec4 color=accent) {
    const auto p=ImGui::GetCursorScreenPos(),t=ImGui::CalcTextSize(label);const auto d=ImGui::GetStyle().FontScaleDpi;
    ImVec4 fill=color;fill.w=.11f;const ImVec2 size{t.x+20*d,t.y+10*d};
    ImGui::GetWindowDrawList()->AddRectFilled(p,{p.x+size.x,p.y+size.y},ImGui::GetColorU32(fill),4*d);
    ImGui::GetWindowDrawList()->AddText({p.x+10*d,p.y+5*d},ImGui::GetColorU32(color),label);ImGui::Dummy(size);
}
inline bool navigation(const char* label,bool selected,ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button,selected?ImVec4{.14f,.19f,.15f,1}:ImVec4{0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_Text,selected?accent:muted);
    const bool clicked=ImGui::Button(label,size);ImGui::PopStyleColor(2);return clicked;
}
}
