// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <cstdint>

// Our C ABI only; no proprietary types cross this DLL boundary.
struct LabFeatureBoundary {
    std::uint64_t call=0, parent=0, command=0, qpc=0;
    std::uint64_t bindings[4]{}; // Color, Depth, MVec, Output; raw addresses only.
    unsigned kind=0, module=0, result=0; // kind: 1 begin, 2 end; module: 0 Core, 1 NR.
};
struct LabFeatureHost {
    unsigned abi_version = 3;
    unsigned struct_bytes = sizeof(LabFeatureHost);
    void* budget_context = nullptr;
    BOOL (__cdecl* reserve_log_bytes)(void*, std::uint64_t) = nullptr;
    void* boundary_context=nullptr;
    void (__cdecl* boundary)(void*, const LabFeatureBoundary*) noexcept=nullptr;
    // Called synchronously only while NGX Get supplied a valid D3D12 resource
    // and its description query succeeded. IDs belong to command-events.
    void* object_context=nullptr;
    std::uint64_t (__cdecl* observe_resource)(void*, void*, std::uint64_t*) noexcept=nullptr;
};
using LabFeatureStartFn = BOOL (__cdecl*)(const wchar_t*, const char*, BOOL, const LabFeatureHost*);
using LabFeaturePollFn = void (__cdecl*)();
using LabFeatureStopFn = void (__cdecl*)();
using LabFeatureSnapshotFn = unsigned (__cdecl*)(char*, unsigned);
