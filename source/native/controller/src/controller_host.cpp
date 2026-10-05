// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// The NR controller's own in-game host (the controller half of the dual-track
// split). It does what a user needs to run NR and nothing else: the control
// pipe, the Insert panel, Streamline admission on public evidence, and the
// runtime bridge client.
//
// What it deliberately does NOT have, and why the split exists: no vectored
// exception observer, no queue/command/present observers, no post binding
// inspection, no access probe, no frame or display capture, no private RR
// callbacks and no common-module restore hook. Its link closure contains no
// research object at all, which tests/controller/test_controller_link_closure.py
// checks against the linker map. Two observers it does have are
// shared runtime/provider code, not research: the command-signature creation
// observer on the early path of a binding-preservation package (007 First Light:
// without it every ExecuteIndirect carried no layout and the strict policy refused
// most frames as "indirect-signature-unobserved"), and the game's own latency
// markers (a Present declared for an earlier frame expires nothing).
//
// Consequences that are stated, not hidden: admission runs on public evidence
// only (admission_mode "public-evaluate-evidence-only"), so a game whose
// Streamline build has a reviewed private profile is admitted on strictly weaker
// evidence here than under the research host. The bridge it loads must be the
// controller variant; a research bridge is refused by identity, not silently
// tolerated.
#include "lab_standalone.hpp"
#include "lab_control.hpp"
#include "lab_pipe.hpp"
#include "lab_installation.hpp"
#include "lab_package_identity.hpp"
#include "lab_startup_failure.hpp"
#include "lab_hook_bank.hpp"
#include "lab_overlay.hpp"
#include "lab_workbench_adapter.hpp"
#include "lab_controller_fixture.hpp"
#include "lab_late_discovery.hpp"
#include "lab_present_queue_witness.hpp"
#include "lab_late_panel.hpp"
#include "lab_sl_frame_provider.hpp"
#include "lab_ngx_observer.hpp"
#include "lab_streamline.hpp"
#include "lab_live_runtime_client.hpp"
#include "lab_controller_live_sink.hpp"
#include "lab_indirect_observer.hpp"
#include "lab_latency_markers.hpp"
#include <sl_core_api.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace lab {
namespace {
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
bool flag(const wchar_t* name){wchar_t v[8]{};return GetEnvironmentVariableW(name,v,8)==1&&v[0]==L'1';}
std::string env_text(const wchar_t* name){std::array<wchar_t,512> value{};
    const auto size=GetEnvironmentVariableW(name,value.data(),static_cast<DWORD>(value.size()));
    return size&&size<value.size()?utf8(std::wstring(value.data(),size)):std::string{};}
// Health of the root proxy, read back from the proxy itself. Null when this
// process has no Lab proxy at all, which is the late-attach shape. It is the
// only window onto what happened in the game's loader before we existed: a
// failed system-dxgi resolution or a refused host load shows up here and
// nowhere else.
struct ProxyStatus {
    unsigned size,attempts,failures,fallback_calls,last_stage,host_stage;
    unsigned long last_error,host_last_error;
    unsigned resolved,host_loaded,compat_string_pending,compat_string_replayed;
    unsigned mode,insert_presses,insert_stage; // 1 load at the first factory, 2 load on the first Insert
};
// The proxy hands us its own module when it calls in. Looking it up by name
// would be wrong: once the system dxgi.dll is loaded, two modules answer to
// "dxgi.dll" and the name lookup can return the system one.
std::atomic<HMODULE> root_proxy_module{nullptr};
json proxy_status()noexcept{
    const auto dxgi=root_proxy_module.load(std::memory_order_acquire);
    if(!dxgi)return json(nullptr); // late attach: there is no proxy in this process
    const auto read=reinterpret_cast<void(__cdecl*)(ProxyStatus*)>(GetProcAddress(dxgi,"LabDXGIProxyStatus"));
    if(!read)return json(nullptr);
    ProxyStatus s{};s.size=sizeof(s);read(&s);
    return {{"system_dxgi_resolved",s.resolved!=0},{"resolve_attempts",s.attempts},{"resolve_failures",s.failures},
        {"safe_default_calls",s.fallback_calls},{"last_resolve_stage",s.last_stage},{"last_resolve_error",s.last_error},
        {"host_loaded",s.host_loaded!=0},{"host_load_stage",s.host_stage},{"host_load_error",s.host_last_error},
        {"compat_string_pending",s.compat_string_pending!=0},{"compat_string_replayed",s.compat_string_replayed!=0},
        {"mode",s.mode==2?"on-insert":s.mode==1?"at-first-factory":"undecided"},{"insert_presses",s.insert_presses},{"insert_stage",s.insert_stage},
        {"scope","the game-directory proxy's own counters: whether it resolved the system dxgi.dll and whether it could load this host from the payload subdirectory"}};
}
// Only an address inside this very executable's committed image may be hooked.
bool fixture_address(const void* p,bool execute,HMODULE exe){MEMORY_BASIC_INFORMATION m{};
    return p&&VirtualQuery(p,&m,sizeof(m))&&m.State==MEM_COMMIT&&m.AllocationBase==exe&&
        (!execute||(m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE)));}

struct Host;
std::atomic<Host*> current{nullptr};
std::once_flag once;
thread_local unsigned present_depth=0;
thread_local unsigned creation_depth=0;
thread_local unsigned resize_depth=0;
struct CreationScope {bool outer=creation_depth++==0;~CreationScope(){--creation_depth;}};
struct ResizeScope {bool outer=resize_depth++==0;~ResizeScope(){--resize_depth;}};

// The adapter admits RR calls into this sink; it forwards to the frame provider
// once the bridge client exists, and drops them before that, exactly as the
// research host drops them while its client is null.
struct Host final:SlAdmissionSink {
    Controller controller;PipeServer pipe{controller};
    // The game's own latency markers (lab_latency_markers.hpp). The detours and the
    // adapter reference it for the life of the process; the Host is never
    // destroyed. Declared before the adapter. Hooked once the bridge exists.
    chain::LatencyMarkers markers;
    // No research extension and no research observer: public discovery only.
    WorkbenchAdapterHost adapter{true,this,WorkbenchScope::full_metadata,nullptr};
    // Early path only (factory_created), for a package with binding preservation:
    // the command-signature creation observer is installed before the game
    // creates its device (lab_indirect_observer.hpp). A late attach never has it.
    std::atomic<bool> indirect_observing{false};
    std::string indirect_error;
    HMODULE module=nullptr;
    json executable,module_info;
    std::optional<Installation> installation;
    std::unique_ptr<GameOverlay> overlay;
    ControllerLiveSink sink{controller};
    std::atomic<LiveRuntimeClient*> live{nullptr};
    std::optional<SlFrameProvider> provider;
    // Routes whose DLSS/RR does not run through Streamline. On those the
    // host NEVER builds a provider and therefore cannot admit a frame even
    // by accident: there is no receiver to admit one into. All it does is
    // record what the game itself does at the NGX boundary.
    std::optional<NgxObserver> ngx_observer;
    // ngx-rr and ngx-sr: the NGX observer admits into the live client -- RR,
    // and SR (SR only while no RR feature is
    // alive). The Streamline provider is not built on these routes.
    // observe_only is kept for a future route that can only be watched.
    bool observe_only=false,ngx_admit=false;
    bool console_started=false;
    // Set only when there is NO validated installation, so installed always wins.
    bool fixture=false;
    std::atomic<bool> fixture_attached{false};
    std::atomic<bool> stopping{false},worker_done{false},teardown_done{false};
    std::atomic<bool> hook_fault{false};
    std::atomic<std::uint64_t> presents{0},successful_presents{0},resizes{0},swapchains{0};
    // Swapchains handed to our hook that we cannot drive and therefore skip.
    // Nonzero is normal for a game with frame generation; it is not an error.
    std::atomic<std::uint64_t> foreign_swapchains{0};
    std::mutex seed_mutex,adapter_worker_mutex;ComPtr<IUnknown> pending_queue;
    bool seed_taken=false,preparation_requested=false;
    std::atomic<bool> queue_available{false};
    HookBank<4> factory_hooks;
    HookBank<5> present_hooks;
    // ---- late loading: injected into a game that is already presenting.
    bool late=false;
    latebind::Discovery discovery;
    std::atomic<bool> adopted{false};
    std::atomic<std::uint64_t> late_started_at{0};
    std::string late_note;
    // Nothing on the late path keeps a reference to the game's swapchain or its
    // back buffers between presents. RE9 once crashed inside Streamline
    // frame generation on a null swapchain right after FG was switched on in
    // game, while this host held the adopted chain for the process lifetime and
    // the panel held every back buffer; DXGI admits one flip chain per HWND, so
    // a chain kept alive by us makes the game's next one on that window fail.
    // The chain is now touched only in the Present detour, through the pointer
    // that call was handed (late_present); latebind::LatePanel follows it by
    // identity and binds the panel.
    std::optional<latebind::LatePanel> late_panel;
    // Guards the probe record, the device and the refusal note below. Taken
    // inside the panel's evidence callbacks (panel lock first) and alone by
    // the worker; never the other way round.
    std::mutex late_mutex;
    // The adopted chain's device: the witness is built on it and a take-over
    // must share it. A device reference does not keep a swapchain alive.
    ComPtr<ID3D12Device> late_device;
    // 1: the queue RR ran on is the queue the swapchain presents with; 0: it is
    // not; -1: RR's queue not observed yet. Reported in the host status.
    int late_panel_queue_match=-1;
    // What asking the followed swapchain for its present queue returned, per step.
    json late_panel_probe=nullptr;const void* late_panel_probed_chain=nullptr;
    bool late_prepare_requested=false;
    // When the chain does not name its queue: the present-transition witness
    // (lab_present_queue_witness.hpp). Asked for by the detour, installed by the
    // worker on late_device (MinHook freezes threads), only then.
    std::atomic<PresentQueueWitness*> present_witness{nullptr};
    std::atomic<bool> witness_wanted{false},present_witness_refused{false};std::string witness_refused_note;
    // The queue RR ran on, as an identity for the match note only; set by the worker.
    std::atomic<const void*> rr_queue{nullptr};
    std::atomic<bool> late_open_panel{false}; // brought in by the player's Insert: open once attached

    explicit Host(HMODULE own):module(own){
        adapter.set_latency_markers(&markers); // token feed and Present reading go through the adapter
        // An Xbox app title's EXE cannot be read even from inside its own
        // process tree reliably; its identity is the installed OS package.
        executable=game_executable_identity();module_info=module_identity(own);
        fixture=!has_installation(own)&&flag(L"OVERGLAZE_CONTROLLER_FIXTURE");
        need(fixture||has_installation(own),"Controller host requires a validated installation beside it");
        if(fixture){
            // The acceptance fixture: an isolated run directory under the lab's own
            // data root, the exact launcher-verified executable, and explicit
            // offline / no-anticheat / controlled-NR confirmation. A game can never
            // reach here because an installed host has a config and takes the other
            // branch before the environment is consulted.
            const auto exe=std::filesystem::path(wide(executable.at("path").get<std::string>()));
            need(exe.filename()==L"lab_controller_harness.exe","Controller fixture identity");
            // Host contract V4: the fixture's data root is the parent of its own
            // run directory (OVERGLAZE_LIVE_DATA_PATH), held to the local-disk
            // rules -- no compiled machine path.
            need(exe.parent_path().parent_path()==fixture_data_root(),
                 "Controller fixture must run from an isolated lab run directory");
            need(executable.value("sha256","")==env_text(L"OVERGLAZE_CONTROLLER_FIXTURE_SHA256"),"Controller fixture hash");
            need(flag(L"OVERGLAZE_OFFLINE_CONFIRMED")&&flag(L"OVERGLAZE_NO_ANTICHEAT_CONFIRMED"),"Controller fixture needs offline/no-anticheat confirmation");
            need(flag(L"OVERGLAZE_ENABLE_CONTROLLED_NR"),"Controller fixture must explicitly enable the controlled backend");
        }else{
            installation=load_installation(own,executable,module_info);
            adapter.set_game_facts(installation->facts);
            controller.accept_game_profile(installation->profile);
            controller.set_default_exposure_stops(installation->facts.default_exposure_stops);
            // sl-rr / sl-sr admit through the Streamline provider; ngx-rr /
            // ngx-sr admit through the NGX observer (see ngx_admit below).
            ngx_admit=installation->facts.route=="ngx-rr"||installation->facts.route=="ngx-sr";
            observe_only=false;
            if(ngx_admit||observe_only)ngx_observer.emplace();
        }
        // Self-configuring on both paths: the SL viewport and depth semantic come
        // from the game's own calls (lab_sl_bindings.hpp), not from the package.
        adapter.self_configure_before_attach();
        // Policy: RR where the game has it, SR where it does not,
        // decided per call: DLSS super resolution is a second target, so NR runs
        // after SR in a pure-SR game and in an RR game with RR switched off.
        adapter.target_super_resolution_before_attach();
        controller.enable_nr_preparation(true); // normal launch stands by with NR OFF
        // Compute-only is a controller feature, not a research one: it runs the
        // conversion, NR and composite but never writes the game's colour back,
        // which is how cost is separated from picture. Without this the panel's
        // button never appears and SetNrMode compute-only is refused.
        controller.enable_nr_compute_only();
        controller.enable_embedded_control();
        // This process's run directory must exist before anything can need it: the
        // live client's data path, the panel's preferences and the NR fault record
        // all read installation->run, which is empty until the installation is
        // activated. Without this the bridge client cannot be created at all.
        if(!fixture)activate_installation(*installation);
        // The panel is built here, not on the worker: the only attach site is
        // offer(), which runs on the game thread the moment a swapchain appears,
        // and a swapchain created before the worker started would never attach.
        try{
            // An empty preferences path makes the panel's settings file read-only,
            // so a fixture writes nothing into data\\settings.
            const auto preferences=fixture?std::filesystem::path{}:installation->output_root/L"settings"/wide(installation->facts.settings_file);
            overlay=std::make_unique<GameOverlay>(controller,preferences,fixture?std::filesystem::path{}:installation->output_root);
        }catch(const std::exception& e){controller.diagnostic(std::string("Panel unavailable: ")+e.what());}
        pipe.start();
    }

    // ---- Streamline admission (SlAdmissionSink). Forwarded to the provider.
    void game_binding_enter(const slboundary::Resolution& r,void* command) noexcept override {
        if(auto* p=frame_provider())p->game_binding_enter(r,command);
    }
    void game_binding_exit(std::uint64_t call) noexcept override {if(auto* p=frame_provider())p->game_binding_exit(call);}
    void enter(const slboundary::Resolution& r,const rr::Packet& packet,void* command) noexcept override {
        if(auto* p=frame_provider())if(!stopping&&!hook_fault)p->enter(r,packet,command);
    }
    void boundary_returned(std::uint64_t call,bool success) noexcept override {if(auto* p=frame_provider())p->boundary_returned(call,success);}
    void aborted(std::uint64_t call) noexcept override {if(auto* p=frame_provider())p->aborted(call);}
    void unavailable() noexcept override {if(auto* p=frame_provider())p->unavailable();}
    void unavailable(const RejectedCall& why) noexcept override {if(auto* p=frame_provider())p->unavailable(why);}
    // On the NGX routes a live client exists but no Streamline provider is ever
    // built: dereferencing the empty optional there would be undefined
    // behaviour. live is published after emplace, so a set live
    // with a provider always sees the provider.
    SlFrameProvider* frame_provider() noexcept {return live.load()&&provider?&*provider:nullptr;}

    // ---- DXGI factory and swapchain hooks
    template<unsigned Bank>static HRESULT STDMETHODCALLTYPE create(IDXGIFactory* f,IUnknown* q,DXGI_SWAP_CHAIN_DESC* d,IDXGISwapChain** out){auto* h=current.load();CreationScope scope;
        const auto r=h->factory_hooks.original<HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*,IUnknown*,DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**)>(Bank,0)(f,q,d,out);
        if(scope.outer&&SUCCEEDED(r)&&out&&*out)h->offer(q,*out);return r;}
    template<unsigned Bank>static HRESULT STDMETHODCALLTYPE hwnd(IDXGIFactory2* f,IUnknown* q,HWND w,const DXGI_SWAP_CHAIN_DESC1* d,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,IDXGIOutput* o,IDXGISwapChain1** out){auto* h=current.load();CreationScope scope;
        const auto r=h->factory_hooks.original<HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**)>(Bank,1)(f,q,w,d,fs,o,out);
        if(scope.outer&&SUCCEEDED(r)&&out&&*out)h->offer(q,*out);return r;}
    template<unsigned Bank>static HRESULT STDMETHODCALLTYPE core(IDXGIFactory2* f,IUnknown* q,IUnknown* w,const DXGI_SWAP_CHAIN_DESC1* d,IDXGIOutput* o,IDXGISwapChain1** out){auto* h=current.load();CreationScope scope;
        const auto r=h->factory_hooks.original<HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,IUnknown*,const DXGI_SWAP_CHAIN_DESC1*,IDXGIOutput*,IDXGISwapChain1**)>(Bank,2)(f,q,w,d,o,out);
        if(scope.outer&&SUCCEEDED(r)&&out&&*out)h->offer(q,*out);return r;}
    template<unsigned Bank>static HRESULT STDMETHODCALLTYPE composition(IDXGIFactory2* f,IUnknown* q,const DXGI_SWAP_CHAIN_DESC1* d,IDXGIOutput* o,IDXGISwapChain1** out){auto* h=current.load();CreationScope scope;
        const auto r=h->factory_hooks.original<HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,const DXGI_SWAP_CHAIN_DESC1*,IDXGIOutput*,IDXGISwapChain1**)>(Bank,3)(f,q,d,o,out);
        if(scope.outer&&SUCCEEDED(r)&&out&&*out)h->offer(q,*out);return r;}
    template<unsigned Bank>static HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* c,UINT i,UINT f){auto* h=current.load();
        const bool outer=present_depth++==0;
        if(outer&&h->late)h->late_present(c);
        // The panel has to reach the back buffer BEFORE it is presented. Drawing
        // after the original returns puts it in the buffer the game renders over
        // next, which counts a draw and shows nothing -- observed in Alan Wake 2.
        // The research host has always drawn here (before_present).
        if(outer){h->adapter.present(reinterpret_cast<std::uint64_t>(c));if(h->overlay)h->overlay->present(c);}
        const auto r=h->present_hooks.original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT)>(Bank,0)(c,i,f);
        --present_depth;
        if(outer){++h->presents;if(r==S_OK&&!(f&DXGI_PRESENT_TEST))++h->successful_presents;}
        return r;}
    template<unsigned Bank>static HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* c,UINT i,UINT f,const DXGI_PRESENT_PARAMETERS* p){auto* h=current.load();
        const bool outer=present_depth++==0;
        if(outer&&h->late)h->late_present(c);
        if(outer){h->adapter.present(reinterpret_cast<std::uint64_t>(c));if(h->overlay)h->overlay->present(c);} // see present(): before, not after
        const auto r=h->present_hooks.original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*)>(Bank,1)(c,i,f,p);
        --present_depth;
        if(outer){++h->presents;if(r==S_OK&&!(f&DXGI_PRESENT_TEST))++h->successful_presents;}
        return r;}
    template<unsigned Bank>static HRESULT STDMETHODCALLTYPE resize(IDXGISwapChain* c,UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags){auto* host=current.load();
        ResizeScope scope;if(!scope.outer)return host->present_hooks.original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT)>(Bank,2)(c,n,w,h,f,flags);
        ++host->resizes;host->adapter.invalidate();
        if(host->overlay&&!host->overlay->before_resize(c))return DXGI_ERROR_WAS_STILL_DRAWING;
        const auto result=host->present_hooks.original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT)>(Bank,2)(c,n,w,h,f,flags);
        if(SUCCEEDED(result)&&host->overlay){ComPtr<IDXGISwapChain3> chain;if(SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&chain))))host->overlay->resized_default_queue(chain.Get());}return result;}
    template<unsigned Bank>static HRESULT STDMETHODCALLTYPE resize1(IDXGISwapChain3* c,UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags,const UINT* masks,IUnknown* const* queues){auto* host=current.load();
        ResizeScope scope;if(!scope.outer)return host->present_hooks.original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*)>(Bank,3)(c,n,w,h,f,flags,masks,queues);
        ++host->resizes;host->adapter.invalidate();
        if(host->overlay&&!host->overlay->before_resize(c))return DXGI_ERROR_WAS_STILL_DRAWING;
        const auto result=host->present_hooks.original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*)>(Bank,3)(c,n,w,h,f,flags,masks,queues);
        if(SUCCEEDED(result)&&host->overlay&&queues){
            try{DXGI_SWAP_CHAIN_DESC1 desc{};need(SUCCEEDED(c->GetDesc1(&desc))&&desc.BufferCount<=8,"Unsupported resized queue count");
                std::vector<ComPtr<ID3D12CommandQueue>> owners;std::vector<ID3D12CommandQueue*> resolved;
                for(unsigned i=0;i<desc.BufferCount;++i){owners.push_back(host->native_queue(queues[i]));resolved.push_back(owners.back().Get());}
                host->overlay->resized_queues(c,resolved);
            }catch(const std::exception& e){host->controller.diagnostic(std::string("Overlay resize: ")+e.what());host->overlay->resized_queues(c,{});}
        }else if(SUCCEEDED(result)&&host->overlay)host->overlay->resized_default_queue(c);return result;}
    template<unsigned Bank>static HRESULT STDMETHODCALLTYPE set_color(IDXGISwapChain3* c,DXGI_COLOR_SPACE_TYPE color){auto* host=current.load();
        const auto result=host->present_hooks.original<HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,DXGI_COLOR_SPACE_TYPE)>(Bank,4)(c,color);
        if(SUCCEEDED(result)&&host->overlay)host->overlay->color_space(c,color);return result;}
    template<unsigned Bank>static HookBank<4>::Entries factory_detours(){return {reinterpret_cast<void*>(&create<Bank>),reinterpret_cast<void*>(&hwnd<Bank>),reinterpret_cast<void*>(&core<Bank>),reinterpret_cast<void*>(&composition<Bank>)};}
    template<unsigned Bank>static HookBank<5>::Entries present_detours(){return {reinterpret_cast<void*>(&present<Bank>),reinterpret_cast<void*>(&present1<Bank>),reinterpret_cast<void*>(&resize<Bank>),reinterpret_cast<void*>(&resize1<Bank>),reinterpret_cast<void*>(&set_color<Bank>)};}

    void factory(IUnknown* value){ComPtr<IDXGIFactory2> f;need(SUCCEEDED(value->QueryInterface(IID_PPV_ARGS(&f))),"Native DXGI factory2 unavailable");
        auto** v=*reinterpret_cast<void***>(f.Get());factory_hooks.install({v[10],v[15],v[16],v[24]},
            {factory_detours<0>(),factory_detours<1>(),factory_detours<2>(),factory_detours<3>()});}

    void offer(IUnknown* q,IDXGISwapChain* c)noexcept{
        if(stopping)return;try{
            // Everything this hook is handed that we cannot drive is SKIPPED, not
            // treated as a host failure. A D3D11 chain, a composition chain, or a
            // chain Streamline made for frame generation all arrive here, and any
            // one of them failing the host costs the whole session: on Alan Wake 2
            // the first (real) swapchain was
            // hooked and the panel was up, then a later one was refused with
            // "Only D3D12 swapchains admitted" and NR answered "Host is
            // unavailable" for the rest of the run.
            ComPtr<IDXGISwapChain3> chain;
            if(!c||FAILED(c->QueryInterface(IID_PPV_ARGS(&chain)))){++foreign_swapchains;return;}
            // The queue may be a Streamline proxy rather than a native queue, so
            // it is unwrapped through the public native interface BEFORE it is
            // judged. A raw QueryInterface here is what misjudged it.
            ComPtr<ID3D12CommandQueue> native;
            if(q)try{native=native_queue(q);}catch(...){}
            if(!native){++foreign_swapchains;return;}
            auto** v=*reinterpret_cast<void***>(chain.Get());present_hooks.install({v[8],v[22],v[13],v[39],v[38]},
                {present_detours<0>(),present_detours<1>(),present_detours<2>(),present_detours<3>()});
            if(overlay){try{overlay->attach(chain.Get(),native.Get());}catch(const std::exception& e){controller.diagnostic(std::string("Overlay attachment: ")+e.what());}}
            // The research host's fallback too: a device created before the
            // D3D12CreateDevice hook still gets its signature creation observed
            // from here on (never retroactively). Hook banks deduplicate.
            if(indirect_observing.load())try{ComPtr<ID3D12Device> d;
                if(SUCCEEDED(native->GetDevice(IID_PPV_ARGS(&d))))indirect_observer::device(d.Get());
            }catch(const std::exception& e){controller.diagnostic(std::string("Indirect metadata device: ")+e.what());}
            ++swapchains;queue_available=true;
            {std::lock_guard lock(seed_mutex);if(!seed_taken&&!pending_queue)pending_queue=q;}
        }catch(const std::exception& e){hook_fault=true;controller.diagnostic(e.what());}
        catch(...){hook_fault=true;}
    }

    // The late-path twin of offer(). offer() is handed a swapchain and the queue
    // that created it; here the running game hands us only a swapchain, on the
    // first Present after we hooked the implementation we discovered.
    void adopt(IDXGISwapChain* c)noexcept{
        bool expected=false;
        if(!adopted.compare_exchange_strong(expected,true))return; // first Present wins, once
        try{
            ComPtr<IDXGISwapChain3> chain;need(SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&chain))),"Native swapchain3 unavailable");
            // We were called through our own hook, so this should always hold; it
            // is asserted because a mismatch would mean we patched an
            // implementation the game does not use and everything after is noise.
            need(latebind::matches(discovery,chain.Get()),"Adopted swapchain uses a different implementation than the one discovered");
            ComPtr<ID3D12Device> device;
            need(SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device))),"Adopted swapchain is not D3D12");
            // The NR seed. Not the game's present queue -- D3D12 exposes no way to
            // get that -- but a DIRECT queue on the game's own device, which shares
            // the game's queue implementation. The submission router reads
            // ExecuteCommandLists out of the seed's vtable and re-verifies it per
            // submission, so it still binds the game's own submissions.
            D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue> seed;
            need(SUCCEEDED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&seed))),"Seed queue on the adopted device");
            ++swapchains;queue_available=true;
            {std::lock_guard lock(seed_mutex);if(!seed_taken&&!pending_queue)pending_queue=seed;}
            // Identity and device only; the chain itself is not kept.
            {std::lock_guard lock(late_mutex);late_device=device;}
            if(late_panel)late_panel->adopt(c);
            controller.diagnostic("Late attach adopted the running swapchain; NR seeded from its device. "
                                 "The panel waits for the game's own render queue, which the bridge reports "
                                 "after the first admitted RR submission.");
        }catch(const std::exception& e){hook_fault=true;controller.diagnostic(std::string("Late adoption: ")+e.what());}
        catch(...){hook_fault=true;}
    }

    // Every outer Present on the late path, before the panel draws. The chain is
    // touched here and only here, through the pointer this call was handed.
    void late_present(IDXGISwapChain* c)noexcept{
        if(!c)return;
        if(!adopted.load())adopt(c);
        if(!late_panel||!late_panel->follow(c))return;
        // Only the followed chain's presents are evidence; the witness is fed
        // before the panel draws, so a frame's own game transition is judged.
        if(auto* w=present_witness.load(std::memory_order_acquire)){
            ComPtr<IDXGISwapChain3> chain;if(SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&chain))))w->present(chain.Get());}
        if(late_open_panel.exchange(false))late_panel->open_when_attached();
        // The panel attaches only once NR has been prepared: before the bridge
        // exists there is no moment at which this process can name a queue that
        // is the game's own rather than one of ours.
        late_panel->bind(c,live.load()!=nullptr&&!hook_fault&&!stopping,GetTickCount64());
    }

    // The queue the panel submits on. It draws into the back buffer right before
    // Present, so it must be the queue that PRESENTS -- not the queue that ran
    // RR. That used to be the latter, an inference from "the game uses one
    // graphics queue"; on Halo: Campaign Evolved (UE5, Streamline frame
    // generation loaded) the first panel draw -- pressing Insert, NR still OFF --
    // ended in a GPU crash. A D3D12 swapchain may report the queue
    // it was created with through GetDevice; when it does not, the queue comes
    // from evidence (the witness), never from inference.
    latebind::LatePanel::Answer resolve_present_queue(IDXGISwapChain3* chain){
        ComPtr<ID3D12CommandQueue> reported;std::string refused;
        {std::lock_guard lock(late_mutex);
            // Ask the followed object, then -- if Streamline wraps it -- the object
            // underneath. On Halo the adopted chain answered
            // GetDevice(ID3D12Device) but not GetDevice(ID3D12CommandQueue), which
            // fits a proxy that forwards the device only, and equally fits DXGI
            // simply not answering the queue. Every answer is recorded so a run
            // tells the two apart. Probed once per chain; a found queue is re-asked.
            const bool probed=late_panel_probed_chain==chain&&late_panel_probe.is_object()&&!late_panel_probe.value("present_queue_found",false);
            if(!probed){
                late_panel_probed_chain=chain;
                const HRESULT direct=chain->GetDevice(IID_PPV_ARGS(&reported));
                bool unwrapped=false;HRESULT underneath=S_FALSE;
                if(FAILED(direct)||!reported){
                    reported.Reset();
                    const auto base=sl_native(chain,&unwrapped);
                    ComPtr<IDXGISwapChain> native_chain;
                    if(unwrapped&&SUCCEEDED(base.As(&native_chain)))underneath=native_chain->GetDevice(IID_PPV_ARGS(&reported));
                }
                late_panel_probe={{"direct_get_queue_hr",static_cast<std::uint32_t>(direct)},{"sl_proxy_unwrapped",unwrapped},
                    {"underneath_get_queue_hr",static_cast<std::uint32_t>(underneath)},{"present_queue_found",reported!=nullptr}};
            }
            refused=witness_refused_note;
        }
        latebind::LatePanel::Answer a;
        if(reported)a={native_queue(reported.Get()),"swapchain-reported",{}};
        else{
            // The chain does not name its queue (RE9: E_NOINTERFACE, no
            // Streamline proxy). Watch the game transition the current back buffer
            // into PRESENT instead, and take the queue that executed it only once
            // the evidence holds for a run of frames.
            if(present_witness_refused.load())return {nullptr,{},refused};
            auto* w=present_witness.load(std::memory_order_acquire);
            if(!w){witness_wanted=true;return {nullptr,{},"waiting-for-the-present-transition-witness (the swapchain does not report its present queue)"};}
            const auto decided=w->decided();
            if(!decided)return {nullptr,{},"waiting-for-present-transition-evidence (see present_queue_witness)"};
            a={native_queue(decided.Get()),"present-transition-witness",{}};
        }
        // Recorded, not acted on: whether RR and Present share a queue is the
        // fact that explains (or rules out) the Halo crash above.
        {std::lock_guard lock(late_mutex);const void* rr=rr_queue.load();late_panel_queue_match=!rr?-1:(rr==a.queue.Get()?1:0);}
        return a;
    }
    latebind::LatePanel::Evidence late_evidence(){
        latebind::LatePanel::Evidence e;
        // A take-over must be the implementation we hooked, on the device NR was
        // seeded from; anything else is refused by name and never followed.
        e.accept=[this](IDXGISwapChain3* chain,std::string& why){
            if(!latebind::matches(discovery,chain)){why="different-swapchain-implementation";return false;}
            ComPtr<ID3D12Device> device;if(FAILED(chain->GetDevice(IID_PPV_ARGS(&device)))){why="not-d3d12";return false;}
            std::lock_guard lock(late_mutex);if(device.Get()!=late_device.Get()){why="different-device";return false;}
            return true;};
        e.resolve=[this](IDXGISwapChain3* chain){return resolve_present_queue(chain);};
        // Only the witness can be contradicted; a queue the chain reported is not
        // re-judged (unchanged from before).
        e.contradicted=[this](const std::string& source){
            if(!source.empty()&&source!="present-transition-witness")return false;
            auto* w=present_witness.load(std::memory_order_acquire);return w&&w->contradicted();};
        e.restart=[this](std::uint64_t now){auto* w=present_witness.load(std::memory_order_acquire);return w&&w->restart(now);};
        e.rebind=[this]{
            {std::lock_guard lock(late_mutex);late_panel_probed_chain=nullptr;late_panel_probe=nullptr;late_panel_queue_match=-1;}
            if(auto* w=present_witness.load(std::memory_order_acquire))w->rebind();};
        e.report=[this](const std::string& note){controller.diagnostic("Late panel: "+note);};
        return e;
    }
    // Worker side of the late panel: build the witness when the detour asked for
    // it, and note RR's queue. Neither touches the swapchain.
    void service_late_panel()noexcept{
        try{
            if(witness_wanted.load()&&!present_witness.load()&&!present_witness_refused.load()){
                ComPtr<ID3D12Device> device;{std::lock_guard lock(late_mutex);device=late_device;}
                if(device)try{
                    present_witness.store(PresentQueueWitness::install(device.Get()),std::memory_order_release);
                    controller.diagnostic("Late panel: the swapchain does not report its present queue; watching which queue "
                                          "executes the game's transition of the current back buffer into PRESENT.");
                }catch(const std::exception& e){
                    const std::string note=std::string("swapchain-does-not-report-its-present-queue and the present-transition witness could not be installed (")+e.what()+"); panel disabled, control through overglazectl";
                    {std::lock_guard lock(late_mutex);witness_refused_note=note;}
                    present_witness_refused=true;controller.diagnostic("Late panel: "+note);
                }
            }
            if(!rr_queue.load())if(auto* l=live.load())if(const auto q=l->render_queue())rr_queue.store(q.Get());
        }catch(...){}
    }

    // Injected into a running game: find this process's swapchain implementation
    // and hook it. Nothing is adopted here -- the next real Present does that.
    // The early path (the root proxy's first factory): before the game creates its
    // D3D12 device, exactly where the research host starts the same observer.
    // Only a package that preserves the game's bindings reads signature layouts,
    // so only that one gets the hooks (a fixture asks through the environment).
    // A failure is a diagnostic: the strict policy then names every unobserved
    // ExecuteIndirect, as before this existed.
    void start_early()noexcept{
        wchar_t preserve[4]{};
        const bool wanted=fixture?GetEnvironmentVariableW(L"OVERGLAZE_FIXTURE_BINDING_PRESERVATION",preserve,4)==1&&preserve[0]==L'1'
                                 :installation&&installation->facts.binding_preservation;
        if(!wanted)return;
        try{indirect_observer::start();indirect_observing=true;}
        catch(const std::exception& e){indirect_error=e.what();controller.diagnostic(std::string("Indirect metadata observer: ")+e.what());}
        catch(...){indirect_error="unknown failure";}
    }
    void start_late(){
        late=true;
        discovery=latebind::discover_swapchain_methods();
        late_note=discovery.agility?"agility":"system-d3d12";
        // Built before the hooks exist: the detours read it.
        if(overlay)late_panel.emplace(*overlay,late_evidence());
        present_hooks.install({discovery.methods[0],discovery.methods[1],discovery.methods[2],discovery.methods[3],discovery.methods[4]},
            {present_detours<0>(),present_detours<1>(),present_detours<2>(),present_detours<3>()});
        late_started_at=GetTickCount64();
    }

    // The same public-export path the research host uses; the private RVA table
    // is not reachable from this track at all.
    // The object underneath a Streamline proxy, through the signed interposer's
    // own export. Returns the source itself when there is no interposer or the
    // object is not one of its proxies; `unwrapped` says which.
    // The verdict is cached per loaded interposer module, a failure included: the
    // check hashes and signature-verifies the file, and on the late path it now
    // runs in a Present detour, where it must not repeat per call.
    std::mutex sl_check_mutex;HMODULE sl_checked=nullptr;PFun_slGetNativeInterface* sl_checked_native=nullptr;std::string sl_check_error;
    ComPtr<IUnknown> sl_native(IUnknown* source,bool* unwrapped=nullptr){
        if(unwrapped)*unwrapped=false;
        ComPtr<IUnknown> normalized=source;HMODULE sl=GetModuleHandleW(L"sl.interposer.dll");
        if(sl){PFun_slGetNativeInterface* native=nullptr;
            {std::lock_guard lock(sl_check_mutex);
                if(sl_checked!=sl){sl_checked=sl;sl_checked_native=nullptr;sl_check_error="Queue interposer verification failed";
                    try{const auto identity=module_identity(sl);
                        need(identity.value("signature","")=="ValidCachedTrust","Queue interposer is not a valid signed NVIDIA module");
                        sl_checked_native=reinterpret_cast<PFun_slGetNativeInterface*>(lab::slpublic::verified_export(sl,"slGetNativeInterface"));
                        need(sl_checked_native!=nullptr,"Native-interface export missing or outside the verified image");
                    }catch(const std::exception& e){sl_checked_native=nullptr;sl_check_error=e.what();}catch(...){sl_checked_native=nullptr;}}
                need(sl_checked_native!=nullptr,sl_check_error.c_str());native=sl_checked_native;}
            void* base=nullptr;
            if(native(source,&base)==sl::Result::eOk&&base&&base!=source){
                normalized=static_cast<IUnknown*>(base);if(unwrapped)*unwrapped=true;}
        }
        return normalized;
    }
    ComPtr<ID3D12CommandQueue> native_queue(IUnknown* source){
        const auto normalized=sl_native(source);
        ComPtr<ID3D12CommandQueue> queue;need(SUCCEEDED(normalized->QueryInterface(IID_PPV_ARGS(&queue))),"Native D3D12 queue unavailable");
        return queue;
    }

    void run(){
        bool nr_fault_saved=false;
        try{while(!stopping){
            if(!fixture&&!console_started){console_started=true;
                try{open_installed_console(*installation);}catch(const std::exception& e){controller.diagnostic(e.what());}}
            {std::lock_guard lock(adapter_worker_mutex);adapter.poll();}
            // Late loading brings its own panel up: ask for preparation once,
            // NR still OFF, so the operator does not have to drive a pipe.
            if(late&&!hook_fault&&adopted.load()&&!late_prepare_requested)
                late_prepare_requested=controller.request_late_attach_preparation();
            if(!hook_fault&&controller.take_nr_preparation(GetTickCount64()))preparation_requested=true;
            ComPtr<IUnknown> seed;{std::lock_guard lock(seed_mutex);if(!hook_fault&&!observe_only&&preparation_requested&&!seed_taken&&pending_queue){seed=std::move(pending_queue);seed_taken=true;}}
            if(seed&&!hook_fault){
                // Do not let an Evaluate that started before preparation carry
                // startup tags/resources into the newly published receiver.
                adapter.invalidate();auto q=native_queue(seed.Get());
                auto* client=new LiveRuntimeClient(sink,q.Get(),module,
                    fixture?LiveHostKind::native_fixture:LiveHostKind::native_game,
                    fixture?nullptr:&*installation,1u);
                // Self-configuring: the package's viewport/depth facts are no
                // longer needed; each RR call states them (lab_sl_frame_provider.hpp).
                if(!ngx_admit)provider.emplace(*client,fixture?FrameProfile{0,false,true}:
                    FrameProfile{installation->facts.viewport,installation->facts.linear_depth,true});
                live=client; // published last: the admission sink reads it to decide
                // The NGX evaluate detour becomes the provider on this route.
                if(ngx_admit&&ngx_observer)ngx_observer->set_receiver(client);
            }
            if(auto* l=live.load()){
                if(hook_fault)l->stop();
                else l->poll(ngx_admit&&ngx_observer?ngx_observer->blocked():provider?provider->blocked():nullptr);
            }
            // The game's latency markers, resolved the way the game resolves them
            // (the signed interposer's public slGetFeatureFunction), once the bridge
            // exists on a Streamline route. Self-configuring, a Present already
            // expires no tag, so here they decide nothing yet; they are published.
            if(live.load()&&!hook_fault&&!ngx_admit&&provider&&markers.due(GetTickCount64())){
                PFun_slGetFeatureFunction* resolver=nullptr;HMODULE interposer{};
                if(!fixture&&GetModuleHandleExW(0,L"sl.interposer.dll",&interposer)){struct Reference{HMODULE m;~Reference(){FreeLibrary(m);}} hold{interposer};
                    try{resolver=reinterpret_cast<PFun_slGetFeatureFunction*>(lab::slpublic::verified_export(interposer,"slGetFeatureFunction"));}catch(...){resolver=nullptr;}
                    markers.poll(resolver,false,GetTickCount64());}
                else markers.poll(nullptr,fixture,GetTickCount64());} // none: counted, settles
            // eOnlyValidNow depth/motion are copied at their own tag call
            // only while the runtime would use an admitted frame (waiting for one,
            // or NR ON); never on the NGX routes, where no SL frame is admitted.
            {auto* l=live.load();const bool want=l&&!hook_fault&&!stopping&&!ngx_admit&&provider&&l->wants_frames();
                std::lock_guard lock(adapter_worker_mutex);adapter.service_tag_copies(want);}
            if(ngx_observer&&!hook_fault&&!ngx_observer->attached()&&!ngx_observer->refused())
                (void)ngx_observer->attach();
            // The panel is attached, detached on a contradiction and re-decided in
            // the Present detour (late_present), the only place the chain is
            // touched. A contradicted queue is never drawn on again; the witness
            // gathers a fresh run under the same rules, bounded (withdrawing for
            // good left RE9's panel unopenable once FG was on).
            if(late&&late_panel&&!hook_fault)service_late_panel();
            // A terminal NR fault used to exist only in memory and vanished with
            // the process. Persist the runtime and adapter snapshots once.
            if(!nr_fault_saved&&live.load()&&installation&&!installation->run.empty()){
                const auto control=controller.status();const auto runtime=control.value("nr_runtime",json::object());
                if(runtime.value("state","")=="failed"){nr_fault_saved=true;try{
                    json fault{{"kind","overglaze-nr-fault-v1"},{"pid",GetCurrentProcessId()},{"tick_ms",GetTickCount64()},{"executable",executable},{"host_module",module_info},
                        {"nr_runtime",runtime},{"nr_adapter",control.value("nr_adapter",json())},{"host",control.value("host",json())},
                        {"scope","first terminal NR fault of this process, the runtime's own counters at that moment; not a crash record and not GPU proof"}};
                    auto body=fault.dump(2);need(body.size()<=1024*1024,"NR fault record exceeds 1 MiB");
                    const auto file=installation->run/L"nr-fault.json";
                    Handle out(CreateFileW(file.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,0,nullptr));need(out.valid(),"Create NR fault record");
                    DWORD written=0;need(WriteFile(out.value,body.data(),DWORD(body.size()),&written,nullptr)&&written==body.size(),"Write NR fault record");
                }catch(const std::exception& e){controller.diagnostic(e.what());}}
            }
            {std::lock_guard lock(adapter_worker_mutex);controller.publish_nr_adapter(adapter.snapshot());}
            controller.frame_boundary(0,GetTickCount64());
            const auto f=factory_hooks.snapshot(),p=present_hooks.snapshot();
            controller.publish_host({{"backend","controller-d3d12"},{"origin",fixture?"synthetic":"game"},{"module",module_info},{"executable",executable},
                {"state",hook_fault?"failed":live.load()?"connected":queue_available?"standby":"awaiting-native-swapchain"},
                {"automatic_lifecycle",true},{"installation_config",installation.has_value()},
                {"profile",installation?installation->profile:std::string("synthetic-standalone-fixture")},
                {"overlay",overlay?overlay->snapshot():json(nullptr)},
                {"factory_hooks",f},{"present_hooks",p},
                {"presents",presents.load()},{"successful_presents",successful_presents.load()},
                {"resizes",resizes.load()},{"swapchains",swapchains.load()},{"foreign_swapchains",foreign_swapchains.load()},
                {"research_observers",false},{"root_proxy",proxy_status()},
                {"latency_markers",markers.summary()},
                {"indirect_signatures",[&]{if(!indirect_observing.load())return indirect_error.empty()?json(nullptr):json{{"error",indirect_error}};
                    const auto s=indirect_observer::status();
                    return json{{"recorded",s.value("recorded",0ULL)},{"unsupported",s.value("unsupported",0ULL)},{"failures",s.value("failures",0ULL)},
                        {"scope","command-signature creation metadata from the early path; signatures created before it are unobserved and refused (strict)"}};}()},
                {"route",installation?installation->facts.route:std::string("synthetic")},
                {"sl_self_config",[&]{if(ngx_admit)return json(nullptr);
                    const auto s=adapter.viewport_selection();json out=provider?provider->observed():json(nullptr);
                    if(!out.is_object())out=json::object();
                    out["viewport_selection"]={{"locked",s.locked},{"viewport",s.locked?json(s.viewport):json(nullptr)},
                        {"switches",s.switches},{"other_viewport_calls",s.other_viewport_calls}};
                    return out;}()},
                {"ngx_observation",ngx_observer?ngx_observer->report():json(nullptr)},
                // The admission counters again, on their own: the full report is
                // the largest key and the first one clipping drops (a Hellblade 2
                // NR fault record lost it that way), and these are what explain a fault.
                {"ngx_admission",ngx_observer?ngx_observer->admission():json(nullptr)},
                {"late_load",late},{"late_panel",!late?json(nullptr):json(late_panel?late_panel->state():std::string("panel-unavailable"))},
                {"late_panel_source",[&]{const auto source=late_panel?late_panel->source():std::string();return source.empty()?json(nullptr):json(source);}()},
                // Followed chain, take-overs, attach/detach/re-decision counts and
                // the last few notes; small, so it survives status clipping.
                {"late_binding",late_panel?late_panel->status():json(nullptr)},
                {"present_queue_witness",[&]{auto* w=present_witness.load();return w?w->status():json(nullptr);}()},
                {"late_panel_probe",[&]{std::lock_guard lock(late_mutex);return late_panel_probe;}()},
                {"late_panel_queue",[&]{if(!late||!late_panel||!late_panel->attached())return json(nullptr);
                    int match=-1;{std::lock_guard lock(late_mutex);match=late_panel_queue_match;}
                    return json(match==1?"present-queue (RR ran on it too)":match==0?"present-queue (RR ran on a different queue)":"present-queue (RR queue not yet observed)");}()},
                {"scope","controller host: control pipe, panel, public Streamline admission and the NR runtime bridge; no research observer, capture or probe is built into this binary"}});
            Sleep(16);
        }
        teardown();
        }catch(const std::exception& e){
            try{teardown();}catch(...){}
            controller.publish_host({{"backend","controller-d3d12"},{"state","failed"},{"error",e.what()},{"origin","game"}});
        }catch(...){
            try{teardown();}catch(...){}
            try{controller.publish_host({{"backend","controller-d3d12"},{"state","failed"},{"error","unknown controller host fault"},{"origin","game"}});}catch(...){}
        }
        worker_done=true;
    }

    void fail_host(const char* why) noexcept {
        hook_fault=true;
        try{controller.publish_host({{"backend","controller-d3d12"},{"state","failed"},{"error",why},{"origin","game"},
            {"module",module_info},{"executable",executable}});}catch(...){}
    }
    // Worker-thread teardown. Everything here touches state the worker owns, so
    // it must never run on a caller thread: LiveRuntimeClient::poll mutates plain
    // members with no lock, and the adapter snapshot is guarded by the worker mutex.
    void teardown() noexcept {
        if(teardown_done.exchange(true))return;
        if(overlay)overlay->stop();
        {std::lock_guard lock(adapter_worker_mutex);adapter.stop();}
        // Stop offering NGX calls before the receiver stops. A detour that has
        // already captured it finishes its sequence against a stopping client,
        // which the runtime refuses cleanly; the client is never deleted.
        if(ngx_observer)ngx_observer->set_receiver(nullptr);
        if(auto* l=live.load())l->stop();
        try{std::lock_guard lock(adapter_worker_mutex);controller.publish_nr_adapter(adapter.snapshot());}catch(...){}
        // The worker publishes the last word on state; a caller-thread publication
        // would be overwritten by the next loop iteration.
        try{auto h=controller.status().value("host",json::object());h["state"]="stopped";controller.publish_host(h);}catch(...){}
    }
    // The stop entry raises the flag and waits, bounded, for the worker to finish.
    // It never tears anything down itself.
    void stop() noexcept {
        if(stopping.exchange(true))return;
        for(unsigned i=0;i<200&&!worker_done.load();++i)Sleep(10);
    }
};
}

}

// The root proxy hands us the game's factory here, on the game's first
// CreateDXGIFactory. This is the whole early-loading path now: the proxy in the
// game directory carries no host at all, so the host arrives at renderer time
// with the loader quiescent, and gets the swapchain's own creation queue.
extern "C" __declspec(dllexport) void __cdecl LabControllerFactoryCreated(IUnknown* factory,HMODULE proxy){
    lab::root_proxy_module.store(proxy,std::memory_order_release);
    lab::standalone::factory_created(factory);
}

// The bootstrap's only non-DXGI export: an orderly stop for a host that wants to
// unload us. It never forces GPU drain and never claims one.
// The root proxy's on-Insert entry: the player pressed Insert in the running
// game, and the proxy -- which loaded nothing while the game started -- loaded
// us and calls this from its own worker thread, off the loader lock. From here
// on it is exactly a late attach; the proxy's module is kept for the status.
extern "C" __declspec(dllexport) void __cdecl LabControllerStartLate(HMODULE proxy,unsigned flags){
    lab::root_proxy_module.store(proxy,std::memory_order_release);
    lab::standalone::start_late_attach(flags);
    // The loader's own worker may have started the host first (it looks the
    // proxy up by name, and two dxgi.dll modules are mapped); the start runs
    // once either way, so the panel request is applied to whichever host won.
    if(flags&lab::standalone::late_open_panel)if(auto* h=lab::current.load())h->late_open_panel=true;
}

extern "C" __declspec(dllexport) void __cdecl LabControllerStop(){
    if(auto* h=lab::current.load())h->stop();
}

// Test-only. Public evidence only: four public API addresses, the RR options
// setter and a synthetic slGetNativeInterface, all inside the harness image.
// No private begin/end pair and no host-flags address are accepted, because the
// controller never attaches a research discovery and the adapter refuses them.
extern "C" __declspec(dllexport) bool __cdecl LabControllerAttachFixture(const lab::ControllerFixtureBindings* b){
    auto* h=lab::current.load();
    if(!h||!h->fixture||!b||b->size!=sizeof(*b)||b->version!=2||b->frame_tagging>1)return false;
    try{
        auto exe=GetModuleHandleW(nullptr);
        for(auto* p:b->public_api)if(!lab::fixture_address(p,true,exe))return false;
        if(!lab::fixture_address(b->options,true,exe)||!lab::fixture_address(b->native_interface,true,exe))return false;
        if(b->sr_options&&!lab::fixture_address(b->sr_options,true,exe))return false;
        // Latch only once the addresses are known good, so a bad call does not
        // burn the one-shot and make every later attach fail for the wrong reason.
        if(h->fixture_attached.exchange(true))return false;
        std::lock_guard lock(h->adapter_worker_mutex);
        return h->adapter.attach_fixture({b->public_api[0],b->public_api[1],b->public_api[2],b->public_api[3]},
            b->options,{nullptr,nullptr},nullptr,b->frame_tagging!=0,nullptr,false,
            reinterpret_cast<PFun_slGetNativeInterface*>(b->native_interface),b->sr_options);
    }catch(...){return false;}
}

namespace lab {
namespace standalone {
// The DXGI proxy calls this for every factory the game creates. One host is
// started on the first one; later factories only get their creation vtable
// hooked so a second factory cannot escape admission.
// The late entry: called by the injected loader's own worker thread, never
// under the loader lock and never from DllMain. Unlike factory_created there is
// no game call to ride in on, so this both starts the host and begins discovery.
void start_late_attach(unsigned flags) noexcept {
    try{
        HMODULE self{};
        need(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&start_late_attach),&self)!=0,"Cannot pin controller host");
        // Late loading is for installed games only. There is no fixture branch
        // here: a fixture has no installation, and admitting one on this path
        // would mean an injected DLL could start a host from the environment.
        if(!has_installation(self))return;
        // V4: the program folder or its data root was deleted under an installed
        // game. Stand down silently: nothing hooked, nothing shown, nothing written.
        if(installation_root_missing(self))return;
        startup::once(once,[self,flags]{
            auto* h=new Host(self);current=h;h->late_open_panel=(flags&lab::standalone::late_open_panel)!=0;h->start_late();std::thread([h]{h->run();}).detach();
        },[self](const char* why)noexcept{
            try{if(auto* h=current.load())h->fail_host(why);}catch(...){}
            startup::report_async(self,why);
        });
    }catch(const std::exception& e){try{if(auto* h=current.load())h->fail_host(e.what());}catch(...){} }
    catch(...){OutputDebugStringW(L"Overglaze controller: unexpected late attach failure.\n");}
}

void factory_created(IUnknown* value) noexcept {
    try{
        HMODULE self{};
        need(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&factory_created),&self)!=0,"Cannot pin controller host");
        // No validated installation beside this proxy means the game is not ours
        // to touch: forward DXGI and do nothing else.
        // A fixture is admitted only where there is no installation at all; the
        // Host constructor then applies the full identity rule.
        if(!has_installation(self)&&!flag(L"OVERGLAZE_CONTROLLER_FIXTURE"))return;
        // V4: the program folder or its data root was deleted under an installed
        // game. The proxy keeps forwarding DXGI; the host never starts and says
        // nothing (no dialog, no startup record, no hooks).
        if(has_installation(self)&&installation_root_missing(self))return;
        startup::once(once,[self]{
            // The early path: the signature observer is in place before the game's
            // D3D12CreateDevice, which follows this, its first factory.
            auto* h=new Host(self);current=h;h->start_early();std::thread([h]{h->run();}).detach();
        },[self](const char* why)noexcept{
            try{if(auto* h=current.load())h->fail_host(why);}catch(...){}
            startup::report_async(self,why);
        });
        if(auto* h=current.load())if(!h->stopping&&!h->hook_fault)h->factory(value);
    }catch(const std::exception& e){try{if(auto* h=current.load())h->fail_host(e.what());}catch(...){} }
    catch(...){OutputDebugStringW(L"Overglaze controller: unexpected factory observation failure.\n");}
}
}
}
