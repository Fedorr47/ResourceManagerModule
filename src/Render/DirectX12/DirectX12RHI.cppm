module;

#if defined(_WIN32)
// Prevent Windows headers from defining the `min`/`max` macros which break `std::min`/`std::max`.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "d3dx12.h"

// DXC (Shader Model 6.x) - optional at compile time, loaded at runtime.
#if __has_include(<dxcapi.h>)
#include <dxcapi.h>
#define CORE_DX12_HAS_DXC 1
#else
#define CORE_DX12_HAS_DXC 0
#endif

// Dear ImGui (DX12 backend)
#include <imgui.h>
#include <imgui_impl_dx12.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include <array>
#include <stdexcept>
#include <algorithm>
#include <cassert>
#include <bit>

export module core:rhi_dx12;

import :rhi;
import :dx12_core;

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
#endif

// AlignUp(v, a) rounds `v` up to the next multiple of `a` (or keeps it unchanged if it's already aligned).
// Assumes `a` is a power of two (1, 2, 4, 8, ...). Commonly used for buffer/heap alignment.
//
// Example:
//   AlignUp(13, 8) == 16
//   AlignUp(16, 8) == 16
std::uint32_t AlignUp(std::uint32_t v, std::uint32_t a)
{
    assert(a != 0 && std::has_single_bit(a));
    return (v + (a - 1)) & ~(a - 1);
}

DXGI_FORMAT ToDXGIFormat(rhi::Format format)
{
    switch (format)
    {
    case rhi::Format::RGBA8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case rhi::Format::RGBA16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case rhi::Format::BGRA8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case rhi::Format::R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case rhi::Format::D32_FLOAT:
        return DXGI_FORMAT_D32_FLOAT;
    case rhi::Format::D24_UNORM_S8_UINT:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT ToDXGIVertexFormat(rhi::VertexFormat format)
{
    switch (format)
    {
    case rhi::VertexFormat::R32G32B32_FLOAT:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case rhi::VertexFormat::R32G32_FLOAT:
        return DXGI_FORMAT_R32G32_FLOAT;
    case rhi::VertexFormat::R32G32B32A32_FLOAT:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case rhi::VertexFormat::R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case rhi::VertexFormat::R16G16B16A16_UINT:
        return DXGI_FORMAT_R16G16B16A16_UINT;
    case rhi::VertexFormat::R16G16B16A16_UNORM:
        return DXGI_FORMAT_R16G16B16A16_UNORM;
    case rhi::VertexFormat::R32G32B32A32_UINT:
        return DXGI_FORMAT_R32G32B32A32_UINT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

D3D12_COMPARISON_FUNC ToD3DCompare(rhi::CompareOp compareOp)
{
    switch (compareOp)
    {
    case rhi::CompareOp::Never:
        return D3D12_COMPARISON_FUNC_NEVER;
    case rhi::CompareOp::Less:
        return D3D12_COMPARISON_FUNC_LESS;
    case rhi::CompareOp::Equal:
        return D3D12_COMPARISON_FUNC_EQUAL;
    case rhi::CompareOp::LessEqual:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case rhi::CompareOp::Greater:
        return D3D12_COMPARISON_FUNC_GREATER;
    case rhi::CompareOp::NotEqual:
        return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case rhi::CompareOp::GreaterEqual:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case rhi::CompareOp::Always:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    default:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    }
}

D3D12_STENCIL_OP ToD3DStencilOp(rhi::StencilOp op)
{
    switch (op)
    {
    case rhi::StencilOp::Keep:
        return D3D12_STENCIL_OP_KEEP;
    case rhi::StencilOp::Zero:
        return D3D12_STENCIL_OP_ZERO;
    case rhi::StencilOp::Replace:
        return D3D12_STENCIL_OP_REPLACE;
    case rhi::StencilOp::IncrementClamp:
        return D3D12_STENCIL_OP_INCR_SAT;
    case rhi::StencilOp::DecrementClamp:
        return D3D12_STENCIL_OP_DECR_SAT;
    case rhi::StencilOp::Invert:
        return D3D12_STENCIL_OP_INVERT;
    case rhi::StencilOp::IncrementWrap:
        return D3D12_STENCIL_OP_INCR;
    case rhi::StencilOp::DecrementWrap:
        return D3D12_STENCIL_OP_DECR;
    default:
        return D3D12_STENCIL_OP_KEEP;
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3DTopologyType(rhi::PrimitiveTopologyType topologyType)
{
    switch (topologyType)
    {
    case rhi::PrimitiveTopologyType::Line:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case rhi::PrimitiveTopologyType::Triangle:
    default:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

D3D_PRIMITIVE_TOPOLOGY ToD3DTopology(rhi::PrimitiveTopology topology)
{
    switch (topology)
    {
    case rhi::PrimitiveTopology::LineList:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case rhi::PrimitiveTopology::TriangleList:
    default:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

D3D12_CULL_MODE ToD3DCull(rhi::CullMode cullMode)
{
    switch (cullMode)
    {
    case rhi::CullMode::None:
        return D3D12_CULL_MODE_NONE;
    case rhi::CullMode::Front:
        return D3D12_CULL_MODE_FRONT;
    case rhi::CullMode::Back:
        return D3D12_CULL_MODE_BACK;
    default:
        return D3D12_CULL_MODE_BACK;
    }
}

bool IsDepthFormat(rhi::Format format)
{
    return format == rhi::Format::D32_FLOAT || format == rhi::Format::D24_UNORM_S8_UINT;
}

const char* SemanticName(rhi::VertexSemantic semantic)
{
    switch (semantic)
    {
    case rhi::VertexSemantic::Position:
        return "POSITION";
    case rhi::VertexSemantic::Normal:
        return "NORMAL";
    case rhi::VertexSemantic::TexCoord:
        return "TEXCOORD";
    case rhi::VertexSemantic::Color:
        return "COLOR";
    case rhi::VertexSemantic::Tangent:
        return "TANGENT";
    case rhi::VertexSemantic::BoneIndices:
        return "BLENDINDICES";
    case rhi::VertexSemantic::BoneWeights:
        return "BLENDWEIGHT";
    default:
        return "TEXCOORD";
    }
}

std::uint32_t IndexSizeBytes(rhi::IndexType indexType)
{
    return (indexType == rhi::IndexType::UINT16) ? 2u : 4u;
}

export namespace rhi
{
    class DX12Device;

    #include "RHIImpl/DirectX12RHI_SwapChain.inl"

    struct PendingBufferUpdate
    {
        BufferHandle buffer{};
        std::size_t dstOffsetBytes{ 0 };
        std::vector<std::byte> data;
    };

#if defined(_WIN32)
    #include "RHIImpl/DirectX12RHI_Device.inl"
    #include "RHIImpl/DirectX12RHI_SwapChainImpl.inl"
#else
    inline std::unique_ptr<IRHIDevice> CreateDX12Device() { return CreateNullDevice(); }
#endif
} // namespace rhi
