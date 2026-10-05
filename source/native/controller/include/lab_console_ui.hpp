// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include "lab_platform.hpp"
#include <imgui.h>
#include <functional>
#include <map>

namespace lab {
struct ConsoleView {
    int page = 0, pid = 0;
    int last_page = -1;
    char pid_text[16]{};
    bool connected = false, pending = false, scene_confirmed = false;
    bool nr_scene_confirmed=false; // Separate from observer sampling / protocol self-test.
    bool automatic_connection=false;
    float nr_tone=1.f,nr_structure=1.f;
    std::uint64_t nr_settings_revision=UINT64_MAX;
    std::uint64_t nr_rebuild_serial=0;
    std::string nr_settings_session;
    bool nr_settings_dirty=false;
    bool preview_loading=false,preview_open=false;
    std::string preview_source,preview_error;
    json preview_manifest=json();
    unsigned preview_width=0,preview_height=0,preview_original_width=0,preview_original_height=0;
    std::array<ImTextureID,4> preview_textures{};
    int preview_pair=0;
    float preview_zoom=1.f,preview_x=.5f,preview_y=.5f;
    bool discovery_open = false, raw_expanded = false, selftest_open = false;
    int capture_seconds=30;
    bool capture_functional=false;
    char capture_question[257]="记录当前场景的 CPU 呈现回调节奏";
    std::uint64_t received_ms = 0;
    std::string error, client_id;
    json status = json::object();
    std::vector<DWORD> sessions;
    // Test instrumentation for this application's own widgets, in screen pixels.
    std::map<std::string, ImVec4> controls;
    std::map<std::string, ImVec4> control_clips;
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* display = nullptr;
    ImFont* mono = nullptr;
};
ImGuiStyle console_style();
void draw_console(ConsoleView& view, float dpi,
                  const std::function<void(const char*, json)>& send);
}
