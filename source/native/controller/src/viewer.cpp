// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_capture_library.hpp"
#include "lab_viewer_color.hpp"
#include "lab_viewer_shader.hpp"
#include "lab_preview_export.hpp"
#include "lab_preconvert_reference.hpp"
#include "lab_ui_preferences.hpp"
#include "lab_display_selection.hpp"
#include "lab_display_white.hpp"
#include "lab_game_manager_ui.hpp"
#include "lab_brand.hpp"
#include "lab_product_ui.hpp"
#include "lab_root_locator.hpp"
#include "lab_model_setup.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <future>
#include <fstream>
#include <chrono>
#include <iostream>
#include <functional>
using Microsoft::WRL::ComPtr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace {
// The capture library is <Lab root>\data, the root taken from where this
// program sits or from overglaze-root.json beside it (lab_root_locator.hpp).
// Resolved in wWinMain right after the arguments, so a refusal reaches the
// start-up dialog (never in a smoke test). A fresh copy of the programs has no
// data directory yet: that one directory is created, nothing above it.
const std::filesystem::path& library_root(){static const auto data=[]{auto d=lab::root::resolve_self().data;std::error_code ec;std::filesystem::create_directory(d,ec);return d;}();return data;}
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void hr(HRESULT r){if(FAILED(r))throw std::runtime_error("Viewer HRESULT="+std::to_string(unsigned(r)));}
unsigned resize_width=0,resize_height=0;bool moved=true;
LRESULT CALLBACK procedure(HWND w,UINT m,WPARAM a,LPARAM b){
    if(ImGui_ImplWin32_WndProcHandler(w,m,a,b))return 1;
    if(m==WM_SIZE&&a!=SIZE_MINIMIZED){resize_width=LOWORD(b);resize_height=HIWORD(b);return 0;}
    if(m==WM_MOVE||m==WM_DISPLAYCHANGE)moved=true;
    if(m==WM_DPICHANGED){auto* r=reinterpret_cast<RECT*>(b);SetWindowPos(w,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    if(m==WM_GETMINMAXINFO){auto* info=reinterpret_cast<MINMAXINFO*>(b);const auto dpi=GetDpiForWindow(w);info->ptMinTrackSize={MulDiv(1024,dpi,96),MulDiv(680,dpi,96)};return 0;}
    if(m==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(w,m,a,b);
}
struct Renderer {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain3> chain;
    ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11PixelShader> ui_shader,image_shader,difference_shader;
    ComPtr<ID3D11Buffer> constant;ComPtr<ID3D11SamplerState> nearest,linear;std::array<ComPtr<ID3D11ShaderResourceView>,2> textures;
    std::array<int,2> loaded{-1,-1};bool hdr=false,hdr_available=false;std::wstring output_name;std::string notice;
    ComPtr<ID3D11ShaderResourceView> rr_reference;
    std::array<lab::color::Contract,4> contracts;
    HWND window=nullptr;unsigned width=0,height=0;float reported_peak=0;lab::viewer::Display display;bool pixel_nearest=true,ui_white_observed=false,processing_enabled=true;
    lab::viewer::Display viewing_request()const{return lab::viewer::viewing_request(display,processing_enabled);}
    ComPtr<IDXGIFactory1> outputs;std::uint64_t output_generation=0;
    std::wstring hdr_attempted_output;std::uint64_t hdr_attempted_generation=0;
    void make_target(){ComPtr<ID3D11Texture2D> buffer;hr(chain->GetBuffer(0,IID_PPV_ARGS(&buffer)));hr(device->CreateRenderTargetView(buffer.Get(),nullptr,&target));}
    Renderer(HWND window):window(window){D3D_FEATURE_LEVEL level;hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context));
        ComPtr<IDXGIDevice> dxgi;hr(device.As(&dxgi));ComPtr<IDXGIAdapter> adapter;hr(dxgi->GetAdapter(&adapter));ComPtr<IDXGIFactory2> factory;hr(adapter->GetParent(IID_PPV_ARGS(&factory)));
        RECT rect{};GetClientRect(window,&rect);width=rect.right;height=rect.bottom;DXGI_SWAP_CHAIN_DESC1 d{};d.Width=width;d.Height=height;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BufferCount=2;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.SampleDesc.Count=1;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> first;hr(factory->CreateSwapChainForHwnd(device.Get(),window,&d,nullptr,nullptr,&first));hr(first.As(&chain));hr(factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER));make_target();
        auto compile=[&](const char* entry,ComPtr<ID3D11PixelShader>& shader){ComPtr<ID3DBlob> blob,errors;const auto result=D3DCompile(lab::viewer::shader,sizeof(lab::viewer::shader)-1,nullptr,nullptr,nullptr,entry,"ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_IEEE_STRICTNESS,0,&blob,&errors);
            if(FAILED(result))throw std::runtime_error(errors?std::string(static_cast<char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"Viewer shader compilation");hr(device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&shader));};
        compile("PSUI",ui_shader);compile("PSImage",image_shader);compile("PSDifference",difference_shader);
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(display);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;hr(device->CreateBuffer(&cb,nullptr,&constant));
        D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
        hr(device->CreateSamplerState(&sampler,&nearest));sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;hr(device->CreateSamplerState(&sampler,&linear));inspect_output();
    }
    void inspect_output(){hdr_available=false;reported_peak=0;output_name.clear();display.ui_white_nits=80;ui_white_observed=false;
        if(!outputs||!outputs->IsCurrent()){outputs.Reset();if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&outputs))))return;++output_generation;}
        RECT bounds{};if(!GetWindowRect(window,&bounds)||IsIconic(window))return;std::uint64_t largest=0;
        for(unsigned a=0;a<32;++a){ComPtr<IDXGIAdapter1> adapter;if(outputs->EnumAdapters1(a,&adapter)==DXGI_ERROR_NOT_FOUND)break;if(!adapter)continue;
            for(unsigned i=0;i<32;++i){ComPtr<IDXGIOutput> output;if(adapter->EnumOutputs(i,&output)==DXGI_ERROR_NOT_FOUND)break;if(!output)continue;ComPtr<IDXGIOutput6> advanced;if(FAILED(output.As(&advanced)))continue;
                DXGI_OUTPUT_DESC1 d{};if(FAILED(advanced->GetDesc1(&d))||!d.AttachedToDesktop)continue;const auto area=lab::display_intersection(bounds,d.DesktopCoordinates);if(area<=largest)continue;
                largest=area;output_name=d.DeviceName;reported_peak=d.MaxLuminance;hdr_available=d.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;}}
        const auto white=lab::display_sdr_white(output_name);display.ui_white_nits=white.nits;ui_white_observed=white.observed;}
    void resize(unsigned w,unsigned h,bool use_hdr){if(!w||!h)return;context->OMSetRenderTargets(0,nullptr,nullptr);target.Reset();
        const auto format=use_hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;hr(chain->ResizeBuffers(0,w,h,format,0));width=w;height=h;
        const auto space=use_hdr?DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;UINT supported=0;hr(chain->CheckColorSpaceSupport(space,&supported));
        if(!(supported&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)||FAILED(chain->SetColorSpace1(space))){if(use_hdr){notice="当前窗口无法启用 scRGB，已回退 SDR。";resize(w,h,false);return;}throw std::runtime_error("SDR swapchain color space unavailable");}
        hdr=use_hdr;make_target();}
    void apply_hdr_preference(bool preferred,bool explicit_change=false){if(explicit_change)hdr_attempted_output.clear();
        if(!preferred||!hdr_available){if(hdr)resize(width,height,false);hdr_attempted_output.clear();return;}
        if(!hdr&&(hdr_attempted_output!=output_name||hdr_attempted_generation!=output_generation)){hdr_attempted_output=output_name;hdr_attempted_generation=output_generation;resize(width,height,true);}}
    std::array<float,3> read_pixel(unsigned x,unsigned y){ComPtr<ID3D11Texture2D> buffer,readback;hr(chain->GetBuffer(0,IID_PPV_ARGS(&buffer)));D3D11_TEXTURE2D_DESC d{};buffer->GetDesc(&d);
        need(x<d.Width&&y<d.Height,"QA pixel outside window");d.Width=d.Height=1;d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;hr(device->CreateTexture2D(&d,nullptr,&readback));
        const D3D11_BOX box{x,y,0,x+1,y+1,1};context->CopySubresourceRegion(readback.Get(),0,0,0,0,buffer.Get(),0,&box);D3D11_MAPPED_SUBRESOURCE mapped{};hr(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
        std::array<float,3> actual{};for(unsigned c=0;c<3;++c)actual[c]=hdr?lab::preconvert::unhalf(static_cast<const std::uint16_t*>(mapped.pData)[c]):static_cast<const unsigned char*>(mapped.pData)[c]/255.f;context->Unmap(readback.Get(),0);return actual;}
    void upload(const lab::RawCapture& capture,int left,int right){const int wanted[]{left,right};
        for(unsigned slot=0;slot<2;++slot){if(loaded[slot]==wanted[slot])continue;contracts[wanted[slot]]=capture.color_contract(wanted[slot]);need(contracts[wanted[slot]].known(),"Source color interpretation is missing or unknown; viewing refused");textures[slot].Reset();D3D11_TEXTURE2D_DESC d{};d.Width=capture.width();d.Height=capture.height();d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT(capture.format(wanted[slot]));
            d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA pixels{};pixels.pSysMem=capture.data(wanted[slot]);pixels.SysMemPitch=capture.row_pitch(wanted[slot]);
            ComPtr<ID3D11Texture2D> texture;hr(device->CreateTexture2D(&d,&pixels,&texture));hr(device->CreateShaderResourceView(texture.Get(),nullptr,&textures[slot]));loaded[slot]=wanted[slot];}
        if(!rr_reference&&(contracts[left].reconstruction_available||contracts[right].reconstruction_available)){
            D3D11_TEXTURE2D_DESC d{};d.Width=capture.width();d.Height=capture.height();d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT(capture.format(0));d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA pixels{capture.data(0),capture.row_pitch(0),0};ComPtr<ID3D11Texture2D> texture;
            hr(device->CreateTexture2D(&d,&pixels,&texture));hr(device->CreateShaderResourceView(texture.Get(),nullptr,&rr_reference));}}
    void clear_images(){context->PSSetShaderResources(0,3,std::array<ID3D11ShaderResourceView*,3>{nullptr,nullptr,nullptr}.data());textures={};rr_reference.Reset();loaded={-1,-1};}
    void bind(unsigned kind,unsigned stage,bool use_nearest){auto applied=kind==1?viewing_request():display;applied.hdr=hdr;
        if(kind==1)applied=lab::color::effective(contracts[stage],applied);
        if(kind==1&&applied.nr_reconstruct&&!use_nearest)applied.nr_reconstruct|=2; // GPU-only resampling bit, not a persisted recipe.
        if(kind==2)need(lab::color::comparable(contracts[loaded[0]],contracts[loaded[1]]),"Raw difference across incompatible color stages refused");
        context->UpdateSubresource(constant.Get(),0,nullptr,&applied,0,0);ID3D11Buffer* cb=constant.Get();context->PSSetConstantBuffers(0,1,&cb);
        context->PSSetShader(kind==0?ui_shader.Get():kind==2?difference_shader.Get():image_shader.Get(),nullptr,0);
        auto* sampler=kind==0||!use_nearest?linear.Get():nearest.Get();context->PSSetSamplers(0,1,&sampler);auto* other=kind==2?textures[1].Get():nullptr;context->PSSetShaderResources(1,1,&other);
        auto* reference=kind==1&&applied.nr_reconstruct?rr_reference.Get():nullptr;need(!(kind==1&&applied.nr_reconstruct)||reference,"Display RR reference not loaded");context->PSSetShaderResources(2,1,&reference);}
    struct Draw {Renderer* renderer;unsigned kind,stage;bool nearest=false;};
    static void callback(const ImDrawList*,const ImDrawCmd* cmd){const auto* draw=static_cast<const Draw*>(cmd->UserCallbackData);draw->renderer->bind(draw->kind,draw->stage,draw->nearest);}
    lab::json verify_background(){ // Bounded in-memory QA only, never user capture.
        ComPtr<ID3D11Texture2D> buffer,readback;hr(chain->GetBuffer(0,IID_PPV_ARGS(&buffer)));D3D11_TEXTURE2D_DESC d{};buffer->GetDesc(&d);
        d.Width=d.Height=1;d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;hr(device->CreateTexture2D(&d,nullptr,&readback));
        const D3D11_BOX box{2,2,0,3,3,1};context->CopySubresourceRegion(readback.Get(),0,0,0,0,buffer.Get(),0,&box);D3D11_MAPPED_SUBRESOURCE mapped{};hr(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
        std::array<float,3> actual{},expected{};const auto color=ImGui::GetStyle().Colors[ImGuiCol_WindowBg];const float components[]{color.x,color.y,color.z};
        for(unsigned c=0;c<3;++c){actual[c]=hdr?lab::preconvert::unhalf(static_cast<const std::uint16_t*>(mapped.pData)[c]):static_cast<const unsigned char*>(mapped.pData)[c]/255.f;
            const auto quantized=std::round(components[c]*255)/255;expected[c]=float(hdr?lab::viewer::srgb_decode(quantized)*display.ui_white_nits/80:quantized);}
        context->Unmap(readback.Get(),0);for(unsigned c=0;c<3;++c)need(std::abs(actual[c]-expected[c])<(hdr?.0001f:.004f),"Viewer background bypassed display shader");
        return {{"hdr",hdr},{"actual",actual},{"expected",expected},{"passed",true}};
    }
};
struct View {
    HWND window;Renderer& gpu;std::shared_ptr<lab::RawCapture> raw;std::future<std::shared_ptr<lab::RawCapture>> loading;
    lab::CaptureCatalog catalog;std::future<lab::CaptureCatalog> scanning;
    std::future<std::filesystem::path> exporting;std::filesystem::path last_export;
    int left=0,right=3;bool difference=false,wipe=false,wipe_drag=false,fit=true,library_visible=true,hdr_preferred=false;float wipe_position=.5f,zoom=1,center_x=.5f,center_y=.5f,render_center_x=.5f,render_center_y=.5f;int hover_x=-1,hover_y=-1;
    char filter[128]{};std::string error,selected,detail,preference_error;std::uint64_t interactions=0;lab::json controls=lab::json::object();
    bool test_folder_dispatch=false,prefer_interface=false,prefer_processing=false;std::filesystem::path last_folder;
    bool games_page=false,manager_allowed=true;std::unique_ptr<lab::GameManagerPage> manager;
    bool about=false;std::string about_error;std::future<lab::games::ModelStatus> model_job;lab::games::ModelStatus model;bool model_checked=false;
    std::unique_ptr<lab::ModelSetupPreferences> model_setup;
    bool setup_requested=false,setup_importing=false,startup_model_check=false;std::string setup_error;
    Renderer::Draw ui{&gpu,0,0},a{&gpu,1,1},b{&gpu,1,2},delta{&gpu,2,1};
    lab::ViewerPreferences preferences()const{lab::ViewerPreferences p;p.display=gpu.display;p.processing=gpu.processing_enabled;p.nearest=gpu.pixel_nearest;p.library=library_visible;p.hdr=hdr_preferred;p.view=difference?2u:wipe?1u:0u;p.wipe=wipe_position;return p;}
    void restore(const lab::ViewerPreferences& p){const auto ui_white=gpu.display.ui_white_nits;gpu.display=p.display;gpu.processing_enabled=p.processing;gpu.display.ui_white_nits=ui_white;gpu.pixel_nearest=p.nearest;library_visible=p.library;hdr_preferred=p.hdr;difference=p.view==2;wipe=p.view==1;wipe_position=p.wipe;}
    void mark(const char* id){auto p=ImGui::GetItemRectMin(),q=ImGui::GetItemRectMax();controls[id]={p.x,p.y,q.x-p.x,q.y-p.y};}
    void scan(){if(!scanning.valid())scanning=std::async(std::launch::async,[]{return lab::scan_capture_library(library_root());});}
    void folder(const std::filesystem::path& manifest){try{const auto path=lab::capture_directory(manifest);
        if(!test_folder_dispatch){const auto result=ShellExecuteW(window,L"explore",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);need(reinterpret_cast<INT_PTR>(result)>32,"Windows 无法打开采集目录");}
        last_folder=path;++interactions;
    }catch(const std::exception& e){error=std::string("打开所在文件夹失败：")+e.what();}}
    void open(const std::filesystem::path& file){if(loading.valid())return;error.clear();loading=std::async(std::launch::async,[file]{return std::make_shared<lab::RawCapture>(file);});}
    // The user's "allow unrecognized models" setting (Settings), off by default.
    bool allow_unrecognized()const{return model_setup&&model_setup->state.allow_unrecognized;}
    void check_model(bool startup=false){if(model_job.valid()||!manager_allowed)return;startup_model_check=startup;setup_importing=false;
        const auto root=library_root().parent_path();const bool allow=allow_unrecognized();
        model_job=std::async(std::launch::async,[root,allow]{lab::games::Manager m(root);m.allow_unrecognized_model(allow);return m.model_status();});}
    void start_model_setup(){if(!manager_allowed)return;setup_error.clear();setup_requested=true;if(!model_checked)check_model();}
    void select_model(){if(!manager_allowed||model_job.valid())return;
        std::vector<wchar_t> path(32768);OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=window;
        dialog.lpstrFilter=L"NVIDIA NR model (nvngx_dlssnr.dll)\0nvngx_dlssnr.dll\0DLL files (*.dll)\0*.dll\0\0";
        dialog.lpstrFile=path.data();dialog.nMaxFile=DWORD(path.size());dialog.lpstrTitle=L"选择 NR 模型";
        dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT|OFN_EXPLORER;
        if(!GetOpenFileNameW(&dialog)){const auto code=CommDlgExtendedError();if(code)setup_error="无法打开文件选择窗口（Windows "+std::to_string(code)+"）";return;}
        const std::filesystem::path selected_model(path.data());const auto root=library_root().parent_path();setup_error.clear();setup_importing=true;startup_model_check=false;
        const bool allow=allow_unrecognized();
        model_job=std::async(std::launch::async,[root,selected_model,allow]{lab::games::Manager m(root);m.allow_unrecognized_model(allow);return m.import_model(selected_model);});}
    void poll(){using namespace std::chrono_literals;
        if(model_job.valid()&&model_job.wait_for(0ms)==std::future_status::ready){const bool imported=setup_importing;
            try{auto checked=model_job.get();if(imported&&!checked.error.empty())setup_error=checked.error;else{model=std::move(checked);model_checked=true;
                    if(imported&&model.usable()){if(model_setup)model_setup->remember(lab::ModelSetupState::Decision::completed);manager.reset();}}}
            catch(const std::exception& e){if(imported)setup_error=e.what();else{model={};model.error=e.what();model_checked=true;}}
            setup_importing=false;
            if(startup_model_check&&model_setup&&model_setup->state.should_prompt(manager_allowed,model_checked,model.usable()))setup_requested=true;
            startup_model_check=false;}
        if(scanning.valid()&&scanning.wait_for(0ms)==std::future_status::ready){try{catalog=scanning.get();}catch(const std::exception& e){error=e.what();}}
        if(exporting.valid()&&exporting.wait_for(0ms)==std::future_status::ready){try{last_export=exporting.get();}catch(const std::exception& e){error=e.what();}}
        if(loading.valid()&&loading.wait_for(0ms)==std::future_status::ready){try{auto next=loading.get();for(unsigned i=0;i<next->stage_count();++i)need(next->color_contract(i).known(),"颜色解释缺失或未知：不自动套用显示曲线；原始记录仍保留在采集库。");
            gpu.clear_images();raw=std::move(next);left=0;right=raw->display_pair()?1:3;difference=wipe=false;gpu.display.nr_reconstruct=0;
            if(prefer_interface&&!raw->display_pair()){left=1;right=2;gpu.display.nr_linear=0;}
            if(prefer_processing&&!raw->display_pair()&&raw->color_contract(left).reconstruction_available&&raw->color_contract(right).reconstruction_available){gpu.display.nr_reconstruct=1;gpu.processing_enabled=true;}
            if(raw->display_pair()){hdr_preferred=false;gpu.apply_hdr_preference(false);}
            selected=lab::utf8(raw->path().wstring());fit=true;center_x=center_y=.5f;gpu.upload(*raw,left,right);}catch(const std::exception& e){error=e.what();}}
    }
    void set_pair(int l,int r){if(raw)need(l>=0&&r>=0&&unsigned(l)<raw->stage_count()&&unsigned(r)<raw->stage_count(),"Pair outside capture");left=l;right=r;if(raw)gpu.upload(*raw,left,right);++interactions;}
    lab::json verify_wipe(){need(raw&&wipe&&!difference&&fit,"Wipe QA requires fitted image");const auto r=controls.at("image.left");const float width=r[2],height=r[3],scale=std::min(width/raw->width(),height/raw->height());
        const float x0=r[0].get<float>()+(width-raw->width()*scale)*.5f,y0=r[1].get<float>()+(height-raw->height()*scale)*.5f;lab::json checks=lab::json::array();
        for(unsigned side=0;side<2;++side){const float u=side?.75f:.25f;const auto x=unsigned(x0+raw->width()*scale*u),y=unsigned(y0+raw->height()*scale*.5f);auto display=gpu.viewing_request();display.hdr=gpu.hdr;display.stage=side?right:left;
            const auto expected=lab::viewer::display_pixel(raw->pixel(display.stage,std::min(unsigned(raw->width()*u),raw->width()-1),raw->height()/2),lab::color::effective(raw->color_contract(display.stage),display));const auto actual=gpu.read_pixel(x,y);
            for(unsigned c=0;c<3;++c)need(std::abs(actual[c]-expected[c])<.01f,"Wipe selected wrong source or changed pixel alignment");checks.push_back({{"stage",display.stage},{"actual",actual},{"expected",expected}});}
        return checks;}
    lab::json verify_final_codes(){
        need(raw&&raw->display_pair()&&!gpu.hdr&&!fit&&zoom==1&&!wipe&&!difference,"Final code QA requires 1:1 SDR");
        const auto r=controls.at("image.right");const float rw=r[2],rh=r[3],w=std::min(rw,float(raw->width())),h=std::min(rh,float(raw->height()));
        const float x0=r[0].get<float>()+(rw-w)*.5f,y0=r[1].get<float>()+(rh-h)*.5f;
        const float u=w/raw->width(),v=h/raw->height(),cx=std::clamp(render_center_x,u*.5f,1-u*.5f),cy=std::clamp(render_center_y,v*.5f,1-v*.5f);
        lab::json checks=lab::json::array();
        for(unsigned i=0;i<16;++i){const auto x=unsigned(x0+w*(i+.5f)/16),y=unsigned(y0+h*.5f);
            const auto sx=std::min(unsigned(std::floor((cx-u*.5f)*raw->width())+x+.5f-x0),raw->width()-1),sy=std::min(unsigned(std::floor((cy-v*.5f)*raw->height())+y+.5f-y0),raw->height()-1);
            const auto original=raw->bits(1,sx,sy);const auto actual=gpu.read_pixel(x,y);
            for(unsigned c=0;c<3;++c)if(std::lround(actual[c]*255)!=original[c])throw std::runtime_error("Viewer final SDR code changed: "+lab::json{{"window",{x,y}},{"source",{sx,sy}},{"expected",original},{"actual",actual},{"origin",{x0,y0}},{"size",{w,h}},{"stage",unsigned(gpu.contracts[right].stage)},{"nearest",gpu.pixel_nearest},{"display",preferences().document()}}.dump());
            checks.push_back({{"source_pixel",{sx,sy}},{"original_codes",original},{"window_rgb",actual}});
        }return {{"mapping_request",gpu.display.mapping},{"exposure_request",gpu.display.exposure},{"pixel_checks",checks}};
    }
    lab::json verify_processing_pixels(){
        need(raw&&!raw->display_pair()&&!gpu.hdr&&!fit&&zoom==1&&!wipe&&!difference,"Processing QA needs 1:1 SDR NR pair");
        lab::json checks=lab::json::array();
        for(unsigned side=0;side<2;++side){const auto r=controls.at(side?"image.right":"image.left");
            const float rw=r[2],rh=r[3],w=std::min(rw,float(raw->width())),h=std::min(rh,float(raw->height()));
            const float x0=r[0].get<float>()+(rw-w)*.5f,y0=r[1].get<float>()+(rh-h)*.5f;
            const float u=w/raw->width(),v=h/raw->height(),cx=std::clamp(render_center_x,u*.5f,1-u*.5f),cy=std::clamp(render_center_y,v*.5f,1-v*.5f);
            const unsigned stage=side?right:left;const auto applied=lab::color::effective(raw->color_contract(stage),gpu.viewing_request());
            for(unsigned i=0;i<16;++i){const auto x=unsigned(x0+w*(i+.5f)/16),y=unsigned(y0+h*.5f);
                const auto sx=std::min(unsigned(std::floor((cx-u*.5f)*raw->width())+x+.5f-x0),raw->width()-1),sy=std::min(unsigned(std::floor((cy-v*.5f)*raw->height())+y+.5f-y0),raw->height()-1);
                const auto expected=lab::viewer::display_pixel(raw->pixel(stage,sx,sy),applied,raw->pixel(0,sx,sy));const auto actual=gpu.read_pixel(x,y);
                for(unsigned c=0;c<3;++c)need(std::abs(actual[c]-expected[c])<=1.5f/255,"Processing window pixels disagree with raw/reference CPU recipe");
                checks.push_back({{"stage",stage},{"source_pixel",{sx,sy}},{"actual",actual},{"expected",expected}});
            }
        }return {{"nr_reconstruct",gpu.viewing_request().nr_reconstruct},{"mapping",gpu.viewing_request().mapping},{"processing_enabled",gpu.processing_enabled},{"pixels",checks},{"same_raw_textures",gpu.loaded==std::array<int,2>{1,2}}};
    }
    void image(unsigned slot,ImVec2 available){if(!raw)return;auto* list=ImGui::GetWindowDrawList();const auto origin=ImGui::GetCursorScreenPos();available.x=std::max(1.f,available.x);available.y=std::max(1.f,available.y);
        const float scale=fit?std::min(available.x/raw->width(),available.y/raw->height()):zoom;
        const float actual_w=std::min(available.x,raw->width()*scale),actual_h=std::min(available.y,raw->height()*scale);
        const float u=actual_w/(raw->width()*scale),v=actual_h/(raw->height()*scale);const auto cx=std::clamp(render_center_x,u*.5f,1-u*.5f),cy=std::clamp(render_center_y,v*.5f,1-v*.5f);
        ImVec2 first{cx-u*.5f,cy-v*.5f};
        // An odd-sized viewport centered on an even-sized source otherwise
        // samples exactly between texels at 1:1 (precision-dependent ±1 pixel).
        // Snap the source origin for nearest-neighbor inspection, not the data.
        if(gpu.pixel_nearest&&scale>=1){first.x=std::floor(first.x*raw->width())/raw->width();first.y=std::floor(first.y*raw->height())/raw->height();}
        const ImVec2 last{first.x+u,first.y+v};
        const ImVec2 start{origin.x+(available.x-actual_w)*.5f,origin.y+(available.y-actual_h)*.5f},end{start.x+actual_w,start.y+actual_h};
        list->AddRectFilled(origin,{origin.x+available.x,origin.y+available.y},IM_COL32(7,9,10,255));
        auto paint=[&](unsigned index,Renderer::Draw& draw){draw.stage=index?right:left;draw.nearest=gpu.pixel_nearest&&scale>=1;list->AddCallback(Renderer::callback,&draw);
            list->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(gpu.textures[index].Get())),start,end,first,last);list->AddCallback(Renderer::callback,&ui);};
        const auto divide=start.x+actual_w*wipe_position;
        if(wipe&&!difference){list->PushClipRect(start,{divide,end.y},true);paint(0,a);list->PopClipRect();list->PushClipRect({divide,start.y},end,true);paint(1,b);list->PopClipRect();
            list->AddLine({divide,start.y},{divide,end.y},IM_COL32(180,225,110,255),2);list->AddRectFilled({divide-5,start.y+actual_h*.5f-16},{divide+5,start.y+actual_h*.5f+16},IM_COL32(118,185,0,255),3);}
        else paint(slot,difference?delta:(slot?b:a));
        ImGui::InvisibleButton(slot?"##right-image":"##left-image",available,ImGuiButtonFlags_MouseButtonLeft);mark(slot?"image.right":"image.left");
        if(ImGui::IsItemHovered()||ImGui::IsItemActive()){
            auto& io=ImGui::GetIO();if(io.MouseWheel){zoom=std::clamp(scale*std::pow(1.2f,io.MouseWheel),.05f,32.f);fit=false;++interactions;}
            if(ImGui::IsItemClicked())wipe_drag=wipe&&!difference&&std::abs(io.MousePos.x-divide)<12;
            if(ImGui::IsMouseDragging(0)){if(wipe_drag)wipe_position=std::clamp((io.MousePos.x-start.x)/actual_w,0.f,1.f);else{center_x=cx-io.MouseDelta.x/(raw->width()*scale);center_y=cy-io.MouseDelta.y/(raw->height()*scale);}++interactions;}
            if(io.MousePos.x>=start.x&&io.MousePos.x<end.x&&io.MousePos.y>=start.y&&io.MousePos.y<end.y){hover_x=std::clamp(int((first.x+(io.MousePos.x-start.x)/actual_w*u)*raw->width()),0,int(raw->width())-1);hover_y=std::clamp(int((first.y+(io.MousePos.y-start.y)/actual_h*v)*raw->height()),0,int(raw->height())-1);}
        }
    }
    void draw(){const auto& io=ImGui::GetIO();controls=lab::json::object();hover_x=hover_y=-1;ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(io.DisplaySize);
        // Begin() already emits a window background, so binding a color shader
        // only afterwards misses it. Run before every window draw list instead.
        ImGui::GetBackgroundDrawList()->AddCallback(Renderer::callback,&ui);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0);
        ImGui::Begin("Capture viewer",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove);ImGui::PopStyleVar();ImGui::GetWindowDrawList()->AddCallback(Renderer::callback,&ui);
        const float dpi=ImGui::GetStyle().FontScaleDpi;
        lab::product::wordmark(dpi);ImGui::SameLine(340*dpi);
        if(lab::product::navigation("游戏库",games_page,{116*dpi,42*dpi}))games_page=true;mark("page.games");ImGui::SameLine(0,6*dpi);
        if(lab::product::navigation("采集浏览",!games_page,{116*dpi,42*dpi}))games_page=false;mark("page.captures");
        ImGui::SameLine(ImGui::GetWindowWidth()-154*dpi);
        if(ImGui::Button("设置",{126*dpi,40*dpi})){about=true;about_error.clear();ImGui::OpenPopup("设置##product");check_model();}
        mark("app.about");
        if(setup_requested&&manager_allowed){setup_requested=false;ImGui::OpenPopup("模型配置##model-setup");}
        ImGui::SetNextWindowPos({io.DisplaySize.x*.5f,io.DisplaySize.y*.5f},ImGuiCond_Appearing,{.5f,.5f});
        // Width fixed every frame, height from the content (see the game library popups).
        {const float w=std::min(640*dpi,io.DisplaySize.x-48*dpi);ImGui::SetNextWindowSizeConstraints({w,0},{w,FLT_MAX});}
        if(ImGui::BeginPopupModal("模型配置##model-setup",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
            lab::product::wordmark(dpi);ImGui::Dummy({0,16*dpi});
            const bool ready=model_checked&&model.usable(),unrecognized=ready&&!model.known;
            ImGui::Spacing();
            ImGui::PushFont(nullptr,26);ImGui::TextUnformatted("NR 模型");ImGui::PopFont();
            ImGui::TextWrapped("选择原版 nvngx_dlssnr.dll。");
            ImGui::Dummy({0,8*dpi});ImGui::Separator();ImGui::Spacing();
            const auto status=model_job.valid()?(setup_importing?"导入中…":"校验中…"):
                unrecognized?"未识别 · 非原版 · 风险自负":ready?"模型已识别":model_checked?(model.present?"校验未通过":"未配置模型"):"待校验";
            lab::product::pill(status,ready&&!unrecognized?lab::product::accent:lab::product::amber);
            if(unrecognized){ImGui::TextDisabled("SHA-256 %s",model.sha256.c_str());ImGui::TextWrapped("%s",lab::games::render("model-unrecognized-notice").c_str());}
            if(ready){if(!model.label.empty())ImGui::TextWrapped("%s",model.label.c_str());
                ImGui::TextDisabled("Insert 打开游戏内面板。NR 默认关闭。");}
            else ImGui::TextWrapped("模型需自行提供，也可稍后导入。");
            if(!setup_error.empty()){ImGui::TextColored(lab::product::amber,"导入失败");ImGui::TextWrapped("%s",setup_error.c_str());}
            else if(!model_job.valid()&&!model.error.empty())ImGui::TextWrapped("%s",model.error.c_str());
            if(model_setup&&!model_setup->error.empty())ImGui::TextWrapped("配置未保存：%s",model_setup->error.c_str());
            ImGui::Spacing();ImGui::BeginDisabled(model_job.valid());
            if(ready){if(lab::product::action("进入游戏库",{200*dpi,42*dpi},true)){if(model_setup)model_setup->remember(lab::ModelSetupState::Decision::completed);games_page=true;ImGui::CloseCurrentPopup();}mark("setup.complete");
                ImGui::SameLine();if(ImGui::Button("重新选择",{180*dpi,42*dpi}))select_model();mark("setup.import");}
            else{if(lab::product::action("导入模型",{200*dpi,42*dpi},true))select_model();mark("setup.import");
                ImGui::SameLine();if(ImGui::Button("稍后配置",{180*dpi,42*dpi})){if(model_setup)model_setup->remember(lab::ModelSetupState::Decision::skipped);ImGui::CloseCurrentPopup();}mark("setup.skip");}
            ImGui::EndDisabled();ImGui::Spacing();ImGui::TextDisabled("可在设置中重新配置。");ImGui::EndPopup();}
        ImGui::SetNextWindowPos({io.DisplaySize.x*.5f,io.DisplaySize.y*.5f},ImGuiCond_Appearing,{.5f,.5f});
        {const float w=std::min(620*dpi,io.DisplaySize.x-48*dpi);ImGui::SetNextWindowSizeConstraints({w,0},{w,FLT_MAX});}
        if(ImGui::BeginPopupModal("设置##product",&about,ImGuiWindowFlags_AlwaysAutoResize)){
            lab::product::wordmark(dpi);ImGui::Spacing();ImGui::TextDisabled("0.2.0 Preview 3  /  WINDOWS · DX12");ImGui::Spacing();
            ImGui::TextWrapped("NR 控制与采集浏览");
            ImGui::Spacing();ImGui::SeparatorText("模型");
            const bool unrecognized_in_use=model_checked&&model.usable()&&!model.known;
            ImGui::TextColored(model_checked&&model.known&&model.error.empty()?lab::product::accent:lab::product::amber,"%s",model_job.valid()?"校验中…":model_checked?(!model.error.empty()?"模型校验未通过":model.known?"模型已识别":
                unrecognized_in_use?"未识别 · 非原版 · 风险自负":model.present?"模型版本未识别":"未配置模型"):"待校验");
            ImGui::TextWrapped("导入原版模型后可安装游戏插件。");
            if(!model.error.empty())ImGui::TextWrapped("%s",model.error.c_str());
            if(!model.label.empty())ImGui::TextDisabled("%s",model.label.c_str());
            if(model_checked&&model.present&&!model.known&&model.error.empty())ImGui::TextDisabled("SHA-256 %s",model.sha256.c_str());
            // The user's explicit opt-in; off by default. Saved at once; the game
            // library is reopened with it, because every check depends on it.
            {bool allow=allow_unrecognized();
             ImGui::BeginDisabled(!model_setup||!model_setup->enabled()||model_job.valid()||!manager_allowed);
             if(ImGui::Checkbox("允许使用未识别的模型",&allow)&&model_setup){model_setup->allow_unrecognized(allow);manager.reset();check_model();}
             mark("app.about.allow-unrecognized");ImGui::EndDisabled();
             if(allow){ImGui::PushStyleColor(ImGuiCol_Text,lab::product::amber);ImGui::TextWrapped("%s",lab::games::render("model-unrecognized-notice").c_str());ImGui::PopStyleColor();}}
            auto show_folder=[&](const std::filesystem::path& path){about_error.clear();if(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"explore",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)about_error="无法打开本地目录："+lab::utf8(path.wstring());};
            if(ImGui::Button("打开程序目录"))show_folder(library_root().parent_path()/L"app");ImGui::SameLine();
            if(ImGui::Button("打开数据目录"))show_folder(library_root());ImGui::SameLine();
            ImGui::BeginDisabled(model_job.valid()||!manager_allowed);if(ImGui::Button("重新校验"))check_model();ImGui::SameLine();
            if(lab::product::action("配置模型",{},true)){start_model_setup();ImGui::CloseCurrentPopup();}ImGui::EndDisabled();
            if(!about_error.empty()){ImGui::PushStyleColor(ImGuiCol_Text,lab::product::amber);ImGui::TextWrapped("%s",about_error.c_str());ImGui::PopStyleColor();}
            if(ImGui::CollapsingHeader("存储位置")){
                const auto root=lab::utf8(library_root().parent_path().wstring());
                ImGui::TextWrapped("程序：%s",lab::utf8(lab::root::self_executable().wstring()).c_str());
                ImGui::TextWrapped("数据：%s",lab::utf8(library_root().wstring()).c_str());
                ImGui::TextWrapped("不同安装目录的数据独立。");
                if(ImGui::SmallButton("复制根目录路径"))ImGui::SetClipboardText(root.c_str());}
            ImGui::Spacing();ImGui::SeparatorText("使用范围");ImGui::TextWrapped("适合离线单人游戏；联网或带反作弊的游戏风险自负。NR 默认关闭。");
            ImGui::TextWrapped("离线工具，无遥测。非 NVIDIA 官方产品。");
            ImGui::Spacing();ImGui::TextDisabled("MIT · 第三方许可见 THIRD_PARTY_NOTICES.md");
            if(ImGui::Button("关闭",{100*dpi,36*dpi}))ImGui::CloseCurrentPopup();mark("app.about.close");ImGui::EndPopup();}
        ImGui::Dummy({0,12*dpi});ImGui::Separator();ImGui::Dummy({0,8*dpi});
        if(games_page){if(manager_allowed){if(!manager)manager=std::make_unique<lab::GameManagerPage>(library_root().parent_path(),true,allow_unrecognized());manager->draw(window,dpi);}else ImGui::TextDisabled("采集回归模式：不读写游戏管理登记。");ImGui::End();return;}
        ImGui::PushFont(nullptr,26);ImGui::TextUnformatted("采集浏览");ImGui::PopFont();
        ImGui::SameLine(ImGui::GetWindowWidth()-252*dpi);
        if(ImGui::Button(library_visible?"收起记录":"展开记录",{118*dpi,36*dpi})){library_visible=!library_visible;++interactions;}mark("library.toggle");ImGui::SameLine();
        if(ImGui::Button("刷新",{76*dpi,36*dpi}))scan();
        ImGui::TextDisabled("显示设置不修改原件");ImGui::Spacing();
        if(library_visible){ImGui::BeginChild("Library",{260*dpi,0},ImGuiChildFlags_Borders);ImGui::GetWindowDrawList()->AddCallback(Renderer::callback,&ui);
            ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##filter","筛选运行 / 采集编号",filter,sizeof(filter));
            ImGui::TextDisabled("%zu 份记录",catalog.entries.size());if(scanning.valid())ImGui::TextDisabled("读取目录中…");
            ImGui::Separator();for(const auto& item:catalog.entries){const auto label=item.run+" / "+item.pair+" / "+item.application+" / "+item.parameters;if(*filter&&label.find(filter)==std::string::npos)continue;
                ImGui::PushID(lab::utf8(item.manifest.wstring()).c_str());if(ImGui::Selectable(item.pair.c_str(),selected==lab::utf8(item.manifest.wstring()),0,{0,0})&&!loading.valid())open(item.manifest);
                if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",label.c_str());ImGui::TextWrapped("%s",item.run.c_str());
                if(!item.application.empty())ImGui::TextDisabled("%s",item.application.c_str());
                if(!item.error.empty()){ImGui::TextColored({.9f,.65f,.3f,1},"原件缺失 / 记录不完整");if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",item.error.c_str());}
                else {ImGui::TextDisabled("%u×%u · 帧 %llu · %llu MiB",item.width,item.height,item.frame,item.bytes/(1024*1024));if(item.synthetic)ImGui::TextDisabled("合成测试 · 非游戏");}
                if(ImGui::SmallButton("打开所在文件夹"))folder(item.manifest);
                if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",lab::utf8(item.manifest.parent_path().wstring()).c_str());
                ImGui::Spacing();ImGui::Separator();ImGui::PopID();}
            if(catalog.truncated)ImGui::TextWrapped("目录数量达到上限，未遍历全部条目。");ImGui::EndChild();ImGui::SameLine();}
        ImGui::BeginChild("Inspector",{0,0});ImGui::GetWindowDrawList()->AddCallback(Renderer::callback,&ui);
        if(loading.valid())ImGui::TextColored({.7f,.8f,.4f,1},"加载并校验中…");
        if(!error.empty())ImGui::TextColored({1,.6f,.3f,1},"%s",error.c_str());
        if(!preference_error.empty())ImGui::TextColored({1,.6f,.3f,1},"显示设置无法保存：%s",preference_error.c_str());
        if(!last_export.empty()){ImGui::TextColored({.65f,.8f,.4f,1},"PNG 已导出");ImGui::SameLine();
            if(ImGui::SmallButton("打开导出目录"))ShellExecuteW(window,L"open",last_export.c_str(),nullptr,nullptr,SW_SHOWNORMAL);}
        if(!raw){ImGui::Dummy({0,65*dpi});const float width=ImGui::GetContentRegionAvail().x;const float inset=std::max(24.f*dpi,(width-510*dpi)*.5f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()+inset);ImGui::BeginGroup();lab::product::mark(ImGui::GetWindowDrawList(),ImGui::GetCursorScreenPos(),64*dpi);ImGui::Dummy({64*dpi,88*dpi});
            ImGui::PushFont(nullptr,26);ImGui::TextUnformatted(catalog.entries.empty()?"暂无采集记录":"未选择记录");ImGui::PopFont();ImGui::Spacing();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+490*dpi);ImGui::TextColored(lab::product::muted,"%s",catalog.entries.empty()?"保存的采集记录会显示在此处。":"从左侧选择记录。");ImGui::PopTextWrapPos();
            ImGui::Spacing();if(ImGui::Button("打开采集目录",{150*dpi,40*dpi})){
                if(!test_folder_dispatch&&reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"explore",library_root().c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)error="Windows 无法打开采集目录";}
            ImGui::EndGroup();ImGui::EndChild();ImGui::End();return;}
        const bool display_pair=raw->display_pair();
        ImGui::Text("%s   /   帧 %llu · 调用 %llu",display_pair?"显示诊断 · 候选区间配对":"同调用对照",raw->manifest().value("frame",0ULL),raw->manifest().value("call",0ULL));
        ImGui::SameLine();if(ImGui::SmallButton("打开所在文件夹##current"))folder(raw->path());mark("folder.current");
        ImGui::TextDisabled("同步缩放 · 导出副本");
        ImGui::BeginDisabled(display_pair);if(ImGui::Button("NR 输入 / 输出"))set_pair(1,2);mark("pair.nr");ImGui::SameLine();if(ImGui::Button("NR 前后工作图"))set_pair(0,3);mark("pair.hdr");ImGui::EndDisabled();
        ImGui::SameLine();ImGui::BeginDisabled(display_pair);if(ImGui::Checkbox("原始数值差分",&difference)){if(difference)wipe=false;++interactions;}mark("difference");ImGui::EndDisabled();
        ImGui::SameLine();if(ImGui::Checkbox("分割对照",&wipe)){if(wipe)difference=false;++interactions;}mark("wipe");
        if(ImGui::GetContentRegionAvail().x>950*dpi)ImGui::SameLine();ImGui::BeginDisabled(exporting.valid());if(ImGui::Button(exporting.valid()?"导出中…":"导出 SDR 原尺寸 PNG")){auto capture=raw;const auto l=left,r=right;const auto recipe=gpu.viewing_request();exporting=std::async(std::launch::async,[capture,l,r,recipe]{return lab::export_sdr_pair(*capture,l,r,recipe,library_root());});}mark("export");ImGui::EndDisabled();
        const bool compressed=raw->color_contract(left).interface_image();
        const bool can_reconstruct=raw->color_contract(left).reconstruction_available&&raw->color_contract(right).reconstruction_available;
        ImGui::Separator();ImGui::BeginDisabled(difference);
        if(ImGui::Checkbox("观看换算",&gpu.processing_enabled))++interactions;mark("display.processing");ImGui::EndDisabled();
        ImGui::SameLine();ImGui::TextDisabled(gpu.processing_enabled?"左右同步 · 用于显示和导出":"已关闭 · 设置保留");
        if(compressed){ImGui::SameLine();bool assist=gpu.display.nr_reconstruct!=0;ImGui::BeginDisabled(difference||!gpu.processing_enabled||!can_reconstruct);
            if(ImGui::Checkbox("RR 基底辅助恢复",&assist)){gpu.display.nr_reconstruct=assist?1:0;gpu.display.nr_linear=0;++interactions;}mark("display.assist");ImGui::EndDisabled();
            if(!can_reconstruct)ImGui::TextDisabled("缺少同调用 RR 恢复信息。");}
        const bool processed=compressed&&gpu.viewing_request().nr_reconstruct;
        ImGui::BeginDisabled(difference||(compressed&&!processed));
        if(ImGui::Button(gpu.display.mapping==0&&gpu.processing_enabled&&(!compressed||processed)?"自然 SDR · 已选":"自然 SDR")){lab::viewer::viewing_preset(gpu.display,0);gpu.processing_enabled=true;++interactions;}mark("mapping.view");ImGui::SameLine();
        if(ImGui::Button(gpu.display.mapping==3&&gpu.processing_enabled&&(!compressed||processed)?"柔和 ACES · 已选":"柔和 ACES")){lab::viewer::viewing_preset(gpu.display,1);gpu.processing_enabled=true;++interactions;}mark("mapping.aces");ImGui::SameLine();
        ImGui::EndDisabled();ImGui::BeginDisabled(difference);
        if(ImGui::Button(!gpu.processing_enabled?"原始域 · 已选":"原始域")){gpu.processing_enabled=false;++interactions;}mark("mapping.raw");
        ImGui::SameLine();if(ImGui::SmallButton("重置观看")){lab::viewer::viewing_preset(gpu.display,0);gpu.display.nr_reconstruct=0;gpu.processing_enabled=true;++interactions;}mark("display.reset");ImGui::EndDisabled();
        ImGui::BeginDisabled(difference||!gpu.processing_enabled);
        ImGui::SetNextItemWidth(170*dpi);if(ImGui::SliderFloat("曝光 / EV",&gpu.display.exposure,-6,6,"%.2f"))++interactions;mark("exposure");
        if((!compressed||processed)&&gpu.display.mapping==0){ImGui::SameLine();ImGui::SetNextItemWidth(170*dpi);if(ImGui::SliderFloat("显示对比度",&gpu.display.contrast,.75f,1.5f,"%.2f"))++interactions;mark("contrast");}ImGui::EndDisabled();
        ImGui::SameLine();ImGui::BeginDisabled(display_pair);bool hdr=hdr_preferred;if(ImGui::Checkbox("优先 HDR 窗口",&hdr)){hdr_preferred=hdr;gpu.apply_hdr_preference(hdr,true);++interactions;}mark("hdr");ImGui::EndDisabled();
        if(hdr_preferred&&!gpu.hdr_available)ImGui::TextDisabled("当前屏幕未开启 HDR，使用 SDR 映射；移回 HDR 屏后恢复。");
        if(gpu.hdr){ImGui::SetNextItemWidth(170*dpi);ImGui::SliderFloat("图片单位白 / nits（假设）",&gpu.display.white_nits,80,400,"%.0f");
            if(!compressed&&!difference&&gpu.display.mapping!=1&&gpu.display.mapping!=3){ImGui::SameLine();ImGui::SetNextItemWidth(170*dpi);ImGui::SliderFloat("显示峰值 / nits",&gpu.display.peak_nits,400,4000,"%.0f");}}
        if(!compressed&&gpu.display.mapping==2)ImGui::TextWrapped("已保留旧版显示设置；点击「观看映射」启用新版黑位、中间调与高光处理。原图不会改变。");
        if(difference){ImGui::SetNextItemWidth(200*dpi);ImGui::SliderFloat("差分增益 / 原始值",&gpu.display.difference_gain,1,64,"%.1f");}
        if(wipe&&!difference){ImGui::SetNextItemWidth(240*dpi);ImGui::SliderFloat("分割位置",&wipe_position,0,1,"%.2f");mark("wipe.position");controls["wipe.position"][2]=240*dpi;ImGui::SameLine();ImGui::TextDisabled("左：输入阶段  /  右：输出阶段 · 可直接拖动分界线");}
        if(display_pair)ImGui::TextWrapped("左侧：工作图假设预览，可调显示。右侧：最终 SDR 原码值，忽略曝光 / 曲线，禁止二次 ACES。两者不是已证实同帧，不提供跨域原始差分。");
        else if(processed)ImGui::TextWrapped("NR 原件 + 同调用 RR 基底 → 仅在观看分支恢复工作亮度 → 共享显示曲线。开关即时生效，不运行 NR、不改原件；该预览包含 RR 信息，不是纯 NR 输出，也未复刻游戏最终调色。");
        else if(compressed)ImGui::TextWrapped("NR 接口原值观看：不借用 RR，不套工作图色调曲线；没有恢复高光，不是游戏最终图。需要接近游戏的观看外观时，可显式选择 RR 基底辅助恢复；原始差分始终在恢复前计算。");
        else if(gpu.processing_enabled&&gpu.display.mapping==3)ImGui::TextWrapped("工作 RGB → 固定 ACES 拟合 → sRGB。两侧共享曝光；不是游戏原版调色或完整 ACES 参考实现。HDR 窗口内也只显示 SDR 范围。");
        else ImGui::TextWrapped(!gpu.processing_enabled||gpu.display.mapping==1?"原始域直显：不做色调映射。SDR 会截断超出范围的高光；这不是游戏最终调色。":"自然观看：保留黑位、调整中间调并压缩高光，不逐图自动曝光、不拟合右侧参考。源色域按 Rec.709 假设，绝对亮度未校准；追求可用观感，不承诺游戏后期一致。");
        if(!gpu.notice.empty())ImGui::TextWrapped("%s",gpu.notice.c_str());
        if(ImGui::Button("适应窗口")){fit=true;center_x=center_y=.5f;++interactions;}mark("fit");ImGui::SameLine();if(ImGui::Button("1:1 原像素")){fit=false;zoom=1;++interactions;}mark("pixel.one");
        ImGui::SameLine();if(ImGui::Checkbox("最近邻放大",&gpu.pixel_nearest))++interactions;ImGui::SameLine();ImGui::TextDisabled("滚轮缩放 · 拖动同步观察位置");
        const auto area=ImGui::GetContentRegionAvail();const float bottom=100*dpi;const float canvas_height=std::max(80.f,area.y-bottom-28*dpi);const float gap=14*dpi;const float canvas_width=difference||wipe?area.x:(area.x-gap)/2;
        const auto left_label=processed?"02  NR 输入 · 基底辅助观看预览":raw->color_contract(left).label();
        const auto right_label=processed?"03  NR 输出 · 基底辅助观看预览":raw->color_contract(right).label();
        ImGui::TextUnformatted(difference?"原始 FP16 RGB 绝对差分（未先做显示映射）":left_label);if(wipe&&!difference){ImGui::SameLine();ImGui::Text(" / %s",right_label);}else if(!difference){ImGui::SameLine(canvas_width+gap);ImGui::TextUnformatted(right_label);}
        render_center_x=center_x;render_center_y=center_y;auto pos=ImGui::GetCursorScreenPos();image(0,{canvas_width,canvas_height});if(!difference&&!wipe){ImGui::SetCursorScreenPos({pos.x+canvas_width+gap,pos.y});image(1,{canvas_width,canvas_height});}
        ImGui::TextDisabled("原始 %u × %u · %u 份 SHA-256 已校验 · %llu MiB · 原图读取，无低清预览替代",raw->width(),raw->height(),raw->stage_count(),raw->bytes()/(1024*1024));
        if(hover_x>=0){const auto p=raw->pixel(left,hover_x,hover_y),q=raw->pixel(right,hover_x,hover_y);ImGui::Text("原像素 (%d, %d)   L %.6g / %.6g / %.6g   R %.6g / %.6g / %.6g",hover_x,hover_y,p[0],p[1],p[2],q[0],q[1],q[2]);}
        else ImGui::TextDisabled("悬停查看原始数值；紫色标记非有限值。颜色预览不能代替数值与元数据证据。");
        if(ImGui::CollapsingHeader("颜色契约 / 采集来源与证据边界")){ImGui::TextWrapped("%s",selected.c_str());ImGui::TextWrapped(display_pair?"RR 与最终 SDR 复制均完成；仅候选区间配对，不证明完整同帧血缘。":"四颜色阶段属于同次 NR 调用；不是最终 Present，不把单帧输入包称为完整历史重播。");
            for(const int index:{left,right}){const auto c=raw->color_contract(index);auto request=gpu.viewing_request();request.hdr=gpu.hdr;const auto contract=lab::color::recipe(c,request);
                ImGui::TextWrapped("%s · %s",c.label(),contract.at("transform").get<std::string>().c_str());ImGui::TextWrapped("原件解释：%s",c.interpretation.c_str());}
            ImGui::TextWrapped("NR 原件不变；只有显式勾选 RR 基底辅助恢复才借用 RR。输入传递函数来自记录的适配契约，RGBA16F 本身不定义色域；不自动把 RR 请求的 pre-exposure 当成输出应除的曝光。工作色域 / 尼特标尺未校准，预设均为观看近似。原始 alpha 留在原件。");
            if(ImGui::Button("旧版映射（诊断比较）"))gpu.display.mapping=2;
            if(gpu.display.mapping==2){int curve=int(gpu.display.curve);const char* curves[]{"柔和肩部 / SDR","亮度映射 / SDR","线性截断 / SDR"};ImGui::BeginDisabled(compressed||gpu.hdr||difference);if(ImGui::Combo("旧版曲线",&curve,curves,3))gpu.display.curve=curve;ImGui::EndDisabled();}
            ImGui::BeginDisabled(processed);bool linear=gpu.display.nr_linear!=0;if(ImGui::Checkbox("NR 原始数值按线性解释（仅用于比较假设）",&linear))gpu.display.nr_linear=linear;ImGui::EndDisabled();
            ImGui::Text("窗口输出：%s · 系统报告屏幕峰值 %.0f nits（非实测）",gpu.hdr?"FP16 / scRGB":"RGBA8 / SDR",gpu.reported_peak);
            ImGui::Text("界面白：%.0f nits · %s · 独立于图片亮度",gpu.display.ui_white_nits,gpu.ui_white_observed?"Windows SDR 白电平":"读取不可用，回退 80 nits");
            const auto metadata=raw->manifest().value("same_call_metadata",lab::json());if(metadata.is_object()&&metadata.value("observed",false)){const auto rr=metadata.at("rr_request");ImGui::Text("本调用 RR：HDR 请求 %s · pre-exposure %.4g · scale %.4g",rr.at("hdr")==true?"ON":"OFF",rr.at("pre_exposure").get<float>(),rr.at("exposure_scale").get<float>());}
            else ImGui::TextDisabled("此旧采集未保存同调用的 RR 曝光 / jitter 元数据，不补默认值。");
            const auto provenance=raw->manifest().value("provenance",lab::json::object());ImGui::TextWrapped("来源：%s",provenance.value("origin","unknown").c_str());ImGui::TextWrapped("Manifest SHA-256：%s",raw->manifest_hash().c_str());
            if(!last_export.empty())ImGui::TextWrapped("派生 SDR PNG 与显示配方已保存：%s",lab::utf8(last_export.wstring()).c_str());}
        ImGui::EndChild();ImGui::End();
    }
};
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show){
    bool smoke=false;
    try{bool ui_test=false,sdr_view=false,nr_interface=false,processing=false,view_check=false,games=false,captures=false;std::filesystem::path source,report;int argc=0;auto** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        for(int i=1;i<argc;++i){const std::wstring arg=argv[i];if(arg==L"--games")games=true;else if(arg==L"--captures")captures=true;else if(arg==L"--smoke-test")smoke=true;else if(arg==L"--sdr-view")sdr_view=true;else if(arg==L"--nr-interface")nr_interface=true;else if(arg==L"--view-processing")processing=true;else if(arg==L"--view-check"){smoke=view_check=true;}else if(arg==L"--ui-test"){smoke=ui_test=true;}else if(arg==L"--pair"&&i+1<argc)source=argv[++i];else if(arg==L"--report"&&i+1<argc)report=argv[++i];else throw std::runtime_error("Unknown viewer argument");}LocalFree(argv);library_root();
        need(!view_check||(!source.empty()&&!ui_test),"View check requires a source and no UI test");
        need(!ui_test||!source.empty(),"UI test needs a bounded synthetic capture");
        if(!report.empty())need(report.is_absolute()&&!std::filesystem::exists(report),"Report requires a new absolute path");
        ImGui_ImplWin32_EnableDpiAwareness();WNDCLASSEXW wc{sizeof(wc)};wc.hInstance=instance;wc.lpfnWndProc=procedure;wc.lpszClassName=L"OverglazeCaptureViewer";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        wc.hIcon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_SHARED));
        wc.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
        need(wc.hIcon&&wc.hIconSm,"Load application icons");need(RegisterClassExW(&wc)!=0,"Register viewer");
        HWND window=CreateWindowW(wc.lpszClassName,lab::brand::kViewerWindowTitle,WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1580,980,nullptr,nullptr,instance,nullptr);need(window!=nullptr,"Create viewer");
        // Size and centre it for the monitor it was placed on, at that monitor's DPI.
        // The system DPI is the primary monitor's: on a 200% monitor a size taken
        // from it was half what the scaled layout needs, and the content overlapped
        // until the window was resized.
        {const auto dpi=GetDpiForWindow(window);MONITORINFO monitor_info{sizeof(monitor_info)};GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor_info);
            const auto& work=monitor_info.rcWork;const int work_width=work.right-work.left,work_height=work.bottom-work.top,margin=MulDiv(30,dpi,96);
            const int width=std::min<int>(MulDiv(1580,dpi,96),work_width-2*margin),height=std::min<int>(MulDiv(980,dpi,96),work_height-2*margin);
            SetWindowPos(window,nullptr,work.left+(work_width-width)/2,work.top+(work_height-height)/2,width,height,SWP_NOZORDER|SWP_NOACTIVATE);}
        BOOL dark=TRUE;DwmSetWindowAttribute(window,20,&dark,sizeof(dark));Renderer gpu(window);ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        wchar_t windows[MAX_PATH]{};GetWindowsDirectoryW(windows,MAX_PATH);
        const auto font_file=std::filesystem::path(windows)/L"Fonts"/L"msyh.ttc";
        ImVector<ImWchar> glyphs;ImFontGlyphRangesBuilder builder;builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());builder.AddText("釉光");builder.BuildRanges(&glyphs);
        auto* font=std::filesystem::exists(font_file)?io.Fonts->AddFontFromFileTTF(lab::utf8(font_file.wstring()).c_str(),16,nullptr,glyphs.Data):nullptr;
        if(font)io.FontDefault=font;else io.Fonts->AddFontDefault();
        lab::product::apply_theme();auto& style=ImGui::GetStyle();const auto base=style;
        ImGui_ImplWin32_Init(window);ImGui_ImplDX11_Init(gpu.device.Get(),gpu.context.Get());View view{window,gpu};
        // The viewer opens on the game manager page. Opening a
        // capture (--pair) or asking for --captures starts on the capture
        // browser; the regression modes (--smoke-test, --ui-test, --view-check)
        // never touch the game registry and stay on the capture browser.
        view.games_page=!smoke&&!captures&&(games||source.empty());view.manager_allowed=!smoke;
        view.model_setup=std::make_unique<lab::ModelSetupPreferences>(smoke?std::filesystem::path{}:library_root());
        if(!smoke)view.check_model(true);
        auto previous=library_root()/L"settings"/L"viewer.json";
        for(const auto* name:{L"viewer-display-v2.json",L"viewer-display-v3.json"})if(std::filesystem::exists(library_root()/L"settings"/name))previous=library_root()/L"settings"/name;
        lab::UiPreferencesFile preferences(smoke?std::filesystem::path{}:library_root()/L"settings"/L"viewer-display-v4.json",lab::ViewerPreferences{}.document(),smoke?std::filesystem::path{}:previous,smoke?std::filesystem::path{}:library_root());view.restore(lab::ViewerPreferences::parse(preferences.loaded()));view.test_folder_dispatch=smoke;
        if(sdr_view){lab::viewer::viewing_preset(gpu.display,0);gpu.processing_enabled=true;view.hdr_preferred=false;view.difference=view.wipe=false;}
        view.prefer_interface=nr_interface;view.prefer_processing=processing;
        view.scan();if(!source.empty())view.open(source);ShowWindow(window,smoke?SW_HIDE:show);
        struct Step{const char* id;std::function<bool()> check;};std::vector<Step> steps{{"pair.hdr",[&]{return view.left==0&&view.right==3;}},{"mapping.raw",[&]{return !gpu.processing_enabled;}},{"mapping.view",[&]{return gpu.display.mapping==0&&gpu.processing_enabled;}},{"folder.current",[&]{return view.last_folder==view.raw->path().parent_path();}},{"difference",[&]{return view.difference;}},
            {"pair.nr",[&]{return view.left==1&&view.right==2;}},{"difference",[&]{return !view.difference;}},{"pixel.one",[&]{return !view.fit&&view.zoom==1;}},{"fit",[&]{return view.fit;}},
            {"library.toggle",[&]{return !view.library_visible;}},{"library.toggle",[&]{return view.library_visible;}},{"wipe",[&]{return view.wipe;}},{"wipe.position",[&]{return std::abs(view.wipe_position-.5f)<.02f;}},
            {"wipe",[&]{return !view.wipe;}},{"hdr",[&]{return view.hdr_preferred&&(!gpu.hdr_available||gpu.hdr);}},{"hdr",[&]{return !view.hdr_preferred&&!gpu.hdr;}},
            {"display.assist",[&]{return gpu.display.nr_reconstruct==1&&view.left==1&&view.right==2;}},
            {"mapping.view",[&]{return gpu.display.mapping==0&&gpu.display.nr_reconstruct==1;}},
            {"display.processing",[&]{return !gpu.processing_enabled&&gpu.viewing_request().nr_reconstruct==0&&gpu.display.nr_reconstruct==1&&view.left==1&&view.right==2;}},
            {"display.processing",[&]{return gpu.processing_enabled&&gpu.viewing_request().nr_reconstruct==1&&gpu.display.contrast==1.15f;}},
            {"mapping.aces",[&]{return gpu.display.mapping==3&&view.left==1&&view.right==2;}},
            {"export",[&]{return !view.last_export.empty()&&!view.exporting.valid();}}};
        bool quit=false,display_ui_configured=false,processing_qa_started=false;unsigned frames=0,test_phase=0,test_step=0;std::uint64_t next_output=0;const auto start=GetTickCount64();lab::json background_checks=lab::json::object(),wipe_checks=nullptr,final_checks=lab::json::object(),processing_checks=lab::json::array();
        while(!quit){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);if(m.message==WM_QUIT)quit=true;}if(quit)break;
            view.poll();if(resize_width&&resize_height){gpu.resize(resize_width,resize_height,gpu.hdr);resize_width=resize_height=0;}
            if(view_check&&view.raw&&!processing_qa_started){need(view.raw->color_contract(2).reconstruction_available,"No supported reference for view check");
                processing_qa_started=true;view.set_pair(1,2);view.fit=false;view.zoom=1;view.difference=view.wipe=view.hdr_preferred=false;gpu.apply_hdr_preference(false);
                gpu.display.exposure=0;gpu.display.mapping=3;gpu.display.nr_reconstruct=0;}
            if(smoke&&view.raw&&view.raw->display_pair()&&!display_ui_configured){display_ui_configured=true;view.fit=false;view.zoom=1;
                if(ui_test)steps={{"folder.current",[&]{return view.last_folder==view.raw->path().parent_path();}},
                    {"mapping.view",[&]{return gpu.display.mapping==0;}},{"mapping.raw",[&]{return !gpu.processing_enabled;}},{"mapping.aces",[&]{return gpu.display.mapping==3&&gpu.processing_enabled;}},
                    {"difference",[&]{return !view.difference;}},{"hdr",[&]{return !view.hdr_preferred&&!gpu.hdr;}},
                    {"display.reset",[&]{return gpu.display.exposure==0;}},{"export",[&]{return !view.last_export.empty()&&!view.exporting.valid();}}};}
            if(moved||!gpu.outputs||!gpu.outputs->IsCurrent()||GetTickCount64()>next_output){moved=false;next_output=GetTickCount64()+1000;gpu.inspect_output();gpu.apply_hdr_preference(view.hdr_preferred);}
            style=base;const auto scale=std::clamp(GetDpiForWindow(window)/96.f,1.f,3.f);style.ScaleAllSizes(scale);style.FontScaleDpi=scale;
            ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();bool input_issued=false;
            if(ui_test&&view.raw&&!view.loading.valid()&&view.controls.contains("pair.nr")&&frames>3&&test_step<steps.size()){
                input_issued=true;if(test_phase<3){const auto it=view.controls.find(steps[test_step].id);need(it!=view.controls.end(),"Viewer test control disappeared");const auto& r=*it;
                    io.AddMousePosEvent(r[0].get<float>()+r[2].get<float>()*.5f,r[1].get<float>()+r[3].get<float>()*.5f);if(test_phase==1)io.AddMouseButtonEvent(0,true);if(test_phase==2)io.AddMouseButtonEvent(0,false);}}
            view.preference_error=preferences.error();ImGui::NewFrame();view.draw();preferences.save(view.preferences().document());if(input_issued&&++test_phase>=4){if(steps[test_step].check()){test_phase=0;++test_step;if(test_step==steps.size())quit=true;}else test_phase=3;}
            ImGui::Render();const float clear[]{.05f,.06f,.07f,1};gpu.context->OMSetRenderTargets(1,gpu.target.GetAddressOf(),nullptr);gpu.context->ClearRenderTargetView(gpu.target.Get(),clear);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if(smoke&&frames>3&&!background_checks.contains(gpu.hdr?"hdr":"sdr"))background_checks[gpu.hdr?"hdr":"sdr"]=gpu.verify_background();
            if(ui_test&&view.wipe&&test_step>=9&&wipe_checks.is_null()&&view.raw->manifest().value("provenance",lab::json::object()).value("origin","")=="synthetic-viewer-distinct-stages")wipe_checks=view.verify_wipe();
            if(smoke&&view.raw&&view.raw->display_pair()&&frames>3){const auto effective=gpu.viewing_request();const auto key=std::to_string(effective.mapping)+"/"+std::to_string(effective.exposure);
                if(!final_checks.contains(key))final_checks[key]=view.verify_final_codes();}
            if(view_check&&processing_qa_started&&frames>3){processing_checks.push_back(view.verify_processing_pixels());
                if(processing_checks.size()==1)gpu.display.nr_reconstruct=1;
                else if(processing_checks.size()==2)gpu.display.mapping=0;
                else if(processing_checks.size()==3)gpu.display.nr_reconstruct=0;
                else if(processing_checks.size()==4){gpu.display.nr_reconstruct=1;gpu.display.exposure=-.75f;}
                else if(processing_checks.size()==5)gpu.processing_enabled=false;
                else if(processing_checks.size()==6)gpu.processing_enabled=true;
                else {need(processing_checks[4]["pixels"]==processing_checks[6]["pixels"],"Re-enabled window must restore identical pixels");quit=true;}}
            hr(gpu.chain->Present(smoke?0:1,0));++frames;
            if(smoke){if(!view.error.empty())throw std::runtime_error(view.error);if(GetTickCount64()-start>30000){
                    if(!report.empty()){std::ofstream out(report,std::ios::binary);out<<lab::json{{"passed",false},{"error","Viewer smoke timeout"},{"ui_steps_passed",test_step},{"waiting_for",test_step<steps.size()?steps[test_step].id:"none"},{"controls",view.controls},{"display_preferences",view.preferences().document()}}.dump(2);}
                    throw std::runtime_error("Viewer smoke timeout at step "+std::to_string(test_step));}
                if(!ui_test&&!view_check&&!view.loading.valid()&&!view.scanning.valid()&&(source.empty()||view.raw)){
                    if(view.raw&&view.raw->display_pair()){if(frames>5){if(test_phase==0){gpu.display.exposure=6;gpu.display.mapping=0;++test_phase;}
                        else if(test_phase==1){gpu.display.exposure=-6;gpu.display.mapping=3;++test_phase;}else quit=true;}}
                    else if(view.raw&&test_phase<4){if(test_phase==0)view.set_pair(0,3);if(test_phase==1){view.difference=true;view.fit=false;view.zoom=2;}if(test_phase==2){view.set_pair(1,2);view.gpu.display.exposure=1;}if(test_phase==3){view.difference=false;view.fit=true;}++test_phase;}else if(frames>5){if(gpu.hdr_available&&!background_checks.contains("hdr")){if(!gpu.hdr)gpu.resize(gpu.width,gpu.height,true);}else quit=true;}}}
        }
        lab::json receipt={{"purpose","functional-verification"},{"kind","capture-viewer-smoke"},{"passed",true},{"frames",frames},{"game_started",false},{"nr_executed",false},{"source_modified",false},{"source",source.empty()?"":lab::utf8(source.wstring())},{"pair_loaded",bool(view.raw)},{"display_hdr",gpu.hdr},{"hdr_output_available",gpu.hdr_available},{"catalog_entries",view.catalog.entries.size()},{"original_resolution_textures",true},{"controls",view.controls},{"modules",lab::current_modules()}};
        if(view.raw){receipt["manifest_sha256"]=view.raw->manifest_hash();receipt["width"]=view.raw->width();receipt["height"]=view.raw->height();receipt["mapped_bytes"]=view.raw->bytes();
            receipt["color_contracts"]=lab::json::array();for(unsigned i=0;i<view.raw->stage_count();++i)receipt["color_contracts"].push_back(view.raw->color_contract(i).document());}
        receipt["ui_test"]=ui_test;receipt["ui_steps_passed"]=test_step;receipt["derived_export"]=lab::utf8(view.last_export.wstring());
        receipt["capture_folder_requested"]=lab::utf8(view.last_folder.wstring());receipt["explorer_suppressed_for_test"]=smoke;receipt["ui_white_nits"]=gpu.display.ui_white_nits;receipt["ui_white_observed"]=gpu.ui_white_observed;
        receipt["window_background_checks"]=background_checks;receipt["wipe_pixel_checks"]=wipe_checks;receipt["output_generation"]=gpu.output_generation;receipt["display_preferences"]=view.preferences().document();receipt["preferences_enabled"]=!smoke;
        receipt["final_sdr_code_checks"]=final_checks;
        receipt["processing_window_checks"]=processing_checks;receipt["display_processing_enabled"]=gpu.processing_enabled;receipt["rr_assisted_view"]=gpu.viewing_request().nr_reconstruct!=0;
        if(!report.empty()){std::ofstream out(report,std::ios::binary);out<<receipt.dump(2);need(bool(out),"Write smoke receipt");}
        preferences.close();gpu.clear_images();ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();if(IsWindow(window))DestroyWindow(window);return 0;
    }catch(const std::exception& e){OutputDebugStringA(e.what());std::cerr<<e.what()<<'\n';if(!smoke)MessageBoxW(nullptr,lab::wide(e.what()).c_str(),lab::brand::kViewerStartFailureTitle,MB_OK|MB_ICONERROR);return 1;}
}
