// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_overlay.hpp"
#include "lab_panel_hotkey_file.hpp"
#include <wrl/client.h>
#include <d3d12sdklayers.h>
#include <fstream>
#include <iostream>
#include <array>
#include <cmath>
#include <source_location>
using Microsoft::WRL::ComPtr;
namespace {
void need(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void need(bool v,const std::string& why){need(v,why.c_str());}
void hr(HRESULT v,const std::source_location where=std::source_location::current()){if(FAILED(v))throw std::runtime_error("GPU HRESULT "+std::to_string(unsigned(v))+" at overlay_harness.cpp:"+std::to_string(where.line()));}
unsigned key_events=0,insert_downs=0,control_ups=0,f10_messages=0;
LRESULT CALLBACK window_proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_KEYDOWN&&w=='K')++key_events;
    // What the "game" saw of the hotkey tests' keys.
    if(m==WM_KEYDOWN&&w==VK_INSERT)++insert_downs;if(m==WM_KEYUP&&w==VK_CONTROL)++control_ups;
    // F10 is counted and kept from DefWindowProc, which could enter menu mode.
    if((m>=WM_KEYFIRST&&m<=WM_KEYLAST)&&w==VK_F10){++f10_messages;return 0;}
    if(m==WM_CLOSE){DestroyWindow(h);return 0;}return DefWindowProcW(h,m,w,l);}
void barrier(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,0,a,b};c->ResourceBarrier(1,&v);}
struct Gpu {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12DescriptorHeap> rtv;
    lab::Handle event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};std::uint64_t value=0;
    Gpu(){hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));hr(list->Close());
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)));}
    void wait(){hr(queue->Signal(fence.Get(),++value));hr(fence->SetEventOnCompletion(value,event.value));need(WaitForSingleObject(event.value,5000)==WAIT_OBJECT_0,"GPU timeout");}
    void begin(){wait();hr(allocator->Reset());hr(list->Reset(allocator.Get(),nullptr));}
    void submit(){hr(list->Close());ID3D12CommandList* a[]{list.Get()};queue->ExecuteCommandLists(1,a);}
    void clear(IDXGISwapChain3* chain,float red=.14f,float green=.20f,float blue=.26f){ComPtr<ID3D12Resource> b;hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&b)));
        begin();auto h=rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(b.Get(),nullptr,h);barrier(list.Get(),b.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float color[]{red,green,blue,1};list->ClearRenderTargetView(h,color,0,nullptr);barrier(list.Get(),b.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);submit();}
    std::vector<std::uint8_t> read(IDXGISwapChain3* chain){ComPtr<ID3D12Resource> b;hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&b)));
        auto d=b->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT rows{};UINT64 bytes{},total{};device->GetCopyableFootprints(&d,0,1,0,&layout,&rows,&bytes,&total);
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=total;rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        begin();barrier(list.Get(),b.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION from{b.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION to{readback.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};to.PlacedFootprint=layout;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        barrier(list.Get(),b.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);submit();wait();void* p{};D3D12_RANGE r{0,SIZE_T(total)};hr(readback->Map(0,&r,&p));
        std::vector<std::uint8_t> data;data.reserve(SIZE_T(bytes*rows));for(unsigned y=0;y<rows;++y){auto* row=static_cast<std::uint8_t*>(p)+layout.Offset+y*layout.Footprint.RowPitch;data.insert(data.end(),row,row+bytes);}
        D3D12_RANGE none{0,0};readback->Unmap(0,&none);return data;}
};
void toggle(HWND w){SendMessageW(w,WM_KEYDOWN,VK_INSERT,0);SendMessageW(w,WM_KEYUP,VK_INSERT,1LL<<31);}
// One key as Windows delivers it: the thread's key state holds the modifiers
// while the messages are handled (GetKeyState reads exactly that), then
// everything is up again. F10 and Alt combinations arrive as WM_SYSKEY*.
void press(HWND w,UINT vk,bool ctrl=false,bool shift=false){
    BYTE keys[256]{};if(ctrl)keys[VK_CONTROL]=keys[VK_LCONTROL]=0x80;if(shift)keys[VK_SHIFT]=keys[VK_LSHIFT]=0x80;
    need(SetKeyboardState(keys)!=FALSE,"Set the thread's key state");
    const UINT down=vk==VK_F10?WM_SYSKEYDOWN:WM_KEYDOWN,up=vk==VK_F10?WM_SYSKEYUP:WM_KEYUP;
    const LPARAM scan=LPARAM(MapVirtualKeyW(vk,MAPVK_VK_TO_VSC))<<16;
    SendMessageW(w,down,vk,1|scan);SendMessageW(w,up,vk,1|scan|(1LL<<30)|(1LL<<31));
    BYTE released[256]{};SetKeyboardState(released);}
std::filesystem::path temp_base(){std::wstring v(32768,L'\0');const auto n=GetEnvironmentVariableW(L"OVERGLAZE_TEST_TEMP",v.data(),DWORD(v.size()));
    return n&&n<v.size()?std::filesystem::path(v.substr(0,n)):std::filesystem::temp_directory_path();}
}
int wmain(int argc,wchar_t** argv){
    const bool interactive=argc==1||(argc>1&&std::wstring(argv[1])==L"--interactive");HWND window{};std::unique_ptr<Gpu> diagnostic_gpu;lab::json result={{"purpose","functional-verification"},{"origin","synthetic-overlay-no-NR"},{"game_launched",false},{"nr_executed",false},{"raw_files",0},{"passed",false}};
    try{
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=window_proc;wc.lpszClassName=L"OverglazeOverlayFixture";RegisterClassW(&wc);
        window=CreateWindowW(wc.lpszClassName,L"Overglaze — 独立覆盖层测试（不运行 NR）",WS_OVERLAPPEDWINDOW,120,120,1400,940,nullptr,nullptr,wc.hInstance,nullptr);need(window!=nullptr,"Window creation");
        diagnostic_gpu=std::make_unique<Gpu>();auto& gpu=*diagnostic_gpu;ComPtr<IDXGIFactory4> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));DXGI_SWAP_CHAIN_DESC1 d{};d.Width=1280;d.Height=800;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BufferCount=3;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.SampleDesc.Count=1;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> first;hr(factory->CreateSwapChainForHwnd(gpu.queue.Get(),window,&d,nullptr,nullptr,&first));ComPtr<IDXGISwapChain3> chain;hr(first.As(&chain));first.Reset();
        // A data root of its own for the panel hotkey setting (no file yet: Insert);
        // the panel's other preferences stay read-only (no preferences path).
        const auto hotkey_root=temp_base()/("overglaze-overlay-hotkey-"+std::to_string(GetCurrentProcessId()));
        {std::error_code ignored;std::filesystem::remove_all(hotkey_root,ignored);std::filesystem::create_directories(hotkey_root);}
        const auto creation_queue=gpu.queue;lab::Controller controller;controller.enable_nr_preparation(true);controller.enable_embedded_control();lab::GameOverlay overlay(controller,{},hotkey_root);overlay.attach(chain.Get(),gpu.queue.Get());
        need(overlay.snapshot().at("hotkey")=="Insert"&&overlay.snapshot().at("hotkey_source")=="default","No setting file: Insert");
        result["profiles"]=lab::json::array();
        const DXGI_FORMAT formats[]{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R10G10B10A2_UNORM};
        const DXGI_COLOR_SPACE_TYPE spaces[]{DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020};
        for(unsigned i=0;i<3;++i){
            if(i){need(overlay.before_resize(chain.Get()),"Release on Resize");gpu.wait();hr(chain->ResizeBuffers(3,1280,800,formats[i],0));}
            overlay.color_space(chain.Get(),spaces[i]);gpu.clear(chain.Get());const auto baseline=gpu.read(chain.Get());
            const auto before=overlay.snapshot().at("draws");overlay.present(chain.Get());need(overlay.snapshot().at("draws")==before,"Hidden means no drawing");
            SendMessageW(window,WM_KEYDOWN,'K',0);const auto keys=key_events;toggle(window);SendMessageW(window,WM_KEYDOWN,'K',0);need(key_events==keys,"Open panel swallows key input");
            if(i==0){
                const auto unchanged=gpu.read(chain.Get());const auto submitted=overlay.snapshot().at("draws");
                overlay.present(chain.Get(),false,gpu.queue.Get());gpu.wait();
                need(overlay.snapshot().at("draws")==submitted&&gpu.read(chain.Get())==unchanged,"No per-frame permit means no buffer writes");
                ComPtr<ID3D12CommandQueue> other;D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
                hr(gpu.device->CreateCommandQueue(&q,IID_PPV_ARGS(&other)));
                overlay.present(chain.Get(),true,other.Get());gpu.wait();
                need(overlay.snapshot().at("draws")==submitted&&gpu.read(chain.Get())==unchanged,"A permit for a different queue cannot write the buffer");
                need(overlay.snapshot()["submission_evidence"]["proof_skipped"]==2,"Both refused permits are named in diagnostics");
            }
            for(unsigned frame=0;frame<5;++frame){gpu.clear(chain.Get());overlay.present(chain.Get(),true,gpu.queue.Get());gpu.wait();}
            auto status=overlay.snapshot();need(status.at("error")==""&&status.at("draws").get<unsigned>()>=before.get<unsigned>()+5,"Overlay must actually draw");
            const auto pixels=gpu.read(chain.Get());const unsigned stride=i==1?8:4;
            need(std::equal(pixels.end()-stride,pixels.end(),baseline.end()-stride),"Zero-alpha outside panel preserves original bits");
            need(pixels!=baseline,"Panel changed actual target");
            if(i==0){
                const auto bounds=status["controls"]["panel_bounds"];const float dpi=std::clamp(GetDpiForWindow(window)/96.f,1.f,2.5f);
                const auto px=unsigned(bounds[0].get<float>()+6*dpi),py=unsigned(bounds[1].get<float>()+bounds[3].get<float>()*.5f);
                const auto offset=4*(py*1280+px);need(offset+4<=pixels.size(),"Opaque background sample lies inside the target");
                gpu.clear(chain.Get(),.9f,.1f,.8f);overlay.present(chain.Get());gpu.wait();const auto alternate=gpu.read(chain.Get());
                need(std::equal(pixels.begin()+offset,pixels.begin()+offset+4,alternate.begin()+offset),"Panel background RGB is independent of the game underneath");
                result["opaque_background_verified"]=true;
                SendMessageW(window,WM_KILLFOCUS,0,0);
                need(overlay.visible()&&overlay.snapshot()["input_suspended"]==true&&!overlay.snapshot()["input_block_active"].get<bool>(),"Focus loss keeps panel visible and releases input");
                const auto before_focus_draws=overlay.snapshot().at("draws").get<unsigned>();gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                need(overlay.snapshot().at("draws").get<unsigned>()==before_focus_draws+1,"An unfocused panel can still be rendered for screenshots");
                const auto outside_keys=key_events;SendMessageW(window,WM_KEYDOWN,'K',0);need(key_events==outside_keys+1,"Unfocused panel does not consume keyboard input");key_events=outside_keys; // keep the later input-counter baseline independent of this check
                SendMessageW(window,WM_ACTIVATEAPP,TRUE,0);SendMessageW(window,WM_SETFOCUS,0,0);
                need(overlay.visible()&&!overlay.snapshot()["input_suspended"].get<bool>()&&overlay.snapshot()["input_block_active"].get<bool>(),"Returning focus restores input ownership with the panel still open");
                SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(1279,799));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                need(overlay.visible(),"Moving outside the panel within the game does not close it");
                result["focus_preserves_open_panel_verified"]=true;
                RECT client{};need(GetClientRect(window,&client)!=FALSE,"Read client size");
                need(client.right!=1280&&client.bottom!=800,"Exercise stretched buffer, not 1:1 client coordinates");
                auto point=[&](const lab::json& rect,float fraction){return POINT{LONG(std::lround((rect[0].get<float>()+rect[2].get<float>()*fraction)*client.right/1280.f)),LONG(std::lround((rect[1].get<float>()+rect[3].get<float>()*.5f)*client.bottom/800.f))};};
                for(bool on:{true,false}){
                    // Hit-test where the toggle is now. The panel's content follows the
                    // controller's state, and when it is about as tall as this 800 px
                    // surface its scrollbar (and so the toggle's x) settles a frame late.
                    for(unsigned settle=0;settle<2;++settle){gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();}
                    const auto p=point(overlay.snapshot()["controls"]["nr_toggle"],.05f);const auto x=p.x,y=p.y;
                    SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(x,y));SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                    SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(x,y));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                    need(controller.status()["nr_lifecycle"]["desired_mode"]==(on?"on":"off"),"Real ImGui checkbox updates local controller intent");}
                result["checkbox_input_verified"]=true;
                result["style_input_verified"]=lab::json::array();
                for(int selected:{2,1,0}){
                    // Controller state changes after drawing the checkbox. Let
                    // the next frame lay out its OFF status before hit-testing.
                    gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                    const auto hit=point(overlay.snapshot()["controls"]["Style "+std::to_string(selected)],.5f);
                    SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(hit.x,hit.y));SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(hit.x,hit.y));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                    SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(hit.x,hit.y));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                    need(controller.status()["nr_settings_request"]["values"]["style"]==selected,"Style button submits selected integer while OFF");
                    result["style_input_verified"].push_back(selected);}
                const auto slider=point(status["controls"]["tone"],.30f);
                SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(slider.x,slider.y));SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(slider.x,slider.y));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(slider.x,slider.y));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                const auto settings=controller.status()["nr_settings_request"]["values"];
                // ABI22 slider range 0..2: a click at 30% lands near 0.6.
                need(settings["tone"].get<float>()>.4f&&settings["tone"].get<float>()<.8f&&settings["structure"]==1.f&&settings["skin"]==1.f&&settings["automask"]==0,"Scaled Tone click changes Tone, never adjacent Structure or Skin");
                result["stretched_client_input_verified"]={{"client_width",client.right},{"client_height",client.bottom},{"buffer_width",1280},{"buffer_height",800},{"settings",settings}};
                // Edit extrapolation (Live ABI25): the 外推 toggle stages it, the 倍数
                // slider it reveals sets the factor in 1..4, and switching it off keeps
                // the factor. Each hit-test after the layout has settled.
                const auto click=[&](const char* control,float fraction){
                    for(unsigned settle=0;settle<2;++settle){gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();}
                    const auto controls=overlay.snapshot()["controls"];need(controls.contains(control),"Panel control laid out");
                    const auto hit=point(controls[control],fraction);
                    SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(hit.x,hit.y));SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(hit.x,hit.y));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                    SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(hit.x,hit.y));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                    return controller.status()["nr_settings_request"]["values"];};
                need(!overlay.snapshot()["controls"].contains("extrapolate_factor"),"No factor slider while extrapolation is off");
                auto extrapolation=click("extrapolate",.5f);
                need(extrapolation["extrapolate"]==1&&extrapolation["extrapolate_factor"]==2.f&&extrapolation["tone"]==settings["tone"],"外推 toggle stages extrapolation at the default factor");
                extrapolation=click("extrapolate_factor",.5f);
                const auto factor=extrapolation["extrapolate_factor"].get<float>();
                need(extrapolation["extrapolate"]==1&&factor>2.2f&&factor<2.8f,"倍数 slider spans 1..4 (a click at the middle lands near 2.5)");
                extrapolation=click("extrapolate",.5f);
                need(extrapolation["extrapolate"]==0&&extrapolation["extrapolate_factor"]==factor,"Switching extrapolation off keeps the factor");
                result["extrapolation_input_verified"]={{"factor_at_middle",factor}};
                // NR network preset (Live ABI26): the 网络 combo opens its list and an
                // entry stages that network, leaving every other value alone. No
                // model is loaded here, so the combo is enabled.
                need(extrapolation["network"]==0,"Network 0 (自动) by default");
                need(!overlay.snapshot()["controls"].contains("network_1"),"The network list stays closed until the combo is clicked");
                (void)click("network",.5f);
                auto network=click("network_2",.5f);
                need(network["network"]==2&&network["extrapolate"]==0&&network["extrapolate_factor"]==factor&&network["tone"]==settings["tone"],
                     "网络 entry 2 stages network 2; other settings kept");
                gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                need(!overlay.snapshot()["controls"].contains("network_1"),"Choosing an entry closes the list");
                (void)click("network",.5f);network=click("network_0",.5f);
                need(network["network"]==0,"网络 entry 自动 stages network 0");
                result["network_input_verified"]=true;
                // Compare split position (Live ABI27). The split controls sit low in the
                // panel, which on this 800 px surface may scroll: wheel the panel, as a
                // user would, until the control is fully in view, then use it.
                const auto reveal=[&](const char* control){
                    for(unsigned step=0;step<40;++step){
                        for(unsigned settle=0;settle<2;++settle){gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();}
                        const auto controls=overlay.snapshot()["controls"];need(controls.contains(control),"Panel control laid out");
                        const auto rect=controls[control];const float top=rect[1].get<float>(),bottom=top+rect[3].get<float>();
                        if(top>=0.f&&bottom<=792.f)return rect;
                        const LONG wx=LONG(200*client.right/1280),wy=LONG(400*client.bottom/800);POINT screen{wx,wy};ClientToScreen(window,&screen);
                        SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(wx,wy));
                        SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(short(top<0.f?WHEEL_DELTA:-WHEEL_DELTA))),MAKELPARAM(screen.x,screen.y));}
                    throw std::runtime_error(std::string("Panel never scrolled ")+control+" into view");};
                const auto press=[&](const POINT& p,WPARAM keys,UINT message){SendMessageW(window,message,keys,MAKELPARAM(p.x,p.y));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();};
                const auto values=[&]{return controller.status()["nr_settings_request"]["values"];};
                need(!overlay.snapshot()["controls"].contains("compare_split_position"),"No split position slider while the split is off");
                need(values()["compare_split_position"]==.5f,"Split in the middle by default");
                {const auto hit=point(reveal("compare_split"),.5f);press(hit,0,WM_MOUSEMOVE);press(hit,MK_LBUTTON,WM_LBUTTONDOWN);press(hit,0,WM_LBUTTONUP);}
                need(values()["compare_split"]==1&&values()["compare_split_position"]==.5f,"对比分屏 toggle stages the split at the middle");
                // Drag the slider from 20% to 80% with the button held. The panel sends
                // settings while a slider moves, at most every 100 ms: the new position
                // must reach the controller before the button is released.
                const auto split_slider=reveal("compare_split_position");
                const auto from=point(split_slider,.2f),to=point(split_slider,.8f);
                press(from,0,WM_MOUSEMOVE);press(from,MK_LBUTTON,WM_LBUTTONDOWN);
                Sleep(120);press({(from.x+to.x)/2,to.y},MK_LBUTTON,WM_MOUSEMOVE);
                Sleep(120);press(to,MK_LBUTTON,WM_MOUSEMOVE);
                Sleep(120);press(to,MK_LBUTTON,WM_MOUSEMOVE);
                const auto dragging=values()["compare_split_position"].get<float>();
                need(dragging>.7f&&dragging<.9f,"Dragging sends the split position before release (live, throttled to 100 ms)");
                press(to,0,WM_LBUTTONUP);
                const auto released=values()["compare_split_position"].get<float>();
                need(released>.7f&&released<.9f&&values()["compare_split"]==1&&values()["tone"]==settings["tone"],"Release keeps the dragged position; other settings untouched");
                {const auto hit=point(reveal("compare_split_center"),.5f);press(hit,0,WM_MOUSEMOVE);press(hit,MK_LBUTTON,WM_LBUTTONDOWN);press(hit,0,WM_LBUTTONUP);}
                need(values()["compare_split_position"]==.5f,"居中 puts the split back in the middle");
                {const auto hit=point(reveal("compare_split_position"),.25f);press(hit,0,WM_MOUSEMOVE);press(hit,MK_LBUTTON,WM_LBUTTONDOWN);press(hit,0,WM_LBUTTONUP);}
                const auto kept=values()["compare_split_position"].get<float>();need(kept>.15f&&kept<.35f,"A click at a quarter lands near 25%");
                {const auto hit=point(reveal("compare_split"),.5f);press(hit,0,WM_MOUSEMOVE);press(hit,MK_LBUTTON,WM_LBUTTONDOWN);press(hit,0,WM_LBUTTONUP);}
                need(values()["compare_split"]==0&&values()["compare_split_position"]==kept,"Switching the split off keeps the position");
                gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
                need(!overlay.snapshot()["controls"].contains("compare_split_position"),"The slider hides with the split off");
                result["compare_split_input_verified"]={{"while_dragging",dragging},{"after_release",released},{"kept_while_off",kept}};}
            result["profiles"].push_back({{"format",unsigned(formats[i])},{"space",unsigned(spaces[i])},{"outside_unchanged",true},{"overlay",status}});
            toggle(window);const auto hidden=overlay.snapshot().at("draws");overlay.present(chain.Get());need(overlay.snapshot().at("draws")==hidden,"Closing stops draws immediately");
            SendMessageW(window,WM_KEYDOWN,'K',0);need(key_events==keys+1,"Closing releases input");hr(chain->Present(0,0));gpu.wait();
        }
        // ---- The panel hotkey: rebinding in the panel (面板设置 → 修改), a change
        // made by the desktop program while the game runs, a malformed file, and
        // 恢复默认. The game must get the keys that are not the hotkey.
        {RECT client{};need(GetClientRect(window,&client)!=FALSE,"Hotkey client rect");
         const auto frame=[&]{gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();};
         const auto snapshot=[&]{frame();return overlay.snapshot();};
         const auto visible=[&]{return overlay.snapshot().at("visible").get<bool>();};
         const auto at=[&](const lab::json& rect){return POINT{LONG(std::lround((rect[0].get<float>()+rect[2].get<float>()*.5f)*client.right/1280.f)),LONG(std::lround((rect[1].get<float>()+rect[3].get<float>()*.5f)*client.bottom/800.f))};};
         // Scroll the panel, as a user would, until the control is fully in view; then click it.
         const auto click=[&](const char* control){
             for(unsigned step=0;;++step){
                 frame();frame();const auto controls=overlay.snapshot()["controls"];need(controls.contains(control),std::string("Hotkey control laid out: ")+control);
                 const auto rect=controls[control];const float top=rect[1].get<float>(),bottom=top+rect[3].get<float>();
                 if(top>=0.f&&bottom<=792.f){const auto hit=at(rect);
                     SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(hit.x,hit.y));frame();SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(hit.x,hit.y));frame();
                     SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(hit.x,hit.y));frame();return;}
                 need(step<40,std::string("Panel never scrolled into view: ")+control);
                 const LONG wx=LONG(200*client.right/1280),wy=LONG(400*client.bottom/800);POINT screen{wx,wy};ClientToScreen(window,&screen);
                 SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(wx,wy));SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(short(top<0.f?WHEEL_DELTA:-WHEEL_DELTA))),MAKELPARAM(screen.x,screen.y));}};
         const auto file=lab::hotkey::settings_file(hotkey_root);
         const auto follows=[&](const char* name,const char* source){for(unsigned i=0;i<150;++i){const auto s=snapshot();if(s.at("hotkey")==name&&s.at("hotkey_source")==source)return true;Sleep(20);}return false;};
         need(!visible(),"Panel closed before the hotkey tests");
         toggle(window);need(visible(),"Insert opens the panel");
         click("diagnostics");click("hotkey_change");
         need(snapshot().at("hotkey_capturing")==true,"修改 waits for a key");
         const auto games_insert=insert_downs;
         press(window,'A');auto s=snapshot();
         need(s.at("hotkey_capturing")==true&&s.at("hotkey_refusal")==lab::hotkey::refusal_text(lab::hotkey::Refusal::plain_character)&&s.at("hotkey")=="Insert","A letter alone is refused and the panel keeps waiting");
         press(window,VK_INSERT);s=snapshot();
         need(s.at("hotkey_capturing")==false&&s.at("hotkey")=="Insert"&&s.at("visible")==true&&insert_downs==games_insert,"While waiting, Insert is the capture's: chosen again, no toggle, nothing to the game");
         click("hotkey_change");need(snapshot().at("hotkey_capturing")==true,"修改 again");
         press(window,VK_F10,true);s=snapshot();
         need(s.at("hotkey_capturing")==false&&s.at("hotkey")=="Ctrl+F10"&&s.at("hotkey_source")=="file"&&s.at("hotkey_refusal")==""&&s.at("visible")==true,"Ctrl+F10 is taken; the panel stays open");
         {bool written=false;for(unsigned i=0;i<150&&!written;++i){written=lab::hotkey::read(hotkey_root).binding==lab::hotkey::Binding{VK_F10,true,false,false};if(!written)Sleep(20);}
          need(written,"The panel writes the setting file");}
         SendMessageW(window,WM_KEYDOWN,VK_ESCAPE,0);SendMessageW(window,WM_KEYUP,VK_ESCAPE,1LL<<31);need(!visible(),"Esc closes the panel");
         press(window,VK_INSERT);frame();need(!visible()&&insert_downs==games_insert+1,"After rebinding, Insert opens nothing and reaches the game");
         const auto ups=control_ups,f10=f10_messages;
         press(window,VK_F10,true);frame();
         need(visible()&&control_ups==ups+1&&f10_messages==f10,"Ctrl+F10 opens the panel: the game gets Ctrl's release and nothing of F10");
         press(window,VK_F10);frame();need(visible()&&f10_messages==f10,"F10 without Ctrl does not close it (the open panel owns the keyboard)");
         press(window,VK_F10,true);frame();need(!visible()&&f10_messages==f10,"Ctrl+F10 closes it");
         press(window,VK_F10);frame();need(!visible()&&f10_messages==f10+2,"A plain F10 is the game's, down and up");
         // The desktop program changes the setting while the game runs.
         lab::hotkey::write(hotkey_root,{VK_F9,false,true,false});
         need(follows("Shift+F9","file"),"A changed setting file is picked up");
         press(window,VK_F10,true);frame();need(!visible(),"The old binding no longer opens the panel");
         press(window,VK_F9,false,true);frame();need(visible(),"Shift+F9 opens it");
         press(window,VK_F9,false,true);frame();need(!visible(),"Shift+F9 closes it");
         {std::ofstream broken(file,std::ios::binary|std::ios::trunc);broken<<"{broken";}
         need(follows("Insert","malformed"),"A malformed setting file means Insert");
         need(overlay.snapshot().at("hotkey_error").get<std::string>().size()>0,"and the panel says why");
         lab::hotkey::write(hotkey_root,{VK_F10,true,false,false});need(follows("Ctrl+F10","file"),"Valid again");
         press(window,VK_F10,true);frame();need(visible(),"Ctrl+F10 opens the panel");
         click("hotkey_reset");
         need(snapshot().at("hotkey")=="Insert","恢复默认 sets Insert");
         {bool written=false;for(unsigned i=0;i<150&&!written;++i){written=lab::hotkey::read(hotkey_root).binding==lab::hotkey::kDefault&&lab::hotkey::read(hotkey_root).source==lab::hotkey::Source::file;if(!written)Sleep(20);}
          need(written,"and writes it");}
         click("diagnostics"); // fold 高级与诊断 again for the steps below
         toggle(window);need(!visible(),"Insert closes the panel again");
         result["hotkey_verified"]={{"captured","Ctrl+F10"},{"refused","A"},{"external_change","Shift+F9"},{"malformed","Insert"},{"reset","Insert"},
             {"game_got_control_release",true},{"game_got_plain_f10",true}};}
#ifdef LAB_OVERLAY_RESEARCH
        // Synthetic controller publication tests cancellation UI routing, not
        // GPU readback completion. Actual worker cancellation has its own tests.
        // Advertise the existing isolated-backend contract only; no runner is
        // instantiated here and the receipt explicitly records nr_executed=false.
        controller.enable_nr_frame_control("synthetic-input-real-nr",true,true);
        controller.publish_pair({{"state","writing"},{"revision",0}});
        toggle(window);const auto capture_draws=overlay.snapshot().at("draws").get<unsigned>();
        gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
        need(overlay.snapshot().at("capture_hidden").get<bool>()&&overlay.snapshot().at("draws")==capture_draws,"Capture hides drawing");
        const auto passed_keys=key_events;SendMessageW(window,WM_KEYDOWN,'K',0);need(key_events==passed_keys+1,"Capture-hidden panel does not invisibly capture input");
        toggle(window);gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();auto capture_panel=overlay.snapshot();
        need(capture_panel.at("capture_override").get<bool>()&&capture_panel.at("draws")==capture_draws+1&&capture_panel["controls"].contains("cancel_capture"),"Hotkey reopens capture controls");
        RECT capture_client{};need(GetClientRect(window,&capture_client)!=FALSE,"Capture client rect");auto rect=capture_panel["controls"]["cancel_capture"];
        // The panel (auto-sized, capped at the surface height) can be taller than
        // this 800 px surface; scroll it with the wheel, as a user would, until
        // the cancel button is fully visible.
        unsigned wheel_steps=0;
        for(;wheel_steps<20&&rect[1].get<float>()+rect[3].get<float>()>792.f;++wheel_steps){
            const LONG wx=LONG(200*capture_client.right/1280),wy=LONG(400*capture_client.bottom/800);POINT screen{wx,wy};ClientToScreen(window,&screen);
            SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(wx,wy));SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,WORD(-WHEEL_DELTA)),MAKELPARAM(screen.x,screen.y));
            gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();capture_panel=overlay.snapshot();rect=capture_panel["controls"]["cancel_capture"];}
        need(rect[1].get<float>()>=0.f&&rect[1].get<float>()+rect[3].get<float>()<=792.f,"Panel scrolls the cancel button into view");result["capture_scroll_steps"]=wheel_steps;
        const auto click_x=LONG((rect[0].get<float>()+rect[2].get<float>()*.5f)*capture_client.right/1280.f),click_y=LONG((rect[1].get<float>()+rect[3].get<float>()*.5f)*capture_client.bottom/800.f);
        SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(click_x,click_y));SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(click_x,click_y));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
        SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(click_x,click_y));gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
        auto cancellation=controller.take_pair_request(GetTickCount64());need(cancellation&&cancellation->value("cancel",false),"Actual cancel button queues cancellation");
        controller.publish_pair({{"state","cancelled"},{"revision",cancellation->at("revision")}});gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();toggle(window);
        // A terminal revision cannot be resurrected (Controller::publish_pair); a new capture is a new revision.
        controller.publish_pair({{"state","writing"},{"revision",cancellation->at("revision").get<std::uint64_t>()+1}});toggle(window);overlay.present(chain.Get());
        need(overlay.snapshot().at("capture_hidden").get<bool>(),"Second capture hidden");SendMessageW(window,WM_KEYDOWN,VK_ESCAPE,0);SendMessageW(window,WM_KEYUP,VK_ESCAPE,1LL<<31);overlay.present(chain.Get());
        cancellation=controller.take_pair_request(GetTickCount64());need(cancellation&&cancellation->value("cancel",false)&&!overlay.snapshot().at("visible").get<bool>(),"Escape cancels even before hidden Present early return");
        controller.publish_pair({{"state","cancelled"},{"revision",cancellation->at("revision")}});
        result["capture_controls_verified"]={{"synthetic_status",true},{"hidden_input_forwarded",true},{"hotkey_reopen",true},{"button_cancel",true},{"escape_cancel",true},{"gpu_capture_cancel_proven",false}};
#else
        // The controller panel drops the raw-capture UI. Advertise the capture
        // backend anyway and prove the panel neither shows it nor hides itself.
        controller.enable_nr_frame_control("synthetic-input-real-nr",true,true);
        controller.publish_pair({{"state","writing"},{"revision",0}});
        const auto before_capture=overlay.snapshot().at("draws").get<unsigned>();
        gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
        auto controller_panel=overlay.snapshot();
        need(controller_panel.at("draws").get<unsigned>()>=before_capture,"Controller panel keeps drawing while a capture is advertised");
        need(!controller_panel.contains("capture_hidden")&&!controller_panel.contains("capture_override"),"Controller status carries no capture field");
        need(!controller_panel.at("controls").contains("capture")&&!controller_panel.at("controls").contains("cancel_capture"),"Controller panel exposes no capture control");
        result["capture_controls_verified"]={{"variant","controller"},{"capture_ui_present",false},{"capture_status_fields",false}};
#endif
        // Exercise a real display-queue replacement, not only new dimensions.
        need(overlay.before_resize(chain.Get()),"Queue resize preparation");gpu.wait();
        ComPtr<ID3D12CommandQueue> next;D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr(gpu.device->CreateCommandQueue(&qd,IID_PPV_ARGS(&next)));
        IUnknown* present_queues[]{next.Get(),next.Get(),next.Get()};const UINT masks[]{1,1,1};
        hr(chain->ResizeBuffers1(3,1280,800,formats[0],0,masks,present_queues));ID3D12CommandQueue* native_queues[]{next.Get(),next.Get(),next.Get()};
        overlay.resized_queues(chain.Get(),native_queues);need(!overlay.snapshot().at("failed").get<bool>(),"New single Present queue admitted");gpu.queue=next;overlay.color_space(chain.Get(),spaces[0]);
        toggle(window);const auto queue_draws=overlay.snapshot().at("draws").get<unsigned>();gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();
        need(overlay.snapshot().at("draws").get<unsigned>()==queue_draws+1,"Overlay uses replacement queue");toggle(window);
        need(overlay.before_resize(chain.Get()),"Mixed queue test preparation");ComPtr<ID3D12CommandQueue> other;hr(gpu.device->CreateCommandQueue(&qd,IID_PPV_ARGS(&other)));
        ID3D12CommandQueue* mixed[]{next.Get(),other.Get(),next.Get()};overlay.resized_queues(chain.Get(),mixed);
        need(overlay.snapshot().at("failed").get<bool>(),"Rotating queues rejected");toggle(window);overlay.present(chain.Get());
        need(overlay.snapshot().at("draws").get<unsigned>()==queue_draws+1,"Unknown queue never renders on old queue");
        overlay.resized_queues(chain.Get(),native_queues);if(overlay.snapshot().at("visible").get<bool>())toggle(window);
        result["resize_queue_replacement_verified"]=true;result["rotating_queue_rejected"]=true;
        need(overlay.before_resize(chain.Get()),"Return to creation queue");gpu.wait();hr(chain->ResizeBuffers(3,1280,800,formats[0],0));overlay.resized_default_queue(chain.Get());gpu.queue=creation_queue;
        toggle(window);gpu.clear(chain.Get());overlay.present(chain.Get());hr(chain->Present(0,0));gpu.wait();toggle(window);
        result["plain_resize_restores_creation_queue"]=true;
        if(interactive){
            ShowWindow(window,SW_SHOWNOACTIVATE);toggle(window);const auto until=GetTickCount64()+180000;
            while(IsWindow(window)&&GetTickCount64()<until){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}if(!IsWindow(window))break;
                gpu.clear(chain.Get());overlay.present(chain.Get());hr(chain->Present(1,0));Sleep(8);}
            gpu.wait();}
        overlay.stop();ComPtr<ID3D12InfoQueue> info;hr(gpu.device.As(&info));lab::json errors=lab::json::array();
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<std::uint8_t> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,m,&size);
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)errors.push_back(std::string(m->pDescription,m->DescriptionByteLength));}
        result["debug_errors"]=errors;need(errors.empty(),"D3D12 debug errors");result["modules"]=lab::current_modules();result["passed"]=true;
        {std::error_code ignored;std::filesystem::remove_all(hotkey_root,ignored);}
        if(window&&IsWindow(window))DestroyWindow(window);
    }catch(const std::exception& e){result["error"]=e.what();if(window&&IsWindow(window))DestroyWindow(window);
        if(diagnostic_gpu){result["device_removed_reason"]=unsigned(diagnostic_gpu->device->GetDeviceRemovedReason());ComPtr<ID3D12InfoQueue> info;
            if(SUCCEEDED(diagnostic_gpu->device.As(&info))){result["debug_errors"]=lab::json::array();for(UINT64 i=0;i<info->GetNumStoredMessages()&&result["debug_errors"].size()<20;++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);if(size>65536)continue;std::vector<std::uint8_t> data(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());info->GetMessage(i,m,&size);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)result["debug_errors"].push_back(std::string(m->pDescription,m->DescriptionByteLength));}}}
        if(interactive){const auto path=std::filesystem::temp_directory_path()/("overglaze-overlay-desktop-failure-"+std::to_string(GetCurrentProcessId())+".json");
            if(!std::filesystem::exists(path)){std::ofstream f(path,std::ios::binary);f<<result.dump(2);}}
        if(interactive)MessageBoxW(nullptr,lab::wide(e.what()).c_str(),L"Overglaze 独立面板测试失败",MB_OK|MB_ICONERROR);}
    if(argc>2){std::filesystem::path out=argv[2];if(std::filesystem::exists(out)){std::cerr<<"Output exists\n";return 2;}std::ofstream stream(out,std::ios::binary);stream<<result.dump(2);}
    std::cout<<result.dump()<<'\n';return result.at("passed").get<bool>()?0:1;
}
#ifdef LAB_OVERLAY_DEMO
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){wchar_t name[]=L"lab_overlay_demo.exe";wchar_t* args[]{name};return wmain(1,args);}
#endif
