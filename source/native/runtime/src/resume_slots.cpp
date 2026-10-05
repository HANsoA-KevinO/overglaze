// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#define CINTERFACE
#include <d3d12.h>
#include "lab_resume_slots.hpp"
namespace lab::resume {
std::array<std::size_t,Count> slots(){
#define S(x) offsetof(ID3D12GraphicsCommandListVtbl,x)/sizeof(void*)
    return {S(Reset),S(Close),S(SetPipelineState),S(SetDescriptorHeaps),S(SetComputeRootSignature),S(SetComputeRootDescriptorTable),
        S(SetComputeRootConstantBufferView),S(SetComputeRootShaderResourceView),S(SetComputeRootUnorderedAccessView),
        S(SetComputeRoot32BitConstant),S(SetComputeRoot32BitConstants),S(ClearState),S(SetGraphicsRootSignature),
        S(ExecuteBundle),S(ExecuteIndirect),S(SetPredication),
        offsetof(ID3D12GraphicsCommandList4Vtbl,SetPipelineState1)/sizeof(void*),S(DrawInstanced),S(DrawIndexedInstanced),
        offsetof(ID3D12CommandQueueVtbl,ExecuteCommandLists)/sizeof(void*)};
#undef S
}
}
