// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_overlay.hpp"
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
void hr(HRESULT v,const std::source_location where=std::source_location::current()){if(FAILED(v))throw std::runtime_error("GPU HRESULT "+std::to_string(unsigned(v))+" at overlay_harness.cpp:"+std::to_string(where.line()));}
unsigned key_events=0;
LRESULT CALLBACK window_proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_KEYDOWN&&w=='K')++key_events;if(m==WM_CLOSE){DestroyWindow(h);return 0;}return DefWindowProcW(h,m,w,l);}
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
    void clear(IDXGISwapChain3* chain){ComPtr<ID3D12Resource> b;hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&b)));
        begin();auto h=rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(b.Get(),nullptr,h);barrier(list.Get(),b.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float color[]{.14f,.20f,.26f,1};list->ClearRenderTargetView(h,color,0,nullptr);barrier(list.Get(),b.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);submit();}
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
}
int wmain(int argc,wchar_t** argv){
    const bool interactive=argc==1||(argc>1&&std::wstring(argv[1])==L"--interactive");HWND window{};std::unique_ptr<Gpu> diagnostic_gpu;lab::json result={{"purpose","functional-verification"},{"origin","synthetic-overlay-no-NR"},{"game_launched",false},{"nr_executed",false},{"raw_files",0},{"passed",false}};
    try{
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);ComPtr<ID3D12Debug> debug;hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=window_proc;wc.lpszClassName=L"OverglazeOverlayFixture";RegisterClassW(&wc);
        window=CreateWindowW(wc.lpszClassName,L"Overglaze — 独立覆盖层测试（不运行 NR）",WS_OVERLAPPEDWINDOW,120,120,1400,940,nullptr,nullptr,wc.hInstance,nullptr);need(window!=nullptr,"Window creation");
        diagnostic_gpu=std::make_unique<Gpu>();auto& gpu=*diagnostic_gpu;ComPtr<IDXGIFactory4> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));DXGI_SWAP_CHAIN_DESC1 d{};d.Width=1280;d.Height=800;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BufferCount=3;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.SampleDesc.Count=1;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> first;hr(factory->CreateSwapChainForHwnd(gpu.queue.Get(),window,&d,nullptr,nullptr,&first));ComPtr<IDXGISwapChain3> chain;hr(first.As(&chain));first.Reset();
        const auto creation_queue=gpu.queue;lab::Controller controller;controller.enable_nr_preparation(true);controller.enable_embedded_control();lab::GameOverlay overlay(controller);overlay.attach(chain.Get(),gpu.queue.Get());
        result["profiles"]=lab::json::array();
        const DXGI_FORMAT formats[]{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R10G10B10A2_UNORM};
        const DXGI_COLOR_SPACE_TYPE spaces[]{DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020};
        for(unsigned i=0;i<3;++i){
            if(i){need(overlay.before_resize(chain.Get()),"Release on Resize");gpu.wait();hr(chain->ResizeBuffers(3,1280,800,formats[i],0));}
            overlay.color_space(chain.Get(),spaces[i]);gpu.clear(chain.Get());const auto baseline=gpu.read(chain.Get());
            const auto before=overlay.snapshot().at("draws");overlay.present(chain.Get());need(overlay.snapshot().at("draws")==before,"Hidden means no drawing");
            SendMessageW(window,WM_KEYDOWN,'K',0);const auto keys=key_events;toggle(window);SendMessageW(window,WM_KEYDOWN,'K',0);need(key_events==keys,"Open panel swallows key input");
            for(unsigned frame=0;frame<5;++frame){gpu.clear(chain.Get());overlay.present(chain.Get());gpu.wait();}
            auto status=overlay.snapshot();need(status.at("error")==""&&status.at("draws").get<unsigned>()>=before.get<unsigned>()+5,"Overlay must actually draw");
            const auto pixels=gpu.read(chain.Get());const unsigned stride=i==1?8:4;
            need(std::equal(pixels.end()-stride,pixels.end(),baseline.end()-stride),"Zero-alpha outside panel preserves original bits");
            need(pixels!=baseline,"Panel changed actual target");
            if(i==0){RECT client{};need(GetClientRect(window,&client)!=FALSE,"Read client size");
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
                result["stretched_client_input_verified"]={{"client_width",client.right},{"client_height",client.bottom},{"buffer_width",1280},{"buffer_height",800},{"settings",settings}};}
            result["profiles"].push_back({{"format",unsigned(formats[i])},{"space",unsigned(spaces[i])},{"outside_unchanged",true},{"overlay",status}});
            toggle(window);const auto hidden=overlay.snapshot().at("draws");overlay.present(chain.Get());need(overlay.snapshot().at("draws")==hidden,"Closing stops draws immediately");
            SendMessageW(window,WM_KEYDOWN,'K',0);need(key_events==keys+1,"Closing releases input");hr(chain->Present(0,0));gpu.wait();
        }
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
