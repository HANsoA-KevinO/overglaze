// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_pipe.hpp"
#include "lab_console_ui.hpp"
#include "lab_pair_preview.hpp"
#include "lab_root_locator.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <fstream>
#include <future>
#include <chrono>
#include <wincodec.h>
#include <shellapi.h>
#include <dwmapi.h>

using Microsoft::WRL::ComPtr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<IDXGISwapChain> swapchain;
ComPtr<ID3D11RenderTargetView> target;
UINT resize_width = 0, resize_height = 0;
bool quitting = false;
LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
    if (msg == WM_SIZE && wparam != SIZE_MINIMIZED) { resize_width = LOWORD(lparam); resize_height = HIWORD(lparam); return 0; }
    if (msg == WM_GETMINMAXINFO) {
        auto info = reinterpret_cast<MINMAXINFO*>(lparam);
        const auto dpi = GetDpiForWindow(hwnd);
        info->ptMinTrackSize = {MulDiv(1180, dpi ? dpi : 96, 96), MulDiv(780, dpi ? dpi : 96, 96)};
        return 0;
    }
    if (msg == WM_DPICHANGED) {
        auto rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER);
        return 0;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
bool make_target() {
    ComPtr<ID3D11Texture2D> buffer;
    return SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer)))
        && SUCCEEDED(device->CreateRenderTargetView(buffer.Get(), nullptr, &target));
}

void save_render(const std::filesystem::path& path) {
    if (!path.is_absolute() || std::filesystem::exists(path)) throw std::runtime_error("Screenshot requires a new absolute path");
    ComPtr<ID3D11Texture2D> source; if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&source)))) throw std::runtime_error("Screenshot source");
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) throw std::runtime_error("Screenshot staging");
    context->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) throw std::runtime_error("Screenshot map");
    std::vector<BYTE> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x) {
        auto* src = static_cast<BYTE*>(mapped.pData) + y * mapped.RowPitch + x * 4;
        auto* dst = pixels.data() + (static_cast<size_t>(y) * desc.Width + x) * 4;
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255;
    }
    context->Unmap(staging.Get(), 0);
    ComPtr<IWICImagingFactory> factory;
    auto check_hr = [](HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("PNG encoder HRESULT=" + std::to_string(hr)); };
    check_hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream; check_hr(factory->CreateStream(&stream)); check_hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder; check_hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    check_hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; check_hr(encoder->CreateNewFrame(&frame, nullptr)); check_hr(frame->Initialize(nullptr));
    check_hr(frame->SetSize(desc.Width, desc.Height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA; check_hr(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("Unexpected PNG pixel format");
    check_hr(frame->WritePixels(desc.Height, desc.Width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    check_hr(frame->Commit()); check_hr(encoder->Commit());
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandline, int show) {
    const bool interaction_test = std::wstring(commandline).find(L"--ui-test") != std::wstring::npos;
    const bool synthetic_test=std::wstring(commandline).find(L"--ui-test-synthetic")!=std::wstring::npos;
    const bool observer_test=std::wstring(commandline).find(L"--ui-test-observer")!=std::wstring::npos;
    const bool capture_test=std::wstring(commandline).find(L"--ui-test-capture")!=std::wstring::npos;
    const bool nr_test=std::wstring(commandline).find(L"--ui-test-nr-frame")!=std::wstring::npos;
    const bool preparation_test=std::wstring(commandline).find(L"--ui-test-nr-preparation")!=std::wstring::npos;
    const bool workbench_test=std::wstring(commandline).find(L"--ui-test-workbench")!=std::wstring::npos;
    const bool automatic_test=std::wstring(commandline).find(L"--ui-test-automatic")!=std::wstring::npos;
    const bool preview_test=std::wstring(commandline).find(L"--ui-test-pairpreview")!=std::wstring::npos;
    const bool smoke = interaction_test || std::wstring(commandline).find(L"--smoke-test") != std::wstring::npos;
    try {
        lab::Handle singleton;
        if(!smoke){singleton.value=CreateMutexW(nullptr,FALSE,L"Local\\OverglazeConsole.Singleton");
            lab::check(singleton.valid(),"Console instance mutex");if(GetLastError()==ERROR_ALREADY_EXISTS)return 0;}
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::filesystem::path screenshot;
        std::filesystem::path nr_acceptance;
        std::filesystem::path initial_pair;
int initial_page=0, connect_pid=0, window_width=1440, window_height=940, capture_step=-1;
bool screenshot_saved=false,size_specified=false;
        float test_scale=0;
        int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for(int i=1; argv && i<argc; ++i) {
            std::wstring arg=argv[i];
            if(arg==L"--smoke-test" && i+1<argc && argv[i+1][0]!=L'-') screenshot=argv[++i];
else if(arg==L"--page" && i+1<argc) initial_page=std::clamp(_wtoi(argv[++i]),0,3);
            else if(arg==L"--pair-preview"&&i+1<argc){initial_pair=argv[++i];initial_page=1;}
            else if(arg==L"--connect-pid" && i+1<argc) connect_pid=std::max(0,_wtoi(argv[++i]));
            else if(arg==L"--size" && i+2<argc) { window_width=std::max(1180,_wtoi(argv[++i])); window_height=std::max(780,_wtoi(argv[++i])); size_specified=true; }
            else if(arg==L"--test-scale" && i+1<argc && smoke) test_scale=std::clamp(static_cast<float>(_wtof(argv[++i])),1.f,2.f);

            else if(arg==L"--capture-step" && i+1<argc && interaction_test) capture_step=_wtoi(argv[++i]);
            else if(arg==L"--nr-acceptance" && i+1<argc && nr_test) nr_acceptance=argv[++i];
        }
        if (argv) LocalFree(argv);
        if(nr_test && (connect_pid<=0 || nr_acceptance.empty() || !nr_acceptance.is_absolute() || std::filesystem::exists(nr_acceptance)))
            throw std::runtime_error("External real-NR PID and a new absolute acceptance path required");

        if(synthetic_test||observer_test||capture_test||preparation_test||workbench_test) initial_page=0;
        ImGui_ImplWin32_EnableDpiAwareness();
        if(!size_specified) {
            const auto initial_dpi=GetDpiForSystem();
            window_width=MulDiv(window_width,initial_dpi,96);
            window_height=MulDiv(window_height,initial_dpi,96);
        }
        WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = window_proc; wc.hInstance = instance;
        wc.lpszClassName = L"OverglazeConsole"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        lab::check(RegisterClassExW(&wc) != 0, "Register window");
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"釉光 · Overglaze — 实验控制台", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, window_width, window_height, nullptr, nullptr, instance, nullptr);
        lab::check(hwnd != nullptr, "Create window");
        BOOL dark=TRUE; DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
        COLORREF caption=RGB(19,24,30); DwmSetWindowAttribute(hwnd,35,&caption,sizeof(caption));
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2; desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow = hwnd;
        desc.SampleDesc.Count = 1; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL level{};
        auto hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &desc, &swapchain, &device, &level, &context);
        if (FAILED(hr) || !make_target()) throw std::runtime_error("Console D3D11 initialization failed");
        ShowWindow(hwnd, smoke ? SW_HIDE : show);
        IMGUI_CHECKVERSION(); ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        wchar_t windows[32768]{}; GetWindowsDirectoryW(windows, 32768);
lab::ConsoleView view; view.page=initial_page;view.automatic_connection=!smoke&&connect_pid<=0;
        std::future<std::vector<DWORD>> discovery;
        ULONGLONG next_discovery=0;
        auto load_font=[&](const char* name, float size, ImFont* fallback) {
            auto path=std::filesystem::path(windows)/"Fonts"/name;
            return std::filesystem::exists(path) ? io.Fonts->AddFontFromFileTTF(lab::utf8(path.wstring()).c_str(),size) : fallback;
        };
        view.body=load_font("msyh.ttc",17,nullptr);
        if(!view.body) view.body=io.Fonts->AddFontDefault();
        io.FontDefault=view.body;
        view.bold=load_font("msyhbd.ttc",17,view.body);
        view.display=load_font("bahnschrift.ttf",22,view.body);
        view.mono=load_font("consola.ttf",14,view.body);
        ImGuiStyle base_style=lab::console_style();
        ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(device.Get(), context.Get());

        int frames=0, test_step=0, test_phase=0;
        std::string client=lab::uuid(); view.client_id=client;
        if(connect_pid>0) {
            view.pid=connect_pid; snprintf(view.pid_text,sizeof(view.pid_text),"%d",view.pid);
        }
        // This test-only server exercises the real IPC path, never loads NR or a game.
        std::unique_ptr<lab::Controller> test_controller;
        std::unique_ptr<lab::PipeServer> test_server;
        if(synthetic_test||observer_test||capture_test||preparation_test||workbench_test||automatic_test) {
            test_controller=std::make_unique<lab::Controller>(synthetic_test);
            if(workbench_test){test_controller->enable_nr_frame_control("synthetic-input-real-nr",true,true);
                test_controller->publish_nr_runtime({{"profile","cyberpunk-rr-experimental-v1"},{"state","ready"},{"input_origin","ui-test-no-NR"},
                    {"settings",{{"requested_revision",0u},{"requested",{{"tone",1.f},{"structure",1.f}}},{"observed",nullptr}}}});}
            if(preparation_test){test_controller->enable_nr_preparation();test_controller->publish_host({{"backend","standalone-d3d12"},{"origin","synthetic"},{"state","awaiting-user-preparation"}});}
            if(automatic_test){test_controller->enable_nr_preparation(true);test_controller->publish_host({{"backend","standalone-d3d12"},{"origin","synthetic"},{"state","standby"}});}
            if(capture_test){test_controller->enable_manual_capture(lab::root::resolve_self().data,"synthetic");view.capture_functional=true;}
            test_server=std::make_unique<lab::PipeServer>(*test_controller); test_server->start();
            view.pid=static_cast<int>(GetCurrentProcessId());
            snprintf(view.pid_text,sizeof(view.pid_text),"%d",view.pid);
        }
        struct UiStep { const char* id; std::function<bool()> check; };
        std::vector<UiStep> steps;
        std::vector<lab::json> capture_test_results;
        std::vector<lab::json> nr_test_results,nr_test_requests;
        unsigned preparation_requests=0;ULONGLONG preparation_failure_at=0;
        unsigned ui_setting_receipts=0,ui_pairs=0;lab::json ui_pair=nullptr;
        auto step=[&](const char* id,std::function<bool()> check) { steps.push_back({id,std::move(check)}); };
        auto state_is=[&](const char* value) { return view.status.value("state","")==value&&!view.pending; };
        auto mode_is=[&](const char* value) {
            return view.status.value("observed",lab::json::object()).value("nr_mode","")==value &&
                !view.pending&&!view.status.value("pending",false);
        };
        auto no_facade=[&] {
            for(const char* id:{"parameter.structure","parameter.tone","parameter.character","parameter.mask",
                "capture.pair","recipe.0","frame.pair","result.tab.1","nav.experiments","nav.results","layout.0"})
                if(view.controls.contains(id)) return false;
            return true;
        };
        if(preview_test){
            if(initial_pair.empty())throw std::runtime_error("Preview interaction test requires a real local package");
            step("preview.nr",[&]{return view.preview_open&&view.preview_pair==0;});
            step("preview.hdr",[&]{return view.preview_pair==1;});
            step("preview.zoom",[&]{return view.preview_zoom>1;});
            step("preview.x",[&]{return view.preview_open;});
            step("preview.y",[&]{return view.preview_open;});
            step("preview.nr",[&]{return view.preview_pair==0;});
            step("preview.back",[&]{return !view.preview_open;});
        }else if(automatic_test){
            step("session.connect",[&]{return view.connected&&!view.pending&&view.controls.contains("nr.live.on")&&!view.controls.contains("nr.prepare.start")&&!view.controls.contains("nr.scene.confirmed");});
            step("nr.live.on",[&]{return !view.pending&&view.status["nr_frame_control"]["observed_mode"]=="on";});
            step("nr.live.off",[&]{return !view.pending&&view.status["nr_frame_control"]["observed_mode"]=="off";});
            step("nr.live.on",[&]{return !view.pending&&view.status["nr_frame_control"]["observed_mode"]=="on";});
            step("session.disconnect",[&]{return !view.connected;});
        }else if(workbench_test){
            step("session.connect",[&]{return view.connected&&!view.pending&&view.controls.contains("parameter.tone");});
            step("parameter.tone",[&]{return ui_setting_receipts==0;}); // Disabled while OFF/unknown.
            step("nr.live.on",[&]{return !view.pending&&view.status["nr_frame_control"]["observed_mode"]=="on";});
            step("parameter.tone",[&]{return !view.pending&&ui_setting_receipts==1&&view.status["nr_runtime"]["settings"]["observed"].is_object()&&view.status["nr_runtime"]["settings"]["observed"]["tone"].get<float>()<.9f;});
            step("parameter.structure",[&]{return !view.pending&&ui_setting_receipts==2&&view.status["nr_runtime"]["settings"]["observed"].is_object()&&view.status["nr_runtime"]["settings"]["observed"]["structure"].get<float>()<.9f;});
            step("nr.capture.page",[&]{return view.page==1&&view.controls.contains("capture.pair");});
            step("capture.pair",[&]{return !view.pending&&ui_pairs==1&&view.status["frame_pair"]["state"]=="armed";});
            step("capture.pair",[&]{return ui_pairs==1;}); // Busy click cannot duplicate.
            step("capture.pair.cancel",[&]{return !view.pending&&view.status["frame_pair"]["state"]=="cancelled";});
            step("nav.workspace",[&]{return view.page==0&&view.controls.contains("nr.live.off");});
            step("nr.live.off",[&]{return !view.pending&&view.status["nr_frame_control"]["observed_mode"]=="off";});
            step("parameter.structure",[&]{return ui_setting_receipts==2;});
            step("session.disconnect",[&]{return !view.connected;});
        }else if(preparation_test){
            step("session.connect",[&]{return view.connected&&!view.pending&&view.controls.contains("nr.prepare.start")&&!view.controls.contains("nr.live.on");});
            step("nr.prepare.start",[&]{return preparation_requests==0&&test_controller->status()["nr_preparation"]["state"]=="waiting_for_user";});
            step("nr.scene.confirmed",[&]{return view.nr_scene_confirmed;});
            step("nr.prepare.start",[&]{return !view.pending&&preparation_requests==1&&view.status["nr_preparation"]["state"]=="preparing"&&!view.controls.contains("nr.live.on");});
            step("session.refresh",[&]{return !view.pending&&view.status["nr_preparation"]["state"]=="failed"&&view.controls.contains("nr.diagnostics")&&!view.controls.contains("nr.prepare.start")&&!view.controls.contains("nr.live.on");});
            step("nr.diagnostics",[&]{return view.page==3;});
            step("nav.workspace",[&]{return view.page==0&&view.controls.contains("nr.diagnostics");});
            step("session.disconnect",[&]{return !view.connected&&!view.nr_scene_confirmed;});
            step("session.connect",[&]{return view.connected&&!view.pending&&view.controls.contains("nr.diagnostics")&&!view.controls.contains("nr.prepare.start");});
            step("session.disconnect",[&]{return !view.connected;});
        }else if(nr_test){
            step("session.connect",[&]{return view.connected&&!view.pending && view.status.value("origin","")=="synthetic-input-real-nr";});
            const std::array<const char*,6> nr_modes{"off","on","on","off","on","off"};
            for(unsigned i=0;i<nr_modes.size();++i){const auto expected=std::string(nr_modes[i]);
                step(expected=="on"?"nr.live.on":"nr.live.off",[&,i,expected]{
                    const auto nr=view.status.value("nr_frame_control",lab::json());
                    if(view.pending || !nr.is_object() || nr.value("state","")!="applied" || nr["applied_frame"]!=i+1 || nr["observed_mode"]!=expected)return false;
                    nr_test_results.push_back(view.status);return true;
                });
            }
            step("session.disconnect",[&]{return !view.connected;});
        }else if(capture_test){
            auto capture_state=[&](const char* value){return !view.pending && view.status.value("capture",lab::json::object()).value("state","")==value;};
            step("session.connect",[&]{return view.connected&&!view.pending;});
            step("nav.capture",[&]{return view.page==1&&view.controls.contains("capture.start");});
            step("capture.start",[&,capture_state]{return capture_state("recording");});
            step("capture.stop",[&,capture_state]{if(!capture_state("completed"))return false;capture_test_results.push_back(view.status["capture"]["last_result"]);return true;});
            step("capture.start",[&,capture_state]{return capture_state("recording");});
            step("capture.stop",[&,capture_state]{if(!capture_state("completed")||view.status["capture"]["completed_runs"]!=2)return false;
                capture_test_results.push_back(view.status["capture"]["last_result"]);return true;});
            step("session.disconnect",[&]{return !view.connected;});
        } else if(synthetic_test) {
            step("session.connect",[&] { return view.connected&&!view.pending; });
            step("nav.diagnostics",[&] { return view.page==3&&no_facade(); });
            step("diagnostics.selftest",[&] { return view.selftest_open; });
            step("mode.on",[&] { return mode_is("on"); });
            step("scene.confirmed",[&] { return view.scene_confirmed; });
            step("run.arm",[&] { return state_is("armed"); });
            step("run.start",[&] { return state_is("running"); });
            step("mode.off",[&] { return state_is("running")&&mode_is("on"); });
            step("run.pause",[&] { return state_is("paused"); });
            step("run.start",[&] { return state_is("running"); });
            step("run.cancel",[&] { return state_is("cancelled"); });
            step("mode.compute",[&] { return mode_is("compute_bypass"); });
            step("mode.off",[&] { return mode_is("off"); });
            step("nav.observer",[&] { return view.page==2&&!view.controls.contains("mode.on")&&no_facade(); });
            step("session.disconnect",[&] { return !view.connected&&view.status.empty()&&!view.scene_confirmed; });
        } else if(observer_test) {
            step("session.connect",[&] { return view.connected&&!view.pending&&view.status.value("origin","")=="unattached"; });
            step("nav.observer",[&] { return view.page==2&&no_facade(); });
            step("nav.diagnostics",[&] { return view.page==3&&!view.controls.contains("diagnostics.selftest")&&!view.controls.contains("mode.on"); });
            step("diagnostics.raw",[&] { return view.raw_expanded; });
            step("session.disconnect",[&] { return !view.connected&&view.status.empty(); });
        } else {
            step("nav.observer",[&] { return view.page==2&&no_facade(); });
            step("observer.connect",[&] { return view.page==0; });
            step("session.discover",[&] { return view.discovery_open; });
            step("escape",[&] { return !view.discovery_open; });
            step("nav.diagnostics",[&] { return view.page==3&&no_facade(); });
            step("diagnostics.raw",[&] { return view.raw_expanded; });
            step("nav.workspace",[&] { return view.page==0&&!view.controls.contains("mode.on"); });
        }
        const auto test_deadline=GetTickCount64()+15000;
        std::future<lab::json> pending;
        std::future<lab::PairPreview> preview_pending;
        std::array<ComPtr<ID3D11ShaderResourceView>,4> preview_views;
        auto load_preview=[&](const std::filesystem::path& path){if(preview_pending.valid())return;
            view.preview_loading=true;view.preview_error.clear();view.preview_open=false;
            preview_pending=std::async(std::launch::async,[path]{return lab::load_pair_preview(path,lab::root::resolve_self().data);});};
        if(!initial_pair.empty())load_preview(initial_pair);
        ULONGLONG next_poll = 0;
        auto send = [&](const char* method, lab::json params = lab::json::object()) {
            if(std::string(method)=="LoadFramePair"){
                const auto pair=view.status.value("frame_pair",lab::json());
                if(pair.is_object()&&pair.value("state","")=="complete")load_preview(lab::wide(pair.value("manifest",std::string())));return;
            }
            if (view.pid <= 0 || pending.valid()) return;
            lab::json request = {{"protocol", "1.0"}, {"request_id", lab::uuid()}, {"client_id", client}, {"method", method}};
            if (view.status.contains("session_id")) {
                request["session_id"] = view.status["session_id"];
                request["expected_revision"] = view.status["revision"];
            }
            request["params"] = params;
            if(nr_test && std::string(method)=="SetNrMode")nr_test_requests.push_back(request);
            if(preparation_test&&std::string(method)=="PrepareNr")++preparation_requests;
            pending = std::async(std::launch::async, [id = static_cast<DWORD>(view.pid), request]() { return lab::pipe_request(id, request); });
        };
        if(connect_pid>0) send("Hello");
        while (!quitting) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) quitting = true;
            }
            if (quitting) break;
            if(view.automatic_connection&&!view.connected&&!pending.valid()){
                if(discovery.valid()&&discovery.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
                    try{view.sessions=discovery.get();if(view.sessions.size()==1){view.pid=int(view.sessions[0]);
                        snprintf(view.pid_text,sizeof(view.pid_text),"%d",view.pid);send("Hello");}}
                    catch(const std::exception& e){view.error=e.what();}next_discovery=GetTickCount64()+2000;
                }
                if(!pending.valid()&&!discovery.valid()&&GetTickCount64()>=next_discovery)
                    discovery=std::async(std::launch::async,[]{return lab::discover_automatic_games();});
            }
            if(preview_pending.valid()&&preview_pending.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
                try{auto preview=preview_pending.get();std::array<ComPtr<ID3D11ShaderResourceView>,4> made;
                    for(unsigned i=0;i<4;++i){D3D11_TEXTURE2D_DESC d{};d.Width=preview.width;d.Height=preview.height;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                        d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                        D3D11_SUBRESOURCE_DATA data{};data.pSysMem=preview.rgba[i].data();data.SysMemPitch=d.Width*4;ComPtr<ID3D11Texture2D> texture;
                        if(FAILED(device->CreateTexture2D(&d,&data,&texture))||FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&made[i])))throw std::runtime_error("Preview GPU upload failed");}
                    preview_views=std::move(made);for(unsigned i=0;i<4;++i)view.preview_textures[i]=static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(preview_views[i].Get()));
                    view.preview_width=preview.width;view.preview_height=preview.height;view.preview_original_width=preview.original_width;view.preview_original_height=preview.original_height;
                    view.preview_source=preview.source;view.preview_manifest=std::move(preview.manifest);view.preview_open=true;view.preview_zoom=1;view.preview_x=view.preview_y=.5f;view.page=1;
                }catch(const std::exception& e){view.preview_error=e.what();}view.preview_loading=false;
            }
            if (pending.valid() && pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                try {
                    auto response = pending.get();
                    if(view.status.value("session_id","")!=response.at("status").value("session_id","")){view.scene_confirmed=false;view.nr_scene_confirmed=false;}
                    view.status = response.at("status"); view.connected = true; view.received_ms=GetTickCount64();
                    view.error = response.value("ok", false) ? "" : response.at("error").dump();
                } catch (const std::exception& e) {
                    view.error = e.what();
                    view.connected = false;
                    view.status = lab::json::object();
                    view.scene_confirmed = false;
                    view.nr_scene_confirmed = false;
                }
                const auto nr_state=view.status.value("nr_frame_control",lab::json());
                const bool nr_wait=nr_state.is_object() && (nr_state.value("state","")=="requested" || nr_state.value("state","")=="applying");
                const auto prep=view.status.value("nr_preparation",lab::json());
                const bool preparing=prep.is_object()&&(prep.value("state","")=="requested"||prep.value("state","")=="preparing");
                next_poll = GetTickCount64() + ((view.status.value("pending",false) || nr_wait)?16:preparing?100:1000);
            }
            if(test_controller) test_controller->frame_boundary(frames,GetTickCount64());
            if(preparation_test){
                if(test_controller->take_nr_preparation(GetTickCount64()))preparation_failure_at=GetTickCount64()+500;
                if(preparation_failure_at&&GetTickCount64()>=preparation_failure_at){
                    test_controller->publish_host({{"backend","standalone-d3d12"},{"origin","synthetic"},{"state","failed"},{"error","界面故障测试：未执行 NR，拒绝继续调度。"}});
                    preparation_failure_at=0;
                }
            }
            if(capture_test)test_controller->observe_present(1);
            if(automatic_test&&test_controller->take_nr_preparation(GetTickCount64())){
                test_controller->enable_nr_frame_control("synthetic-input-real-nr",true,true);
                test_controller->publish_nr_runtime({{"profile","cyberpunk-rr-experimental-v1"},{"state","ready"},{"input_origin","ui-test-no-NR"},
                    {"settings",{{"requested_revision",0u},{"requested",{{"tone",1.f},{"structure",1.f}}},{"observed",nullptr}}}});
            }
            if(workbench_test||automatic_test){
                if(auto action=test_controller->take_nr_mode_request(GetTickCount64())){
                    auto runtime=test_controller->status()["nr_runtime"];
                    if(action->contains("settings")){++ui_setting_receipts;runtime["settings"]={{"requested",action->at("settings")},{"observed",action->at("settings")},
                        {"requested_revision",action->at("revision")},{"observed_revision",action->at("revision")},{"frame",static_cast<unsigned>(frames)},{"read_mask",3u},{"style_read",true}};
                        test_controller->publish_nr_runtime(runtime);}
                    const bool on=action->at("mode")=="on";test_controller->acknowledge_nr_mode(action->at("revision"),frames,true,on,on);
                }
                if(auto action=test_controller->take_pair_request(GetTickCount64())){
                    if(action->at("cancel")==true){if(ui_pair.is_object()){ui_pair["state"]="cancelled";test_controller->publish_pair(ui_pair);}}
                    else{++ui_pairs;ui_pair={{"revision",action->at("revision")},{"state","armed"},{"files",0},{"ui_fixture_no_image_capture",true}};test_controller->publish_pair(ui_pair);}
                }
            }
            if (view.connected && !pending.valid() && GetTickCount64() >= next_poll) send("GetStatus");
            if (resize_width && resize_height) {
                context->OMSetRenderTargets(0, nullptr, nullptr); target.Reset();
                if (FAILED(swapchain->ResizeBuffers(0, resize_width, resize_height, DXGI_FORMAT_UNKNOWN, 0)) || !make_target())
                    throw std::runtime_error("Console resize failed");
                resize_width = resize_height = 0;
            }
            auto scale = test_scale>0 ? test_scale : ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
            ImGui::GetStyle() = base_style; ImGui::GetStyle().ScaleAllSizes(scale); ImGui::GetStyle().FontScaleDpi = scale;
            ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame();
            bool interaction_issued=false;
            // A large real package may finish loading at this loop's start.
            // Wait until its first draw registered controls, not merely until
            // the background decoder set preview_open. Later missing widgets
            // still fail, and the existing test deadline remains enforced.
            const bool preview_drawn=!preview_test||test_step>0||test_phase>0||view.controls.contains("preview.nr");
            if(interaction_test && preview_drawn && (!preview_test||view.preview_open||test_phase>=3) && frames>2 && test_step<static_cast<int>(steps.size())) {
                interaction_issued=true;
                if(std::string(steps[test_step].id)=="escape") {
                    if(test_phase==1) io.AddKeyEvent(ImGuiKey_Escape,true);
                    if(test_phase==2) io.AddKeyEvent(ImGuiKey_Escape,false);
                } else if(test_phase<3) {
                // After mouse-up, only await the result. A valid state change
                // may remove the clicked button (e.g. Prepare -> preparing).
                auto it=view.controls.find(steps[test_step].id);
                if(it==view.controls.end()) throw std::runtime_error(std::string("UI test widget missing: ")+steps[test_step].id);
                const auto r=it->second;
                const auto a=view.control_clips.at(steps[test_step].id);
                if(test_phase==0 && !view.discovery_open && (r.y<a.y || r.y+r.w>a.y+a.w)) {
                    // Exercise actual wheel scrolling, rather than clicking a clipped item.
                    io.AddMousePosEvent(a.x+a.z*.5f,a.y+a.w*.5f);
                    io.AddMouseWheelEvent(0,r.y<a.y?2.f:-2.f);
                    interaction_issued=false;
                } else {
                // Parameter sliders span 0..2 since Live ABI22: their centre is the
                // default 1.0 and would change nothing, so they are pressed at 25%.
                const bool slider=std::string_view(steps[test_step].id).starts_with("parameter.");
                io.AddMousePosEvent(r.x+r.z*(slider?.25f:.5f),r.y+r.w*.5f);
                if(test_phase==1) io.AddMouseButtonEvent(0,true);
                if(test_phase==2) io.AddMouseButtonEvent(0,false);
                }
                }
            }
            ImGui::NewFrame(); view.pending=pending.valid();
            lab::draw_console(view,scale,send);
            if(interaction_test && interaction_issued) {
                if(++test_phase==4) {
                    if(steps[test_step].check()) {
                        test_phase=0; ++test_step;
                        if(test_step==static_cast<int>(steps.size())) quitting=true;
                    } else {
                        test_phase=3; // Wait for the real async response; never repeat the click.
                    }
                }
            }
            ImGui::Render();
            float clear[] = {14.f/255,17.f/255,20.f/255,1};
            auto rtv = target.Get(); context->OMSetRenderTargets(1, &rtv, nullptr); context->ClearRenderTargetView(rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if(smoke && !screenshot_saved && !screenshot.empty() &&
                !view.preview_loading&&(initial_pair.empty()||view.preview_open)&&
                (capture_step>=0 ? interaction_test && test_step==capture_step && test_phase==0 :
                    (frames>=4 && (connect_pid<=0 || view.connected)))) {
                save_render(screenshot); screenshot_saved=true;
            }
            if (FAILED(swapchain->Present(smoke ? 0 : 1, 0))) throw std::runtime_error("Console Present failed");
            ++frames;
            if (smoke && !interaction_test && frames>=5 && (connect_pid<=0 || view.connected)&&!view.preview_loading) quitting=true;
            if(smoke&&!view.preview_error.empty())throw std::runtime_error(view.preview_error);
            if(smoke && connect_pid>0 && !view.connected && GetTickCount64()>test_deadline)
                throw std::runtime_error("Observer connection smoke test timed out");
            if((synthetic_test||observer_test||capture_test||nr_test||preparation_test||workbench_test) && !view.error.empty()) throw std::runtime_error("Test backend response: "+view.error);
            if(interaction_test && !quitting && GetTickCount64()>test_deadline)
                throw std::runtime_error(std::string("UI test timed out: ")+steps[test_step].id);
            if(interaction_test) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (IsIconic(hwnd)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (pending.valid()) pending.wait();
        if(test_server) test_server->stop();
        if(workbench_test)lab::check(ui_setting_receipts==2&&ui_pairs==1,"UI-only protocol fixture: two parameter submissions, one capture arm; no real NR or image files");
        if(preparation_test)lab::check(preparation_requests==1&&!test_controller->status()["capabilities"]["nr_frame_control"].get<bool>(),"Exactly one confirmed Prepare; no synthetic claim of real NR execution");
        if(nr_test){
            lab::check(nr_test_results.size()==6 && nr_test_requests.size()==6,"Six real NR UI requests and acknowledgements required");
            std::ofstream out(nr_acceptance,std::ios::binary);
            out<<lab::json{{"purpose","functional-verification"},{"status","pass"},{"origin","external-real-nr-console-test"},
                {"backend_pid",connect_pid},{"requests",nr_test_requests},{"applied_statuses",nr_test_results},
                {"game_launched",false},{"p0_game_gate_open",false},{"executable",lab::module_identity(nullptr)}}.dump(2);
            out.close();lab::check(bool(out),"Cannot save real NR UI acceptance");
        }
        if(capture_test){
            lab::check(capture_test_results.size()==2,"Expected two accepted UI capture cycles");
            const auto root=std::filesystem::canonical(lab::root::resolve_self().data);lab::json removed=lab::json::array();
            for(const auto& result:capture_test_results){
                const auto file=std::filesystem::canonical(std::filesystem::path(lab::wide(result["directory"].get<std::string>()))/"callback-events.jsonl");
                lab::check(file.parent_path().parent_path()==root && file.parent_path().filename().wstring().starts_with(L"manual-capture-"),"UI fixture cleanup outside run scope");
                lab::check(result["recording_integrity"]=="complete" && lab::sha256(file)==result["stream_sha256"].get<std::string>(),"UI fixture source changed");
                removed.push_back({{"path",lab::utf8(file.wstring())},{"sha256",result["stream_sha256"]},{"bytes",std::filesystem::file_size(file)}});
                lab::check(std::filesystem::remove(file),"UI fixture raw cleanup failed");
            }
            const auto acceptance=root/lab::wide("console-capture-acceptance-"+lab::uuid()+".json");
            std::ofstream out(acceptance);out<<lab::json{{"purpose","functional-verification"},{"status","pass"},{"origin","synthetic"},
                {"ui_capture_cycles",2},{"results",capture_test_results},{"removed_raw_files",removed},{"raw_payload_available",false},
                {"p0_game_gate_open",false},{"executable",lab::module_identity(nullptr)}}.dump(2);
            lab::check(bool(out),"Cannot save UI capture acceptance");
        }
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
        target.Reset(); swapchain.Reset(); context.Reset(); device.Reset();
        DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, instance);
        CoUninitialize();
        return 0;
    } catch (const std::exception& e) {
        OutputDebugStringA(e.what());
        if (smoke) {
            const auto error=std::string(e.what())+"\n";
            DWORD written=0;
            WriteFile(GetStdHandle(STD_ERROR_HANDLE),error.data(),static_cast<DWORD>(error.size()),&written,nullptr);
        }
        if (!smoke) MessageBoxW(nullptr, lab::wide(e.what()).c_str(), L"Overglaze", MB_ICONERROR);
        return 1;
    }
}
