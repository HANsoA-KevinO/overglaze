// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
// Two-plane depth/stencil -> single-plane R32_FLOAT, on WARP.
//
// Aimed squarely at a display-driver crash seen when a R32G8X24_TYPELESS
// resource was handed to NR as if it were single-plane. Here the codec must read ONLY the
// depth plane through the plane-0 view, copy it unchanged per pixel, keep the
// stencil plane out, and refuse every mode/format mix-up. No NR, no game.
#include "lab_depth_codec.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
int checks = 0;
void need(bool ok, const std::string& why) { ++checks; if (!ok) throw std::runtime_error(why); }
void hr(HRESULT r, const char* what) { if (FAILED(r)) throw std::runtime_error(std::string(what) + " failed"); }
template <class F> void refused(F f, const char* what) {
    bool threw = false;
    try { f(); } catch (const std::exception&) { threw = true; }
    need(threw, what);
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition = {r, sub, a, b}; return x;
}
}

int main() try {
    ComPtr<IDXGIFactory4> factory; hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter> warp; hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "warp");
    ComPtr<ID3D12Device> device; hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC qd{}; ComPtr<ID3D12CommandQueue> queue; hr(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12CommandAllocator> allocator; hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    ComPtr<ID3D12GraphicsCommandList> list; hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    ComPtr<ID3D12Fence> fence; hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");

    const unsigned w = 64, h = 36;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto texture = [&](DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h;
        d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = format; d.SampleDesc.Count = 1; d.Flags = flags;
        ComPtr<ID3D12Resource> r; hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, clear, IID_PPV_ARGS(&r)), "texture");
        return r;
    };

    // RE Engine's spelling: typeless 32-bit depth + 8-bit stencil, two planes.
    D3D12_CLEAR_VALUE clear{}; clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; clear.DepthStencil = {0.375f, 0x5A};
    auto depth = texture(DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
    D3D12_DESCRIPTOR_HEAP_DESC dh{}; dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dh.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> dsv_heap; hr(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&dsv_heap)), "dsv heap");
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{}; dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    const auto dsv_handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(depth.Get(), &dsv, dsv_handle);

    lab::nr::DepthCodec codec(device.Get(), w, h, nullptr, lab::nr::DepthCodecMode::depth_stencil_plane0);
    need(codec.mode() == lab::nr::DepthCodecMode::depth_stencil_plane0, "codec reports its mode");

    // Two depth values per pixel column half, one stencil value everywhere: a
    // uniform result could not tell a real per-pixel copy from a constant.
    list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.375f, 0x5A, 0, nullptr);
    const D3D12_RECT left{0, 0, LONG(w / 2), LONG(h)};
    list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 0.8125f, 0, 1, &left);
    // The runtime's guide_transitions touches subresource 0 only: the depth plane.
    auto b = transition(depth.Get(), 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &b);
    codec.record(list.Get(), depth.Get(), lab::nr::DepthProjection{});

    // Read the codec's private output back.
    D3D12_RESOURCE_DESC out_desc = codec.output()->GetDesc();
    need(out_desc.Format == DXGI_FORMAT_R32_FLOAT && out_desc.Width == w && out_desc.Height == h, "output is single-plane R32_FLOAT at the guide extent");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0;
    device->GetCopyableFootprints(&out_desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES rb{}; rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback; hr(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback");
    b = transition(codec.output(), 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE); list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{codec.output(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = transition(codec.output(), 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); list->ResourceBarrier(1, &b);
    b = transition(depth.Get(), 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE); list->ResourceBarrier(1, &b);
    hr(list->Close(), "close");
    ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    hr(queue->Signal(fence.Get(), 1), "signal");
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr(fence->SetEventOnCompletion(1, event), "event"); WaitForSingleObject(event, 30000); CloseHandle(event);
    hr(device->GetDeviceRemovedReason(), "WARP device removed by the conversion");
    codec.acknowledge_completion();

    float* mapped = nullptr; hr(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map");
    unsigned wrong = 0;
    for (unsigned y = 0; y < h; ++y) {
        const float* row = reinterpret_cast<const float*>(reinterpret_cast<const char*>(mapped) + fp.Offset + y * fp.Footprint.RowPitch);
        for (unsigned x = 0; x < w; ++x) if (row[x] != (x < w / 2 ? 0.8125f : 0.375f)) ++wrong;
    }
    readback->Unmap(0, nullptr);
    need(wrong == 0, std::to_string(wrong) + " pixels differ from the depth plane (stencil 0x5A must not appear)");

    // ---- a mip-chained hardware depth (A Plague Tale: Resonance):
    // level 0 is copied; level 1 carries a different value that must not appear.
    {
        hr(allocator->Reset(), "mip reset"); hr(list->Reset(allocator.Get(), nullptr), "mip list reset");
        D3D12_RESOURCE_DESC md{}; md.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; md.Width = w; md.Height = h;
        md.DepthOrArraySize = 1; md.MipLevels = 3; md.Format = DXGI_FORMAT_R32_TYPELESS; md.SampleDesc.Count = 1;
        md.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE mc{}; mc.Format = DXGI_FORMAT_D32_FLOAT; mc.DepthStencil = {0.25f, 0};
        ComPtr<ID3D12Resource> chained; hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &md, D3D12_RESOURCE_STATE_DEPTH_WRITE, &mc, IID_PPV_ARGS(&chained)), "chained depth");
        D3D12_DESCRIPTOR_HEAP_DESC mh{}; mh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; mh.NumDescriptors = 2;
        ComPtr<ID3D12DescriptorHeap> mip_dsv; hr(device->CreateDescriptorHeap(&mh, IID_PPV_ARGS(&mip_dsv)), "mip dsv heap");
        const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        auto level0 = mip_dsv->GetCPUDescriptorHandleForHeapStart(); auto level1 = level0; level1.ptr += step;
        D3D12_DEPTH_STENCIL_VIEW_DESC v{}; v.Format = DXGI_FORMAT_D32_FLOAT; v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipSlice = 0; device->CreateDepthStencilView(chained.Get(), &v, level0);
        v.Texture2D.MipSlice = 1; device->CreateDepthStencilView(chained.Get(), &v, level1);
        list->ClearDepthStencilView(level0, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
        list->ClearDepthStencilView(level0, D3D12_CLEAR_FLAG_DEPTH, 0.625f, 0, 1, &left);
        list->ClearDepthStencilView(level1, D3D12_CLEAR_FLAG_DEPTH, 0.9375f, 0, 0, nullptr);
        lab::nr::DepthCodec mip0(device.Get(), w, h, nullptr, lab::nr::DepthCodecMode::hardware_mip0_copy);
        need(mip0.mode() == lab::nr::DepthCodecMode::hardware_mip0_copy, "the mip-0 copy reports its mode");
        // Only subresource 0 leaves DEPTH_WRITE, exactly as guide_transitions does.
        auto t0 = transition(chained.Get(), 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); list->ResourceBarrier(1, &t0);
        mip0.record(list.Get(), chained.Get(), lab::nr::DepthProjection{});
        auto t1 = transition(mip0.output(), 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE); list->ResourceBarrier(1, &t1);
        D3D12_TEXTURE_COPY_LOCATION msrc{mip0.output(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; msrc.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &msrc, nullptr);
        std::swap(t1.Transition.StateBefore, t1.Transition.StateAfter); list->ResourceBarrier(1, &t1);
        std::swap(t0.Transition.StateBefore, t0.Transition.StateAfter); list->ResourceBarrier(1, &t0);
        hr(list->Close(), "mip close");
        ID3D12CommandList* mip_lists[]{list.Get()}; queue->ExecuteCommandLists(1, mip_lists);
        hr(queue->Signal(fence.Get(), 2), "mip signal");
        const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr(fence->SetEventOnCompletion(2, done), "mip event"); WaitForSingleObject(done, 30000); CloseHandle(done);
        hr(device->GetDeviceRemovedReason(), "WARP device removed by the mip-0 copy");
        mip0.acknowledge_completion();
        float* level = nullptr; hr(readback->Map(0, nullptr, reinterpret_cast<void**>(&level)), "mip map");
        unsigned off = 0;
        for (unsigned y = 0; y < h; ++y) {
            const float* row = reinterpret_cast<const float*>(reinterpret_cast<const char*>(level) + fp.Offset + y * fp.Footprint.RowPitch);
            for (unsigned x = 0; x < w; ++x) if (row[x] != (x < w / 2 ? 0.625f : 0.25f)) ++off;
        }
        readback->Unmap(0, nullptr);
        need(off == 0, std::to_string(off) + " pixels differ from mip 0 (level 1's 0.9375 must not appear)");
        refused([&] { mip0.validate(list.Get(), depth.Get(), lab::nr::DepthProjection{}); }, "the mip-0 copy refuses a two-plane source");
    }

    // ---- mix-ups are refused, not read through a wrong view
    lab::nr::DepthCodec projection(device.Get(), w, h, nullptr, lab::nr::DepthCodecMode::view_z_to_hardware);
    lab::nr::DepthProjection p{}; p.a = 1.f; p.b = 0.f; p.c = 1.f; p.d = 0.f; p.near_plane = 0.1f; p.far_plane = 1000.f;
    hr(allocator->Reset(), "reset"); hr(list->Reset(allocator.Get(), nullptr), "list reset");
    refused([&] { projection.validate(list.Get(), depth.Get(), p); }, "the projection mode refuses a two-plane source");
    auto single = texture(DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
    refused([&] { codec.validate(list.Get(), single.Get(), lab::nr::DepthProjection{}); }, "the plane copy refuses a single-plane source");
    auto hidden = texture(DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
    refused([&] { codec.validate(list.Get(), hidden.Get(), lab::nr::DepthProjection{}); }, "a depth/stencil resource that denies shader reads is refused");
    hr(list->Close(), "close 2");

    std::cout << "PASS " << checks << " depth/stencil plane-0 conversion checks on WARP; stencil kept out, NR never sees two planes\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL after " << checks << " checks: " << e.what() << "\n";
    return 1;
}
