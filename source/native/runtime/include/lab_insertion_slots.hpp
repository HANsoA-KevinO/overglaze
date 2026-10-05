// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
namespace lab::insertions {
enum Slot {Pipeline,Heaps,Root,Table,Cbv,Srv,Uav,Constant,Constants,GraphicsRoot,GraphicsTable,Clear,Bundle,Indirect,Predicate,StateObject,BeginPass,EndPass,
    GraphicsCbv,GraphicsSrv,GraphicsUav,GraphicsConstant,GraphicsConstants,Topology,Viewports,Scissors,Blend,Stencil,Index,Vertex,StreamOutput,Targets,
    DepthBounds,SamplePositions,ViewMask,ShadingRate,ShadingImage,SplitStencil,DepthBias,StripCut,ProtectedSession,InitMeta,ExecuteMeta,Program,Count};
std::array<std::size_t,Count> slots();
std::size_t create_command_list_slot();
}
