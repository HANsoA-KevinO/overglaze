// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#include "lab_sl_resource_lease.hpp"
namespace lab::slboundary {
// allow_mip_chains (controller, self-configuring): the colour and depth roles
// may carry a mip chain, which the runtime meets through level 0 only; motion
// stays single-mip because NR is handed it directly.
ResourceLease ResourceLease::acquire(const Resolution& r,bool allow_mip_chains,IUnknown* device_command) {
    ResourceLease out;auto& f=out.facts_;
    if(!r.ready() || !r.binding.call || !r.binding.command){f.reason="exact-entry-unavailable";return out;}
    out.identity_=r.binding;
    auto* command=device_command?device_command:static_cast<IUnknown*>(r.binding.command);
    if(FAILED(command->QueryInterface(IID_PPV_ARGS(&out.command_)))){f.reason="not-d3d12-command-list";return out;}
    f.command_type=out.command_->GetType();
    if(f.command_type!=D3D12_COMMAND_LIST_TYPE_DIRECT && f.command_type!=D3D12_COMMAND_LIST_TYPE_COMPUTE){f.reason="unsupported-command-list-type";return out;}
    Microsoft::WRL::ComPtr<ID3D12Device> device;Microsoft::WRL::ComPtr<IUnknown> identity;
    if(FAILED(out.command_->GetDevice(IID_PPV_ARGS(&device))) || FAILED(device.As(&identity))){f.reason="command-device-unavailable";return out;}
    f.same_device=true;
    // The exposure texture the binding carries, if any, also while its tag
    // lifetime still covers it. A failure only leaves the frame without it.
    if(const auto& x=r.binding.exposure;!r.binding.exposure_note&&x.native&&!x.null_resource&&!x.issues)
        if(SUCCEEDED(static_cast<IUnknown*>(x.native)->QueryInterface(IID_PPV_ARGS(&out.exposure_))))out.exposure_native_=x.native;
    for(unsigned i=0;i<3;++i){const auto& tag=r.binding.resources[i];
        if(tag.issues || tag.null_resource || !tag.native || tag.resource_type!=sl::ResourceType::eTex2d){f.reason="invalid-resource-tag";return out;}
        auto* input=static_cast<IUnknown*>(tag.native);
        if(FAILED(input->QueryInterface(IID_PPV_ARGS(&out.resources_[i])))){f.reason="not-d3d12-resource";return out;}
        f.desc[i]=out.resources_[i]->GetDesc();f.described[i]=true;
        Microsoft::WRL::ComPtr<ID3D12Device> owner;Microsoft::WRL::ComPtr<IUnknown> owner_identity;
        if(FAILED(out.resources_[i]->GetDevice(IID_PPV_ARGS(&owner))) || FAILED(owner.As(&owner_identity))){f.reason="resource-device-unavailable";return out;}
        f.same_device=f.same_device && owner_identity.Get()==identity.Get();
        const auto& d=f.desc[i];const auto& e=tag.extent;
        // Keep describing the remaining roles after a shape/extent refusal; the
        // first refusal is the one reported, and nothing is admitted.
        if(f.failed_role<0&&(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || !d.Width || !d.Height || d.Width>8192 || d.Height>8192 ||
           (allow_mip_chains&&i<2?d.MipLevels<1:d.MipLevels!=1) || d.DepthOrArraySize!=1 || d.SampleDesc.Count!=1)){f.reason="unsupported-resource-shape";f.failed_role=int(i);continue;}
        if(f.failed_role<0&&(e.left || e.top || ((e.width || e.height) && (e.width!=d.Width || e.height!=d.Height)))){f.reason="tag-extent-mismatch";f.failed_role=int(i);continue;}
    }
    if(f.failed_role>=0)return out;
    f.inspected=true;f.reason=f.same_device?"descriptors-observed":"device-identity-mismatch";return out;
}
bool ResourceLease::matches(const Resolution& r) const noexcept {
    const auto& b=r.binding;const auto& old=identity_;
    if(!facts_.inspected || !r.ready() || old.call!=b.call || old.token_generation!=b.token_generation || old.frame_index!=b.frame_index ||
       old.viewport!=b.viewport || old.thread!=b.thread || old.command!=b.command || old.present_epoch!=b.present_epoch)return false;
    for(unsigned i=0;i<3;++i)if(old.resources[i].native!=b.resources[i].native || old.tag_calls[i]!=b.tag_calls[i])return false;
    return true;
}
json describe(const ResourceFacts& f) {
    json out={{"status",f.reason},{"inspected",f.inspected},{"entry_return_matched",f.matched_return},
        {"gpu_lifetime_verified",false},{"allocation_aliasing_verified",false},{"color_domain_verified",false},
        {"resource_states_verified",false},{"texture_readbacks",0}};
    const char* names[]={"rr-output","depth","motion"};
    if(f.inspected){out["same_com_device_identity"]=f.same_device;out["command_type"]=f.command_type;out["resources"]=json::array();
        for(unsigned i=0;i<3;++i){const auto& d=f.desc[i];
            out["resources"].push_back({{"role",names[i]},{"width",d.Width},{"height",d.Height},{"format",d.Format},{"flags",d.Flags}});}}
    else if(f.failed_role>=0){
        out["failed_role"]=names[f.failed_role];out["resources"]=json::array();
        for(unsigned i=0;i<3;++i){if(!f.described[i])continue;const auto& d=f.desc[i];
            out["resources"].push_back({{"role",names[i]},{"dimension",unsigned(d.Dimension)},{"width",d.Width},{"height",d.Height},
                {"mips",d.MipLevels},{"array",d.DepthOrArraySize},{"samples",d.SampleDesc.Count},{"format",unsigned(d.Format)},{"flags",unsigned(d.Flags)}});}
        out["scope"]="shape of every role the game tagged, as D3D12 describes it; dimension 3 = TEXTURE2D";
    }
    return out;
}
}
