// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#define CINTERFACE
#include <d3d12.h>
#include "lab_insertion_slots.hpp"
namespace lab::insertions {
std::size_t create_command_list_slot(){return offsetof(ID3D12DeviceVtbl,CreateCommandList)/sizeof(void*);}
std::array<std::size_t,Count> slots(){
#define S(x) offsetof(ID3D12GraphicsCommandList10Vtbl,x)/sizeof(void*)
    return {S(SetPipelineState),S(SetDescriptorHeaps),S(SetComputeRootSignature),S(SetComputeRootDescriptorTable),
        S(SetComputeRootConstantBufferView),S(SetComputeRootShaderResourceView),S(SetComputeRootUnorderedAccessView),
        S(SetComputeRoot32BitConstant),S(SetComputeRoot32BitConstants),S(SetGraphicsRootSignature),S(SetGraphicsRootDescriptorTable),
        S(ClearState),S(ExecuteBundle),S(ExecuteIndirect),S(SetPredication),S(SetPipelineState1),S(BeginRenderPass),S(EndRenderPass),
        S(SetGraphicsRootConstantBufferView),S(SetGraphicsRootShaderResourceView),S(SetGraphicsRootUnorderedAccessView),S(SetGraphicsRoot32BitConstant),S(SetGraphicsRoot32BitConstants),
        S(IASetPrimitiveTopology),S(RSSetViewports),S(RSSetScissorRects),S(OMSetBlendFactor),S(OMSetStencilRef),S(IASetIndexBuffer),S(IASetVertexBuffers),S(SOSetTargets),S(OMSetRenderTargets),
        S(OMSetDepthBounds),S(SetSamplePositions),S(SetViewInstanceMask),S(RSSetShadingRate),S(RSSetShadingRateImage),S(OMSetFrontAndBackStencilRef),S(RSSetDepthBias),S(IASetIndexBufferStripCutValue),
        S(SetProtectedResourceSession),S(InitializeMetaCommand),S(ExecuteMetaCommand),S(SetProgram)};
#undef S
}
}
