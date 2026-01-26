#pragma once

// Minimal subset of d3dx12.h utilities (expand later as needed).
// The official full header can be dropped in without changing engine code.

#include <d3d12.h>

struct CD3DX12_HEAP_PROPERTIES : public D3D12_HEAP_PROPERTIES
{
    explicit CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE type) noexcept
    {
        Type = type;
        CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        CreationNodeMask = 1;
        VisibleNodeMask = 1;
    }
};

struct CD3DX12_RESOURCE_DESC : public D3D12_RESOURCE_DESC
{
    static CD3DX12_RESOURCE_DESC Buffer(UINT64 bytes) noexcept
    {
        CD3DX12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Alignment = 0;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        d.SampleDesc.Quality = 0;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        return d;
    }
};
