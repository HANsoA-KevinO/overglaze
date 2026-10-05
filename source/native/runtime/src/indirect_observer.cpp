// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_indirect_observer.hpp"
#include "lab_hook_bank.hpp"
#include "indirect_layout.hpp"
#include <wrl/client.h>
namespace lab::indirect_observer {
namespace {
HookBank<1> signatures;
HookBank<1,1> devices;
std::atomic<std::uint64_t> recorded{0},unsupported{0},failed{0};
template<unsigned B>HRESULT STDMETHODCALLTYPE create_signature(ID3D12Device* d,const D3D12_COMMAND_SIGNATURE_DESC* desc,
    ID3D12RootSignature* root,REFIID iid,void** out){
    const auto hr=signatures.original<HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_COMMAND_SIGNATURE_DESC*,ID3D12RootSignature*,REFIID,void**)>(B,0)(d,desc,root,iid,out);
    if(SUCCEEDED(hr)&&out&&*out)try{
        Microsoft::WRL::ComPtr<ID3D12CommandSignature> signature;
        if(SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&signature)))){
            // Record the innermost native result too. A wrapper may delegate
            // private data to it; never overwrite that native root identity.
            host::indirect::Layout existing;if(host::indirect::read(signature.Get(),existing))return hr;
            const auto layout=host::indirect::describe(desc,root);
            if(FAILED(signature->SetPrivateData(host::indirect::layout_key,sizeof(layout),&layout)))++failed;
            else {++recorded;if(layout.error!=host::indirect::Error::none)++unsupported;}
        }else ++failed;
    }catch(...){++failed;}
    return hr; // Observation never changes the application's result.
}
HRESULT WINAPI create_device(IUnknown* a,D3D_FEATURE_LEVEL f,REFIID iid,void** out){
    const auto hr=devices.original<PFN_D3D12_CREATE_DEVICE>(0,0)(a,f,iid,out);
    // Observe each successful native/wrapper result; hook banks deduplicate
    // implementations. Selecting only the outer result misses native calls.
    if(SUCCEEDED(hr)&&out&&*out)try{
        Microsoft::WRL::ComPtr<ID3D12Device> d;
        if(SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&d))))device(d.Get());
    }catch(...){++failed;}
    return hr;
}
void pin(){HMODULE module{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&pin),&module))throw std::runtime_error("Pin indirect metadata observer");}
}
void device(ID3D12Device* d){
    if(!d)throw std::logic_error("Missing indirect metadata device");pin();
    signatures.install({(*reinterpret_cast<void***>(d))[create_signature_slot()]},
        {{{reinterpret_cast<void*>(&create_signature<0>)},{reinterpret_cast<void*>(&create_signature<1>)},
          {reinterpret_cast<void*>(&create_signature<2>)},{reinterpret_cast<void*>(&create_signature<3>)}}});
}
void start(){
    pin();const auto m=LoadLibraryExW(L"d3d12.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto fn=m?GetProcAddress(m,"D3D12CreateDevice"):nullptr;
    if(!fn)throw std::runtime_error("Indirect metadata device entry unavailable");
    // Module reference retained with the process-lifetime hook.
    devices.install({reinterpret_cast<void*>(fn)},{{{reinterpret_cast<void*>(&create_device)}}});
}
json status(){return {{"scope","command-signature-creation-metadata-only"},{"recorded",recorded.load()},
    {"unsupported",unsupported.load()},{"failures",failed.load()},{"bytes_per_signature",sizeof(host::indirect::Layout)},
    {"texture_reads",0},{"event_files",0},{"retroactive_recovery",false},
    {"device_hooks",devices.snapshot()},{"signature_hooks",signatures.snapshot()}};}
void uninstall_quiesced_for_test(){devices.uninstall_quiesced_for_test();signatures.uninstall_quiesced_for_test();}
}
