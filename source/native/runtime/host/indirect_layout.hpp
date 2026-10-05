// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <type_traits>

namespace lab::host::indirect {
// Process-local POD exchanged by the early host observer and the NR state block.
// The signature owns this private data and already retains its root signature.
// No address-keyed cache, argument-buffer readback or driver-private ABI.
inline constexpr GUID layout_key={0x4101efc8,0xb39a,0x426d,{0x9f,0x41,0x53,0x67,0xe5,0x4a,0xf8,0x22}};
enum class Error:std::uint32_t {none,missing,capacity,operation,argument,range,root,stride};
struct Layout {
    std::uint32_t magic=0x4c494e44,version=1,bytes=sizeof(Layout),count=0;
    Error error=Error::missing;
    std::uint32_t graphics=0,stride=0,reserved=0;
    std::uint64_t root=0;
    std::array<D3D12_INDIRECT_ARGUMENT_DESC,128> arguments{};
};
static_assert(std::is_trivially_copyable_v<Layout>);
inline Layout describe(const D3D12_COMMAND_SIGNATURE_DESC* d,ID3D12RootSignature* root)noexcept{
    Layout v;v.root=reinterpret_cast<std::uint64_t>(root);
    if(!d||!d->pArgumentDescs||!d->NumArgumentDescs)return v;
    if(d->NumArgumentDescs>v.arguments.size()){v.error=Error::capacity;return v;}
    v.count=d->NumArgumentDescs;v.stride=d->ByteStride;
    const auto op=d->pArgumentDescs[v.count-1].Type;
    if(op==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW||op==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)v.graphics=1;
    else if(op!=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH){v.error=Error::operation;return v;}
    std::uint64_t minimum=op==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW?16:op==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED?20:12;
    bool root_changed=false;
    for(UINT i=0;i<v.count;++i){const auto& a=d->pArgumentDescs[i];v.arguments[i]=a;if(i+1==v.count)break;
        switch(a.Type){
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
            if(!v.graphics||a.VertexBuffer.Slot>=32){v.error=Error::range;return v;}minimum+=16;break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
            if(op!=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED){v.error=Error::argument;return v;}minimum+=16;break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
            if(a.Constant.RootParameterIndex>=64||!a.Constant.Num32BitValuesToSet||a.Constant.DestOffsetIn32BitValues>=64||a.Constant.Num32BitValuesToSet>64-a.Constant.DestOffsetIn32BitValues){v.error=Error::range;return v;}
            minimum+=4ull*a.Constant.Num32BitValuesToSet;root_changed=true;break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
            if(a.ConstantBufferView.RootParameterIndex>=64){v.error=Error::range;return v;}minimum+=8;root_changed=true;break;
        default:v.error=Error::argument;return v;
        }
    }
    if(root_changed!=(root!=nullptr)){v.error=Error::root;return v;}
    if(d->ByteStride%4||minimum>d->ByteStride){v.error=Error::stride;return v;}
    v.error=Error::none;return v;
}
inline bool read(ID3D12CommandSignature* s,Layout& out)noexcept{
    if(!s)return false;UINT bytes=sizeof(out);
    if(FAILED(s->GetPrivateData(layout_key,&bytes,&out)))return false;
    return bytes==sizeof(out)&&out.bytes==sizeof(out)&&out.magic==0x4c494e44&&out.version==1;
}
}
