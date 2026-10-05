// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
namespace lab::resume {
enum Slot { Reset,Close,Pipeline,Heaps,Root,Table,Cbv,Srv,Uav,Constant,Constants,
    ClearState,GraphicsRoot,Bundle,Indirect,Predication,StateObject,Draw,DrawIndexed,Execute,Count };
std::array<std::size_t,Count> slots();
}
