#pragma once

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

namespace d3d12_common {
using Microsoft::WRL::ComPtr;

inline D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

inline D3D12_HEAP_PROPERTIES write_back_heap_properties() {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = D3D12_HEAP_TYPE_CUSTOM;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

inline D3D12_RESOURCE_DESC buffer_description(UINT64 bytes,
                                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

inline bool supports_uma_write_back(ID3D12Device* device) {
    D3D12_FEATURE_DATA_ARCHITECTURE a{};
    a.NodeIndex = 0;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &a, sizeof(a))) || !a.UMA)
        return false;
    ComPtr<ID3D12Resource> probe;
    auto heap = write_back_heap_properties();
    auto desc = buffer_description(4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&probe))))
        return false;
    void* ptr = nullptr;
    D3D12_RANGE range{0, 0};
    if (FAILED(probe->Map(0, &range, &ptr)) || !ptr)
        return false;
    probe->Unmap(0, nullptr);
    return true;
}

inline bool create_write_back_buffer(ID3D12Device* device, UINT64 bytes,
                                     ComPtr<ID3D12Resource>& resource, void*& mapped) {
    auto heap = write_back_heap_properties();
    auto desc = buffer_description(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource))))
        return false;
    D3D12_RANGE range{0, 0};
    return SUCCEEDED(resource->Map(0, &range, &mapped)) && mapped;
}
}
