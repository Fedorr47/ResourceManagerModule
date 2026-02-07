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

    struct DX12SwapChainDesc
    {
        SwapChainDesc base{};
        HWND hwnd{ nullptr };
        std::uint32_t bufferCount{ 2 };
    };

    struct PendingBufferUpdate
    {
        BufferHandle buffer{};
        std::size_t dstOffsetBytes{ 0 };
        std::vector<std::byte> data;
    };

    class DX12SwapChain final : public IRHISwapChain
    {
    public:
        DX12SwapChain(DX12Device& device, DX12SwapChainDesc desc);
        ~DX12SwapChain() override = default;

        SwapChainDesc GetDesc() const override;
        FrameBufferHandle GetCurrentBackBuffer() const override;
        void Present() override;

        std::uint32_t FrameIndex() const noexcept { return static_cast<std::uint32_t>(currBackBuffer_); }

        ID3D12Resource* CurrentBackBuffer() const { return backBuffers_[currBackBuffer_].Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTV() const
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(currBackBuffer_) * rtvInc_;
            return handle;
        }

        ID3D12Resource* DepthBuffer() const { return depth_.Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE DSV() const { return dsv_; }

        DXGI_FORMAT BackBufferFormat() const { return bbFormat_; }
        DXGI_FORMAT DepthFormat() const { return depthFormat_; }

        D3D12_RESOURCE_STATES& CurrentBackBufferState()
        {
            return backBufferStates_[currBackBuffer_];
        }

        const D3D12_RESOURCE_STATES& CurrentBackBufferState() const
        {
            return backBufferStates_[currBackBuffer_];
        }

        void ResetBackBufferStates(D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT)
        {
            for (auto& st : backBufferStates_)
            {
                st = state;
            }
        }


        void EnsureSizeUpToDate();

    private:
        DX12Device& device_;
        DX12SwapChainDesc chainSwapDesc_;

        ComPtr<IDXGISwapChain4> swapChain_;
        ComPtr<ID3D12DescriptorHeap> rtvHeap_;
        UINT rtvInc_{ 0 };

        std::vector<ComPtr<ID3D12Resource>> backBuffers_;
        UINT currBackBuffer_{ 0 };
        DXGI_FORMAT bbFormat_{ DXGI_FORMAT_B8G8R8A8_UNORM };

        ComPtr<ID3D12Resource> depth_;
        ComPtr<ID3D12DescriptorHeap> dsvHeap_;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_{};
        DXGI_FORMAT depthFormat_{ DXGI_FORMAT_D32_FLOAT };

        std::vector<D3D12_RESOURCE_STATES> backBufferStates_;
    };

#if defined(_WIN32)
    class DX12Device final : public IRHIDevice
    {
    public:

        DX12Device()
        {
            core_.Init();

            // -----------------------------------------------------------------
            // Frame resources (allocator + small persistent CB upload buffer)
            // -----------------------------------------------------------------
            for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
            {
                ThrowIfFailed(NativeDevice()->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&frames_[i].cmdAlloc)),
                    "DX12: CreateCommandAllocator failed");

                // Per-frame constant upload buffer (persistently mapped).
                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

                D3D12_RESOURCE_DESC resourceDesc{};
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                resourceDesc.Width = static_cast<UINT64>(kPerFrameCBUploadBytes);
                resourceDesc.Height = 1;
                resourceDesc.DepthOrArraySize = 1;
                resourceDesc.MipLevels = 1;
                resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
                resourceDesc.SampleDesc.Count = 1;
                resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &resourceDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&frames_[i].cbUpload)),
                    "DX12: Create per-frame constant upload buffer failed");

                void* mapped = nullptr;
                ThrowIfFailed(frames_[i].cbUpload->Map(0, nullptr, &mapped),
                    "DX12: Map per-frame constant upload buffer failed");

                // Per-frame buffer upload ring (persistently mapped).
                D3D12_RESOURCE_DESC bufDesc = resourceDesc;
                bufDesc.Width = static_cast<UINT64>(kPerFrameBufUploadBytes);

                ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &bufDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&frames_[i].bufUpload)),
                    "DX12: Create per-frame buffer upload ring failed");

                void* bufMapped = nullptr;
                ThrowIfFailed(frames_[i].bufUpload->Map(0, nullptr, &bufMapped),
                    "DX12: Map per-frame buffer upload ring failed");

                frames_[i].bufMapped = reinterpret_cast<std::byte*>(bufMapped);
                frames_[i].bufCursor = 0;

                frames_[i].cbMapped = reinterpret_cast<std::byte*>(mapped);
                frames_[i].cbCursor = 0;
                frames_[i].fenceValue = 0;
            }

            // Command list (created once, reset per frame).
            ThrowIfFailed(NativeDevice()->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                frames_[0].cmdAlloc.Get(),
                nullptr,
                IID_PPV_ARGS(&cmdList_)),
                "DX12: CreateCommandList failed");
            cmdList_->Close();

            // Fence
            ThrowIfFailed(NativeDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
                "DX12: CreateFence failed");
            fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!fenceEvent_)
            {
                throw std::runtime_error("DX12: CreateEvent failed");
            }

            // SRV heap (shader visible)
            {
                D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
                heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                heapDesc.NumDescriptors = 4096;
                heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                ThrowIfFailed(NativeDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap_)),
                    "DX12: Create SRV heap failed");

                srvInc_ = NativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

                // null SRVs:
                //  slot 0: null Texture2D SRV (for t0/t1 texture slots)
                //  slot 1: null StructuredBuffer SRV (for t2 lights SB)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();

                    D3D12_SHADER_RESOURCE_VIEW_DESC nullTex{};
                    nullTex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    nullTex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    nullTex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    nullTex.Texture2D.MipLevels = 1;
                    NativeDevice()->CreateShaderResourceView(nullptr, &nullTex, cpu);

                    cpu.ptr += static_cast<SIZE_T>(srvInc_);

                    D3D12_SHADER_RESOURCE_VIEW_DESC nullBuf{};
                    nullBuf.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                    nullBuf.Format = DXGI_FORMAT_UNKNOWN;
                    nullBuf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    nullBuf.Buffer.FirstElement = 0;
                    nullBuf.Buffer.NumElements = 1;
                    nullBuf.Buffer.StructureByteStride = 16;
                    nullBuf.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
                    NativeDevice()->CreateShaderResourceView(nullptr, &nullBuf, cpu);
                }

                nextSrvIndex_ = 3; // 0=null tex, 1=null buffer, 2=ImGui font SRV
                freeSrv_.clear();
            }

            CreateRootSignature();
        }

        ~DX12Device() override
        {
            // Make sure GPU is idle before we release resources referenced by the queue.
            try
            {
                if (fence_ && core_.cmdQueue)
                {
                    FlushGPU();
                }
            }
            catch (...)
            {
                // Avoid exceptions from destructors.
            }

            for (auto& fr : frames_)
            {
                if (fr.cbUpload)
                {
                    fr.cbUpload->Unmap(0, nullptr);
                    fr.cbMapped = nullptr;
                }

                fr.deferredResources.clear();
                fr.deferredFreeSrv.clear();
                fr.deferredFreeRtv.clear();
                fr.deferredFreeDsv.clear();
            }

            if (fenceEvent_)
            {
                CloseHandle(fenceEvent_);
                fenceEvent_ = nullptr;
            }
        }

        void ReplaceSampledTextureResource(rhi::TextureHandle textureHandle, ID3D12Resource* newRes, DXGI_FORMAT fmt, UINT mipLevels)
        {
            auto it = textures_.find(textureHandle.id);
            if (it == textures_.end())
            {
                throw std::runtime_error("DX12: ReplaceSampledTextureResource: texture handle not found");
            }

            it->second.resource.Reset();
            it->second.resource.Attach(newRes); // takes ownership (AddRef already implied by Attach contract)
            it->second.hasSRV = false;

            AllocateSRV(it->second, fmt, mipLevels);
        } /// DX12Device

        TextureHandle RegisterSampledTexture(ID3D12Resource* res, DXGI_FORMAT fmt, UINT mipLevels)
        {
            if (!res)
            {
                return {};
            }

            TextureHandle textureHandle{ ++nextTexId_ };
            TextureEntry textureEntry{};

            // Fill extent from resource desc
            const D3D12_RESOURCE_DESC resourceDesc = res->GetDesc();
            textureEntry.extent = Extent2D{
                static_cast<std::uint32_t>(resourceDesc.Width),
                static_cast<std::uint32_t>(resourceDesc.Height) };
            textureEntry.format = rhi::Format::RGBA8_UNORM; // internal book-keeping only (engine side)

            // Take ownership (AddRef)
            textureEntry.resource = res;

            // Allocate SRV in our shader-visible heap
            AllocateSRV(textureEntry, fmt, mipLevels);

            textures_[textureHandle.id] = std::move(textureEntry);
            return textureHandle;
        }

        TextureHandle RegisterSampledTextureCube(ID3D12Resource* res, DXGI_FORMAT fmt, UINT mipLevels)
        {
            if (!res)
            {
                return {};
            }

            TextureHandle textureHandle{ ++nextTexId_ };
            TextureEntry textureEntry{};

            const D3D12_RESOURCE_DESC resourceDesc = res->GetDesc();
            textureEntry.extent = Extent2D{
                static_cast<std::uint32_t>(resourceDesc.Width),
                static_cast<std::uint32_t>(resourceDesc.Height) };
            textureEntry.format = rhi::Format::RGBA8_UNORM;
            textureEntry.type = TextureEntry::Type::Cube;

            // Take ownership (AddRef)
            textureEntry.resource = res;

            AllocateSRV(textureEntry, fmt, mipLevels);

            textures_[textureHandle.id] = std::move(textureEntry);
            return textureHandle;
        }

        void SetSwapChain(DX12SwapChain* swapChain)
        {
            swapChain_ = swapChain;
        }

        std::string_view GetName() const override
        {
            return "DirectX12 RHI";
        }

        // ---- Dear ImGui hooks ----
        void InitImGui(void* hwnd, int framesInFlight, rhi::Format rtvFormat) override
        {
            if (imguiInitialized_)
            {
                return;
            }
            if (!hwnd)
            {
                throw std::runtime_error("DX12: InitImGui: hwnd is null");
            }

            const DXGI_FORMAT fmt = ToDXGIFormat(rtvFormat);

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(kImGuiFontSrvIndex) * static_cast<SIZE_T>(srvInc_);

            D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(kImGuiFontSrvIndex) * static_cast<UINT64>(srvInc_);

            // The ImGui Win32 backend is initialized in the app; here we only setup the DX12 backend.
            if (!ImGui_ImplDX12_Init(NativeDevice(), framesInFlight, fmt, srvHeap_.Get(), cpu, gpu))
            {
                throw std::runtime_error("DX12: ImGui_ImplDX12_Init failed");
            }

            imguiInitialized_ = true;
        }

        void ImGuiNewFrame() override
        {
            if (imguiInitialized_)
            {
                ImGui_ImplDX12_NewFrame();
            }
        }

        void ShutdownImGui() override
        {
            if (imguiInitialized_)
            {
                ImGui_ImplDX12_Shutdown();
                imguiInitialized_ = false;
            }
        }
        Backend GetBackend() const noexcept override
        {
            return Backend::DirectX12;
        }

        // ---------------- Textures (RenderGraph transient) ---------------- //
        TextureHandle CreateTexture2D(Extent2D extent, Format format) override
        {
            TextureHandle textureHandle{ ++nextTexId_ };
            TextureEntry textureEntry{};
            textureEntry.extent = extent;
            textureEntry.format = format;

            const DXGI_FORMAT dxFmt = ToDXGIFormat(format);

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resourceDesc.Width = extent.width;
            resourceDesc.Height = extent.height;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = dxFmt;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            D3D12_CLEAR_VALUE clearValue{};
            clearValue.Format = dxFmt;

            D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON;

            if (IsDepthFormat(format))
            {
                DXGI_FORMAT dsvFmt = ToDXGIFormat(format);
                DXGI_FORMAT resFmt = DXGI_FORMAT_UNKNOWN;
                DXGI_FORMAT srvFmt = DXGI_FORMAT_UNKNOWN;

                if (format == Format::D32_FLOAT)
                {
                    resFmt = DXGI_FORMAT_R32_TYPELESS;
                    srvFmt = DXGI_FORMAT_R32_FLOAT;
                }
                else if (format == Format::D24_UNORM_S8_UINT)
                {
                    resFmt = DXGI_FORMAT_R24G8_TYPELESS;
                    srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                }
                else
                {
                    resFmt = dsvFmt; // fallback (no sampling)
                    srvFmt = DXGI_FORMAT_UNKNOWN;
                }

                resourceDesc.Format = resFmt;
                resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

                D3D12_CLEAR_VALUE clearValue{};
                clearValue.Format = dsvFmt;
                clearValue.DepthStencil.Depth = 1.0f;
                clearValue.DepthStencil.Stencil = 0;

                ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &resourceDesc,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    &clearValue,
                    IID_PPV_ARGS(&textureEntry.resource)),
                    "DX12: Create depth texture failed");

                textureEntry.resourceFormat = resFmt;
                textureEntry.dsvFormat = dsvFmt;
                textureEntry.srvFormat = srvFmt;
                textureEntry.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;

                EnsureDSVHeap();
                textureEntry.dsv = AllocateDSV(textureEntry.resource.Get(), dsvFmt, textureEntry.dsvIndex);
                textureEntry.hasDSV = true;

                // SRV for sampling (shadow maps)
                if (srvFmt != DXGI_FORMAT_UNKNOWN)
                {
                    AllocateSRV(textureEntry, srvFmt, 1);
                }
            }
            else
            {
                resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                clearValue.Color[0] = 0.0f;
                clearValue.Color[1] = 0.0f;
                clearValue.Color[2] = 0.0f;
                clearValue.Color[3] = 1.0f;
                initState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &resourceDesc,
                    initState,
                    &clearValue,
                    IID_PPV_ARGS(&textureEntry.resource)),
                    "DX12: Create color texture failed");

                textureEntry.resourceFormat = dxFmt;
                textureEntry.srvFormat = dxFmt;
                textureEntry.rtvFormat = dxFmt;
                textureEntry.state = initState;

                EnsureRTVHeap();
                textureEntry.rtv = AllocateRTV(textureEntry.resource.Get(), dxFmt, textureEntry.rtvIndex);
                textureEntry.hasRTV = true;

                AllocateSRV(textureEntry, dxFmt, 1);
            }

            textures_[textureHandle.id] = std::move(textureEntry);
            return textureHandle;
        }


        TextureHandle CreateTextureCube(Extent2D extent, Format format) override
        {
            // Currently used for point light shadows: R32_FLOAT distance cubemap.
            if (IsDepthFormat(format))
            {
                throw std::runtime_error("DX12: CreateTextureCube: depth formats are not supported for cubemaps");
            }

            TextureHandle textureHandle{ ++nextTexId_ };
            TextureEntry textureEntry{};
            textureEntry.extent = extent;
            textureEntry.format = format;
            textureEntry.type = TextureEntry::Type::Cube;

            const DXGI_FORMAT dxFmt = ToDXGIFormat(format);

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resourceDesc.Width = extent.width;
            resourceDesc.Height = extent.height;
            resourceDesc.DepthOrArraySize = 6; // cubemap faces
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = dxFmt;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clearValue{};
            clearValue.Format = dxFmt;
            clearValue.Color[0] = 1.0f;
            clearValue.Color[1] = 1.0f;
            clearValue.Color[2] = 1.0f;
            clearValue.Color[3] = 1.0f;

            const D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_RENDER_TARGET;

            ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                initState,
                &clearValue,
                IID_PPV_ARGS(&textureEntry.resource)),
                "DX12: Create cubemap texture failed");

            textureEntry.resourceFormat = dxFmt;
            textureEntry.srvFormat = dxFmt;
            textureEntry.rtvFormat = dxFmt;
            textureEntry.state = initState;

            EnsureRTVHeap();
            textureEntry.hasRTVFaces = true;
            for (UINT face = 0; face < 6; ++face)
            {
                textureEntry.rtvFaces[face] = AllocateRTVTexture2DArraySlice(textureEntry.resource.Get(), dxFmt, face, textureEntry.rtvIndexFaces[face]);
            }

            AllocateSRV(textureEntry, dxFmt, 1);

            textures_[textureHandle.id] = std::move(textureEntry);
            return textureHandle;
        }

        void DestroyTexture(TextureHandle texture) noexcept override
        {
            if (texture.id == 0)
            {
                return;
            }

            auto it = textures_.find(texture.id);
            if (it == textures_.end())
            {
                return;
            }

            TextureEntry entry = std::move(it->second);
            textures_.erase(it);

            // Keep the resource alive until GPU finishes the frame that referenced it.
            if (entry.resource)
            {
                CurrentFrame().deferredResources.push_back(std::move(entry.resource));
            }

            // Recycle SRV index after the frame fence is completed (see BeginFrame()).
            if (entry.hasSRV && entry.srvIndex != 0)
            {
                CurrentFrame().deferredFreeSrv.push_back(entry.srvIndex);
            }

            if (entry.hasRTV)
            {
                CurrentFrame().deferredFreeRtv.push_back(entry.rtvIndex);
            }
            if (entry.hasRTVFaces)
            {
                for (UINT idx : entry.rtvIndexFaces)
                {
                    CurrentFrame().deferredFreeRtv.push_back(idx);
                }
            }
            if (entry.hasDSV)
            {
                CurrentFrame().deferredFreeDsv.push_back(entry.dsvIndex);
            }
        }

        // ---------------- Framebuffers ----------------
        FrameBufferHandle CreateFramebuffer(TextureHandle color, TextureHandle depth) override
        {
            FrameBufferHandle frameBuffer{ ++nextFBId_ };
            FramebufferEntry frameBufEntry{};
            frameBufEntry.color = color;
            frameBufEntry.depth = depth;
            framebuffers_[frameBuffer.id] = frameBufEntry;
            return frameBuffer;
        }


        FrameBufferHandle CreateFramebufferCubeFace(TextureHandle colorCube, std::uint32_t faceIndex, TextureHandle depth) override
        {
            FrameBufferHandle frameBuffer{ ++nextFBId_ };
            FramebufferEntry frameBufEntry{};
            frameBufEntry.color = colorCube;
            frameBufEntry.depth = depth;
            frameBufEntry.colorCubeFace = faceIndex;
            framebuffers_[frameBuffer.id] = frameBufEntry;
            return frameBuffer;
        }

        void DestroyFramebuffer(FrameBufferHandle frameBuffer) noexcept override
        {
            if (frameBuffer.id == 0)
            {
                return;
            }
            framebuffers_.erase(frameBuffer.id);
        }

        // ---------------- Buffers ----------------
        BufferHandle CreateBuffer(const BufferDesc& desc) override
        {
            BufferHandle handle{ ++nextBufId_ };
            BufferEntry bufferEntry{};
            bufferEntry.desc = desc;

            const UINT64 sz = static_cast<UINT64>(desc.sizeInBytes);

            // GPU-local buffer (DEFAULT heap). Updates happen via per-frame upload ring.
            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resourceDesc.Width = std::max<UINT64>(1, sz);
            resourceDesc.Height = 1;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            const D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON;

            ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                initState,
                nullptr,
                IID_PPV_ARGS(&bufferEntry.resource)),
                "DX12: CreateBuffer failed");

            bufferEntry.state = initState;

            if (desc.bindFlag == BufferBindFlag::StructuredBuffer)
            {
                AllocateStructuredBufferSRV(bufferEntry);
            }

            buffers_[handle.id] = std::move(bufferEntry);
            return handle;
        }

        void UpdateBuffer(BufferHandle buffer, std::span<const std::byte> data, std::size_t offsetBytes = 0) override
        {
            if (!buffer || data.empty())
            {
                return;
            }

            auto it = buffers_.find(buffer.id);
            if (it == buffers_.end())
            {
                return;
            }

            BufferEntry& entry = it->second;

            const std::size_t end = offsetBytes + data.size();
            if (end > entry.desc.sizeInBytes)
                throw std::runtime_error("DX12: UpdateBuffer out of bounds");

            // If swapchain not set yet → blocking upload.
            if (!swapChain_)
            {
                ImmediateUploadBuffer(entry, data, offsetBytes);
                return;
            }

            PendingBufferUpdate u{};
            u.buffer = buffer;
            u.dstOffsetBytes = offsetBytes;
            u.data.assign(data.begin(), data.end());
            pendingBufferUpdates_.push_back(std::move(u));
        }

        void DestroyBuffer(BufferHandle buffer) noexcept override
        {
            if (buffer.id == 0)
            {
                return;
            }

            auto it = buffers_.find(buffer.id);
            if (it == buffers_.end())
            {
                return;
            }

            BufferEntry entry = std::move(it->second);
            buffers_.erase(it);

            // Remove pending updates for this buffer.
            if (!pendingBufferUpdates_.empty())
            {
                pendingBufferUpdates_.erase(
                    std::remove_if(pendingBufferUpdates_.begin(), pendingBufferUpdates_.end(),
                        [&](const PendingBufferUpdate& u) { return u.buffer.id == buffer.id; }),
                    pendingBufferUpdates_.end());
            }

            if (entry.resource && swapChain_)
            {
                CurrentFrame().deferredResources.push_back(std::move(entry.resource));
            }

            if (entry.hasSRV && entry.srvIndex != 0)
            {
                if (swapChain_)
                {
                    CurrentFrame().deferredFreeSrv.push_back(entry.srvIndex);
                }
                else
                {
                    freeSrv_.push_back(entry.srvIndex);
                }
            }
        }

        // ---------------- Input layouts ----------------
        InputLayoutHandle CreateInputLayout(const InputLayoutDesc& desc) override
        {
            InputLayoutHandle handle{ ++nextLayoutId_ };
            InputLayoutEntry inputLayoutEntry{};
            inputLayoutEntry.strideBytes = desc.strideBytes;

            inputLayoutEntry.semanticStorage.reserve(desc.attributes.size());
            inputLayoutEntry.elems.reserve(desc.attributes.size());

            for (const auto& attribute : desc.attributes)
            {
                inputLayoutEntry.semanticStorage.emplace_back(SemanticName(attribute.semantic));

                const bool instanced = (attribute.inputSlot != 0);

                D3D12_INPUT_ELEMENT_DESC elemDesc{};
                elemDesc.SemanticName = inputLayoutEntry.semanticStorage.back().c_str();
                elemDesc.SemanticIndex = attribute.semanticIndex;
                elemDesc.Format = ToDXGIVertexFormat(attribute.format);
                elemDesc.InputSlot = attribute.inputSlot;
                elemDesc.AlignedByteOffset = attribute.offsetBytes;
                elemDesc.InputSlotClass = instanced
                    ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                    : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                elemDesc.InstanceDataStepRate = instanced ? 1 : 0;

                inputLayoutEntry.elems.push_back(elemDesc);
            }

            layouts_[handle.id] = std::move(inputLayoutEntry);
            return handle;
        }

        void DestroyInputLayout(InputLayoutHandle layout) noexcept override
        {
            layouts_.erase(layout.id);
        }

        // ---------------- Shaders / Pipelines ----------------
        ShaderHandle CreateShader(ShaderStage stage, std::string_view debugName, std::string_view sourceOrBytecode) override
        {
            ShaderHandle handle{ ++nextShaderId_ };
            ShaderEntry shaderEntry{};
            shaderEntry.stage = stage;
            shaderEntry.name = std::string(debugName);

            const char* target = (stage == ShaderStage::Vertex) ? "vs_5_1" : "ps_5_1";

            ComPtr<ID3DBlob> code;
            ComPtr<ID3DBlob> errors;

            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

            auto TryCompile = [&](const char* entry) -> bool
                {
                    code.Reset();
                    errors.Reset();

                    HRESULT hr = D3DCompile(
                        sourceOrBytecode.data(),
                        sourceOrBytecode.size(),
                        shaderEntry.name.c_str(),
                        nullptr, nullptr,
                        entry, target,
                        flags, 0,
                        &code, &errors);

                    return SUCCEEDED(hr);
                };

            if (!TryCompile(shaderEntry.name.c_str()))
            {
                const char* fallback = (stage == ShaderStage::Vertex) ? "VSMain" : "PSMain";
                if (!TryCompile(fallback))
                {
                    std::string err = "DX12: shader compile failed: ";
                    if (errors)
                    {
                        err += std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
                    }
                    throw std::runtime_error(err);
                }
            }

            shaderEntry.blob = code;
            shaders_[handle.id] = std::move(shaderEntry);
            return handle;
        }

        void DestroyShader(ShaderHandle shader) noexcept override
        {
            shaders_.erase(shader.id);
        }

        PipelineHandle CreatePipeline(std::string_view debugName, ShaderHandle vertexShader, ShaderHandle pixelShader) override
        {
            PipelineHandle handle{ ++nextPsoId_ };
            PipelineEntry pipelineEntry{};
            pipelineEntry.debugName = std::string(debugName);
            pipelineEntry.vs = vertexShader;
            pipelineEntry.ps = pixelShader;
            pipelines_[handle.id] = std::move(pipelineEntry);
            return handle;
        }

        void DestroyPipeline(PipelineHandle pso) noexcept override
        {
            pipelines_.erase(pso.id);
            // TODO: PSO cache entries - it can be cleared indpendtly - but right here it is ok
        }

        // ---------------- Submission ----------------
        void SubmitCommandList(CommandList&& commandList) override
        {
            if (!swapChain_)
            {
                throw std::runtime_error("DX12: swapchain is not set on device (CreateDX12SwapChain must set it).");
            }

            // Begin frame: wait/recycle per-frame stuff + reset allocator/list
            BeginFrame();

            // Set descriptor heaps (SRV)
            ID3D12DescriptorHeap* heaps[] = { NativeSRVHeap() };
            cmdList_->SetDescriptorHeaps(1, heaps);

            FlushPendingBufferUpdates();

            // State while parsing high-level commands
            GraphicsState curState{};
            PipelineHandle curPipe{};

            InputLayoutHandle curLayout{};
            static constexpr std::uint32_t kMaxVBSlots = 2;
            std::array<BufferHandle, kMaxVBSlots> vertexBuffers{};
            std::array<std::uint32_t, kMaxVBSlots> vbStrides{};
            std::array<std::uint32_t, kMaxVBSlots> vbOffsets{};

            BufferHandle indexBuffer{};
            IndexType ibType = IndexType::UINT16;
            std::uint32_t ibOffset = 0;

            // Bound textures by slot (we actuallu use only slot 0)
            std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kMaxSRVSlots> boundTex{};
            for (auto& t : boundTex)
            {
                t = srvHeap_->GetGPUDescriptorHandleForHeapStart(); // null SRV slot0
            }

            // slot 2 (t2) expects StructuredBuffer SRV; point it to null-buffer descriptor (SRV heap index 1).
            if (boundTex.size() > 2)
            {
                boundTex[2].ptr += static_cast<UINT64>(srvInc_);
            }

            // Per-draw constants (raw bytes).
            // The renderer is responsible for packing the layout expected by HLSL.
            std::array<std::byte, 256> perDrawBytes{};
            std::uint32_t perDrawSize = 0;
            std::uint32_t perDrawSlot = 0;

            auto WriteCBAndBind = [&]()
                {
                    FrameResource& fr = CurrentFrame();

                    const std::uint32_t used = (perDrawSize == 0) ? 1u : perDrawSize;
                    const std::uint32_t cbSize = AlignUp(used, 256);

                    if (fr.cbCursor + cbSize > kPerFrameCBUploadBytes)
                    {
                        throw std::runtime_error("DX12: per-frame constant upload ring overflow (increase kPerFrameCBUploadBytes)");
                    }

                    if (perDrawSize != 0)
                    {
                        std::memcpy(fr.cbMapped + fr.cbCursor, perDrawBytes.data(), perDrawSize);
                    }

                    const D3D12_GPU_VIRTUAL_ADDRESS gpuVA = fr.cbUpload->GetGPUVirtualAddress() + fr.cbCursor;
                    cmdList_->SetGraphicsRootConstantBufferView(perDrawSlot, gpuVA);

                    fr.cbCursor += cbSize;
                };

            auto ResolveTextureHandleFromDesc = [&](TextureDescIndex idx) -> TextureHandle
                {
                    if (idx == 0) // 0 = null SRV (как в рендерере)
                    {
                        return {};
                    }

                    auto it = descToTex_.find(idx);
                    if (it == descToTex_.end())
                    {
                        throw std::runtime_error("DX12: TextureDescIndex not mapped");
                    }
                    return it->second;
                };

            auto GetTextureSRV = [&](TextureHandle textureHandle) -> D3D12_GPU_DESCRIPTOR_HANDLE
                {
                    if (!textureHandle)
                    {
                        return srvHeap_->GetGPUDescriptorHandleForHeapStart();
                    }
                    auto it = textures_.find(textureHandle.id);
                    if (it == textures_.end())
                    {
                        return srvHeap_->GetGPUDescriptorHandleForHeapStart();
                    }
                    if (!it->second.hasSRV)
                    {
                        return srvHeap_->GetGPUDescriptorHandleForHeapStart();
                    }
                    return it->second.srvGpu;
                };

            auto NullBufferSRV = [&]() -> D3D12_GPU_DESCRIPTOR_HANDLE
                {
                    D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap_->GetGPUDescriptorHandleForHeapStart();
                    h.ptr += static_cast<UINT64>(srvInc_); // SRV heap index 1
                    return h;
                };

            auto GetBufferSRV = [&](BufferHandle bufferHandle) -> D3D12_GPU_DESCRIPTOR_HANDLE
                {
                    if (!bufferHandle)
                    {
                        return NullBufferSRV();
                    }

                    auto it = buffers_.find(bufferHandle.id);
                    if (it == buffers_.end())
                    {
                        return NullBufferSRV();
                    }

                    if (!it->second.hasSRV)
                    {
                        return NullBufferSRV();
                    }

                    return it->second.srvGpu;
                };



            UINT curNumRT = 1;
            std::array<DXGI_FORMAT, 8> curRTVFormats{};
            curRTVFormats[0] = swapChain_->BackBufferFormat();
            DXGI_FORMAT curDSVFormat = swapChain_->DepthFormat();
            bool curPassIsSwapChain = false;

            auto Barrier = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& curState, D3D12_RESOURCE_STATES desired)
                {
                    if (!res)
                    {
                        return;
                    }
                    if (curState == desired)
                    {
                        return;
                    }

                    D3D12_RESOURCE_BARRIER resBarrier{};
                    resBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    resBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    resBarrier.Transition.pResource = res;
                    resBarrier.Transition.StateBefore = curState;
                    resBarrier.Transition.StateAfter = desired;
                    resBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    cmdList_->ResourceBarrier(1, &resBarrier);
                    curState = desired;
                };

            auto TransitionTexture = [&](TextureHandle tex, D3D12_RESOURCE_STATES desired)
                {
                    if (!tex)
                    {
                        return;
                    }
                    auto it = textures_.find(tex.id);
                    if (it == textures_.end())
                    {
                        return;
                    }
                    Barrier(it->second.resource.Get(), it->second.state, desired);
                };

            auto TransitionBackBuffer = [&](D3D12_RESOURCE_STATES desired)
                {
                    Barrier(swapChain_->CurrentBackBuffer(), swapChain_->CurrentBackBufferState(), desired);
                };

            auto EnsurePSO = [&](PipelineHandle pipelineHandle, InputLayoutHandle layout) -> ID3D12PipelineState*
                {
                    auto PackState = [&](const GraphicsState& s) -> std::uint32_t
                        {
                            std::uint32_t v = 0;
                            v |= (static_cast<std::uint32_t>(s.rasterizer.cullMode) & 0x3u) << 0;
                            v |= (static_cast<std::uint32_t>(s.rasterizer.frontFace) & 0x1u) << 2;
                            v |= (s.depth.testEnable ? 1u : 0u) << 3;
                            v |= (s.depth.writeEnable ? 1u : 0u) << 4;
                            v |= (static_cast<std::uint32_t>(s.depth.depthCompareOp) & 0x7u) << 5;
                            v |= (s.blend.enable ? 1u : 0u) << 8;
                            return v;
                        };

                    auto Fnv1a64 = [](std::uint64_t h, std::uint64_t v) -> std::uint64_t
                        {
                            constexpr std::uint64_t kPrime = 1099511628211ull;
                            for (int i = 0; i < 8; ++i)
                            {
                                const std::uint8_t byte = static_cast<std::uint8_t>((v >> (i * 8)) & 0xffu);
                                h ^= byte;
                                h *= kPrime;
                            }
                            return h;
                        };

                    // PSO cache key MUST include: shaders, state, layout, and render-target formats.
                    std::uint64_t key = 1469598103934665603ull; // FNV-1a offset basis
                    key = Fnv1a64(key, static_cast<std::uint64_t>(pipelineHandle.id));
                    key = Fnv1a64(key, static_cast<std::uint64_t>(layout.id));
                    key = Fnv1a64(key, static_cast<std::uint64_t>(PackState(curState)));
                    key = Fnv1a64(key, static_cast<std::uint64_t>(curNumRT));
                    key = Fnv1a64(key, static_cast<std::uint64_t>(curDSVFormat));
                    for (std::size_t i = 0; i < curRTVFormats.size(); ++i)
                    {
                        key = Fnv1a64(key, static_cast<std::uint64_t>(curRTVFormats[i]));
                    }

                    if (auto it = psoCache_.find(key); it != psoCache_.end())
                    {
                        return it->second.Get();
                    }

                    auto pit = pipelines_.find(pipelineHandle.id);
                    if (pit == pipelines_.end())
                    {
                        throw std::runtime_error("DX12: pipeline handle not found");
                    }

                    auto vsIt = shaders_.find(pit->second.vs.id);
                    auto psIt = shaders_.find(pit->second.ps.id);
                    if (vsIt == shaders_.end() || psIt == shaders_.end())
                    {
                        throw std::runtime_error("DX12: shader handle not found");
                    }

                    auto layIt = layouts_.find(layout.id);
                    if (layIt == layouts_.end())
                    {
                        throw std::runtime_error("DX12: input layout handle not found");
                    }

                    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
                    pipelineDesc.pRootSignature = rootSig_.Get();

                    pipelineDesc.VS = { vsIt->second.blob->GetBufferPointer(), vsIt->second.blob->GetBufferSize() };
                    pipelineDesc.PS = { psIt->second.blob->GetBufferPointer(), psIt->second.blob->GetBufferSize() };

                    pipelineDesc.BlendState = CD3D12_BLEND_DESC(D3D12_DEFAULT);
                    pipelineDesc.SampleMask = UINT_MAX;

                    // Blend
                    if (curState.blend.enable)
                    {
                        D3D12_BLEND_DESC blendDesc = CD3D12_BLEND_DESC(D3D12_DEFAULT);
                        blendDesc.AlphaToCoverageEnable = FALSE;
                        blendDesc.IndependentBlendEnable = FALSE;

                        D3D12_RENDER_TARGET_BLEND_DESC renderTartget{};
                        renderTartget.BlendEnable = TRUE;
                        renderTartget.LogicOpEnable = FALSE;
                        renderTartget.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                        renderTartget.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                        renderTartget.BlendOp = D3D12_BLEND_OP_ADD;
                        renderTartget.SrcBlendAlpha = D3D12_BLEND_ONE;
                        renderTartget.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                        renderTartget.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                        renderTartget.LogicOp = D3D12_LOGIC_OP_NOOP;
                        renderTartget.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

                        for (UINT i = 0; i < 8; ++i)
                        {
                            blendDesc.RenderTarget[i] = renderTartget;
                        }

                        pipelineDesc.BlendState = blendDesc;
                    }

                    // Rasterizer from current state
                    pipelineDesc.RasterizerState = CD3D12_RASTERIZER_DESC(D3D12_DEFAULT);
                    pipelineDesc.RasterizerState.CullMode = ToD3DCull(curState.rasterizer.cullMode);
                    pipelineDesc.RasterizerState.FrontCounterClockwise = (curState.rasterizer.frontFace == FrontFace::CounterClockwise) ? TRUE : FALSE;

                    // Depth
                    pipelineDesc.DepthStencilState = CD3D12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
                    pipelineDesc.DepthStencilState.DepthEnable = curState.depth.testEnable ? TRUE : FALSE;
                    pipelineDesc.DepthStencilState.DepthWriteMask = curState.depth.writeEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
                    pipelineDesc.DepthStencilState.DepthFunc = ToD3DCompare(curState.depth.depthCompareOp);

                    pipelineDesc.InputLayout = { layIt->second.elems.data(), static_cast<UINT>(layIt->second.elems.size()) };
                    pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

                    pipelineDesc.NumRenderTargets = curNumRT;
                    for (UINT i = 0; i < curNumRT; ++i)
                    {
                        pipelineDesc.RTVFormats[i] = curRTVFormats[i];
                    }
                    pipelineDesc.DSVFormat = curDSVFormat;

                    pipelineDesc.SampleDesc.Count = 1;

                    ComPtr<ID3D12PipelineState> pso;
                    ThrowIfFailed(NativeDevice()->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pso)),
                        "DX12: CreateGraphicsPipelineState failed");

                    psoCache_[key] = pso;
                    return pso.Get();
                };

            // Parse high-level commands and record native D3D12
            for (auto& command : commandList.commands)
            {
                std::visit([&](auto&& cmd)
                    {
                        using T = std::decay_t<decltype(cmd)>;

                        if constexpr (std::is_same_v<T, CommandBeginPass>)
                        {
                            const BeginPassDesc& pass = cmd.desc;
                            const ClearDesc& c = pass.clearDesc;

                            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> rtvs{};
                            UINT numRT = 0;

                            D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
                            bool hasDSV = false;

                            curNumRT = 0;
                            std::fill(curRTVFormats.begin(), curRTVFormats.end(), DXGI_FORMAT_UNKNOWN);
                            curDSVFormat = DXGI_FORMAT_UNKNOWN;

                            if (pass.frameBuffer.id == 0)
                            {
                                if (!swapChain_)
                                    throw std::runtime_error("DX12: CommandBeginPass: swapChain is null");

                                TransitionBackBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET);

                                rtvs[0] = swapChain_->CurrentRTV();
                                numRT = 1;

                                curNumRT = numRT;
                                curRTVFormats[0] = swapChain_->BackBufferFormat();

                                dsv = swapChain_->DSV();
                                hasDSV = (dsv.ptr != 0);
                                curDSVFormat = swapChain_->DepthFormat();

                                const D3D12_CPU_DESCRIPTOR_HANDLE* rtvPtr = rtvs.data();
                                const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = hasDSV ? &dsv : nullptr;
                                cmdList_->OMSetRenderTargets(numRT, rtvPtr, FALSE, dsvPtr);

                                if (c.clearColor)
                                {
                                    const float* col = c.color.data();
                                    cmdList_->ClearRenderTargetView(rtvs[0], col, 0, nullptr);
                                }
                                if (c.clearDepth && hasDSV)
                                {
                                    cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, c.depth, 0, 0, nullptr);
                                }

                                curPassIsSwapChain = true;
                            }
                            else
                            {
                                // ----- Offscreen framebuffer pass -----
                                auto fbIt = framebuffers_.find(pass.frameBuffer.id);
                                if (fbIt == framebuffers_.end())
                                {
                                    throw std::runtime_error("DX12: CommandBeginPass: framebuffer not found");
                                }

                                const FramebufferEntry& fb = fbIt->second;

                                // Color (0 or 1 RT)
                                if (fb.color)
                                {
                                    auto it = textures_.find(fb.color.id);
                                    if (it == textures_.end())
                                    {
                                        throw std::runtime_error("DX12: CommandBeginPass: framebuffer color texture not found");
                                    }

                                    auto& te = it->second;

                                    if (fb.colorCubeFace != 0xFFFFFFFFu)
                                    {
                                        if (!te.hasRTVFaces)
                                        {
                                            throw std::runtime_error("DX12: CommandBeginPass: cubemap color texture has no RTV faces");
                                        }
                                        if (fb.colorCubeFace >= 6)
                                        {
                                            throw std::runtime_error("DX12: CommandBeginPass: cubemap face index out of range");
                                        }

                                        Barrier(te.resource.Get(), te.state, D3D12_RESOURCE_STATE_RENDER_TARGET);

                                        rtvs[0] = te.rtvFaces[fb.colorCubeFace];
                                        numRT = 1;
                                        curRTVFormats[0] = te.rtvFormat;
                                    }
                                    else
                                    {
                                        if (!te.hasRTV)
                                        {
                                            throw std::runtime_error("DX12: CommandBeginPass: color texture has no RTV");
                                        }

                                        Barrier(te.resource.Get(), te.state, D3D12_RESOURCE_STATE_RENDER_TARGET);

                                        rtvs[0] = te.rtv;
                                        numRT = 1;
                                        curRTVFormats[0] = te.rtvFormat;
                                    }
                                }
                                // Depth
                                if (fb.depth)
                                {
                                    auto it = textures_.find(fb.depth.id);
                                    if (it == textures_.end())
                                    {
                                        throw std::runtime_error("DX12: CommandBeginPass: framebuffer depth texture not found");
                                    }

                                    auto& te = it->second;
                                    if (!te.hasDSV)
                                    {
                                        throw std::runtime_error("DX12: CommandBeginPass: depth texture has no DSV");
                                    }

                                    Barrier(te.resource.Get(), te.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);

                                    dsv = te.dsv;
                                    hasDSV = true;
                                    curDSVFormat = te.dsvFormat;
                                }

                                curPassIsSwapChain = false;

                                curNumRT = numRT;

                                // Bind RT/DSV
                                const D3D12_CPU_DESCRIPTOR_HANDLE* rtvPtr = (numRT > 0) ? rtvs.data() : nullptr;
                                const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = (hasDSV) ? &dsv : nullptr;
                                cmdList_->OMSetRenderTargets(numRT, rtvPtr, FALSE, dsvPtr);

                                // Clear
                                if (c.clearColor && numRT > 0)
                                {
                                    const float* col = c.color.data();
                                    for (UINT i = 0; i < numRT; ++i)
                                    {
                                        cmdList_->ClearRenderTargetView(rtvs[i], col, 0, nullptr);
                                    }
                                }

                                if (c.clearDepth && hasDSV)
                                {
                                    cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, c.depth, 0, 0, nullptr);
                                }
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandEndPass>)
                        {
                            if (curPassIsSwapChain)
                            {
                                TransitionBackBuffer(D3D12_RESOURCE_STATE_PRESENT);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandSetViewport>)
                        {
                            D3D12_VIEWPORT viewport{};
                            viewport.TopLeftX = static_cast<float>(cmd.x);
                            viewport.TopLeftY = static_cast<float>(cmd.y);
                            viewport.Width = static_cast<float>(cmd.width);
                            viewport.Height = static_cast<float>(cmd.height);
                            viewport.MinDepth = 0.0f;
                            viewport.MaxDepth = 1.0f;
                            cmdList_->RSSetViewports(1, &viewport);

                            D3D12_RECT scissor{};
                            scissor.left = cmd.x;
                            scissor.top = cmd.y;
                            scissor.right = cmd.x + cmd.width;
                            scissor.bottom = cmd.y + cmd.height;
                            cmdList_->RSSetScissorRects(1, &scissor);
                        }
                        else if constexpr (std::is_same_v<T, CommandSetState>)
                        {
                            curState = cmd.state;
                        }
                        else if constexpr (std::is_same_v<T, CommandBindPipeline>)
                        {
                            curPipe = cmd.pso;
                        }
                        else if constexpr (std::is_same_v<T, CommandBindInputLayout>)
                        {
                            curLayout = cmd.layout;
                        }
                        else if constexpr (std::is_same_v<T, CommandBindVertexBuffer>)
                        {
                            if (cmd.slot >= kMaxVBSlots)
                            {
                                throw std::runtime_error("DX12: BindVertexBuffer: slot out of range");
                            }
                            vertexBuffers[cmd.slot] = cmd.buffer;
                            vbStrides[cmd.slot] = cmd.strideBytes;
                            vbOffsets[cmd.slot] = cmd.offsetBytes;
                        }
                        else if constexpr (std::is_same_v<T, CommandBindIndexBuffer>)
                        {
                            indexBuffer = cmd.buffer;
                            ibType = cmd.indexType;
                            ibOffset = cmd.offsetBytes;
                        }
                        else if constexpr (std::is_same_v<T, CommnadBindTexture2D>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                auto it = textures_.find(cmd.texture.id);
                                if (it == textures_.end())
                                {
                                    throw std::runtime_error("DX12: BindTexture2D: texture not found in textures_ map");
                                }

                                if (!it->second.hasSRV)
                                {
                                    throw std::runtime_error("DX12: BindTexture2D: texture has no SRV");
                                }

                                TransitionTexture(cmd.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                boundTex[cmd.slot] = GetTextureSRV(cmd.texture);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandBindTextureCube>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                auto it = textures_.find(cmd.texture.id);
                                if (it == textures_.end())
                                {
                                    throw std::runtime_error("DX12: BindTextureCube: texture not found in textures_ map");
                                }

                                if (!it->second.hasSRV)
                                {
                                    throw std::runtime_error("DX12: BindTextureCube: texture has no SRV");
                                }

                                TransitionTexture(cmd.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                boundTex[cmd.slot] = GetTextureSRV(cmd.texture);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandTextureDesc>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                TextureHandle handle = ResolveTextureHandleFromDesc(cmd.texture);
                                if (!handle)
                                {
                                    // null SRV
                                    boundTex[cmd.slot] = srvHeap_->GetGPUDescriptorHandleForHeapStart();
                                    return;
                                }

                                TransitionTexture(handle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                boundTex[cmd.slot] = GetTextureSRV(handle);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandBindStructuredBufferSRV>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                boundTex[cmd.slot] = GetBufferSRV(cmd.buffer);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandSetUniformInt> ||
                            std::is_same_v<T, CommandUniformFloat4> ||
                            std::is_same_v<T, CommandUniformMat4>)
                        {
                            // DX12 backend does not interpret the name-based uniform commands.
                            // Use CommandSetConstants instead.
                        }
                        else if constexpr (std::is_same_v<T, CommandSetConstants>)
                        {
                            perDrawSlot = cmd.slot;
                            perDrawSize = cmd.size;
                            if (perDrawSize > 256)
                            {
                                perDrawSize = 256;
                            }
                            if (perDrawSize != 0)
                            {
                                std::memcpy(perDrawBytes.data(), cmd.data.data(), perDrawSize);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandDrawIndexed>)
                        {
                            // PSO + RootSig
                            ID3D12PipelineState* pso = EnsurePSO(curPipe, curLayout);
                            cmdList_->SetPipelineState(pso);
                            cmdList_->SetGraphicsRootSignature(rootSig_.Get());

                            // IA bindings (slot0..slotN based on input layout)
                            auto layIt = layouts_.find(curLayout.id);
                            if (layIt == layouts_.end())
                            {
                                throw std::runtime_error("DX12: input layout handle not found");
                            }

                            std::uint32_t maxSlot = 0;
                            for (const auto& e : layIt->second.elems)
                            {
                                maxSlot = std::max(maxSlot, static_cast<std::uint32_t>(e.InputSlot));
                            }
                            const std::uint32_t numVB = maxSlot + 1;
                            if (numVB > kMaxVBSlots)
                            {
                                throw std::runtime_error("DX12: input layout uses more VB slots than supported");
                            }

                            std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVBSlots> vbv{};
                            for (std::uint32_t s = 0; s < numVB; ++s)
                            {
                                if (!vertexBuffers[s])
                                {
                                    throw std::runtime_error("DX12: missing vertex buffer binding for required slot");
                                }
                                auto vbIt = buffers_.find(vertexBuffers[s].id);
                                if (vbIt == buffers_.end())
                                {
                                    throw std::runtime_error("DX12: vertex buffer not found");
                                }

                                const std::uint32_t off = vbOffsets[s];
                                vbv[s].BufferLocation = vbIt->second.resource->GetGPUVirtualAddress() + off;
                                vbv[s].SizeInBytes = (UINT)(vbIt->second.desc.sizeInBytes - off);
                                vbv[s].StrideInBytes = vbStrides[s];
                            }
                            cmdList_->IASetVertexBuffers(0, numVB, vbv.data());
                            cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                            if (indexBuffer)
                            {
                                auto ibIt = buffers_.find(indexBuffer.id);
                                if (ibIt == buffers_.end())
                                {
                                    throw std::runtime_error("DX12: index buffer not found");
                                }
                                D3D12_INDEX_BUFFER_VIEW ibv{};
                                ibv.BufferLocation = ibIt->second.resource->GetGPUVirtualAddress() + ibOffset
                                    + static_cast<UINT64>(cmd.firstIndex) * static_cast<UINT64>(IndexSizeBytes(cmd.indexType));
                                ibv.SizeInBytes = static_cast<UINT>(ibIt->second.desc.sizeInBytes - ibOffset);
                                ibv.Format = (cmd.indexType == IndexType::UINT16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

                                cmdList_->IASetIndexBuffer(&ibv);
                            }

                            // Root bindings: CBV (0) + SRV table (1)
                            WriteCBAndBind();

                            for (UINT i = 0; i < kMaxSRVSlots; ++i)
                            {
                                cmdList_->SetGraphicsRootDescriptorTable(1 + i, boundTex[i]);
                            }

                            cmdList_->DrawIndexedInstanced(cmd.indexCount, cmd.instanceCount, 0, cmd.baseVertex, cmd.firstInstance);
                        }
                        else if constexpr (std::is_same_v<T, CommandDraw>)
                        {
                            ID3D12PipelineState* pso = EnsurePSO(curPipe, curLayout);
                            cmdList_->SetPipelineState(pso);
                            cmdList_->SetGraphicsRootSignature(rootSig_.Get());

                            // IA bindings (slot0..slotN based on input layout)
                            auto layIt = layouts_.find(curLayout.id);
                            if (layIt == layouts_.end())
                            {
                                throw std::runtime_error("DX12: input layout handle not found");
                            }

                            std::uint32_t maxSlot = 0;
                            for (const auto& e : layIt->second.elems)
                            {
                                maxSlot = std::max(maxSlot, static_cast<std::uint32_t>(e.InputSlot));
                            }
                            const std::uint32_t numVB = maxSlot + 1;
                            if (numVB > kMaxVBSlots)
                            {
                                throw std::runtime_error("DX12: input layout uses more VB slots than supported");
                            }

                            std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVBSlots> vbv{};
                            for (std::uint32_t s = 0; s < numVB; ++s)
                            {
                                if (!vertexBuffers[s])
                                {
                                    throw std::runtime_error("DX12: missing vertex buffer binding for required slot");
                                }
                                auto vbIt = buffers_.find(vertexBuffers[s].id);
                                if (vbIt == buffers_.end())
                                {
                                    throw std::runtime_error("DX12: vertex buffer not found");
                                }

                                const std::uint32_t off = vbOffsets[s];
                                vbv[s].BufferLocation = vbIt->second.resource->GetGPUVirtualAddress() + off;
                                vbv[s].SizeInBytes = (UINT)(vbIt->second.desc.sizeInBytes - off);
                                vbv[s].StrideInBytes = vbStrides[s];
                            }
                            cmdList_->IASetVertexBuffers(0, numVB, vbv.data());
                            cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                            WriteCBAndBind();

                            for (UINT i = 0; i < kMaxSRVSlots; ++i)
                            {
                                cmdList_->SetGraphicsRootDescriptorTable(1 + i, boundTex[i]);
                            }

                            cmdList_->DrawInstanced(cmd.vertexCount, cmd.instanceCount, cmd.firstVertex, cmd.firstInstance);
                        }
                        else if constexpr (std::is_same_v<T, CommandDX12ImGuiRender>)
                        {
                            if (!imguiInitialized_ || !cmd.drawData)
                            {
                                return;
                            }

                            // Ensure ImGui sees the same shader-visible heap.
                            ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
                            cmdList_->SetDescriptorHeaps(1, heaps);

                            ImGui_ImplDX12_RenderDrawData(reinterpret_cast<ImDrawData*>(const_cast<void*>(cmd.drawData)), cmdList_.Get());
                        }
                        else
                        {
                            // other commands ignored
                        }

                    }, command);
            }

            // Close + execute + signal fence for the current frame resource
            EndFrame();
        }

        // ---------------- Bindless descriptor indices ----------------
        TextureDescIndex AllocateTextureDesctiptor(TextureHandle texture) override
        {
            // 0 = invalid
            TextureDescIndex idx{};
            if (!freeTexDesc_.empty())
            {
                idx = freeTexDesc_.back();
                freeTexDesc_.pop_back();
            }
            else
            {
                idx = TextureDescIndex{ nextTexDesc_++ };
            }

            UpdateTextureDescriptor(idx, texture);
            return idx;
        }

        void UpdateTextureDescriptor(TextureDescIndex idx, TextureHandle tex) override
        {
            if (!tex)
            {
                descToTex_[idx] = {};
                return;
            }

            descToTex_[idx] = tex;

            auto it = textures_.find(tex.id);
            if (it == textures_.end())
            {
                throw std::runtime_error("DX12: UpdateTextureDescriptor: texture not found");
            }

            auto& te = it->second;
            if (!te.hasSRV)
            {
                if (te.srvFormat == DXGI_FORMAT_UNKNOWN)
                {
                    throw std::runtime_error("DX12: UpdateTextureDescriptor: texture has no SRV format");
                }


                AllocateSRV(te, te.srvFormat, /*mips*/ 1);
            }
        }

        void FreeTextureDescriptor(TextureDescIndex index) noexcept override
        {
            descToTex_.erase(index);
            freeTexDesc_.push_back(index);
        }

        // ---------------- Fences (минимально) ----------------
        FenceHandle CreateFence(bool signaled = false) override
        {
            const auto id = ++nextFenceId_;
            fences_[id] = signaled;
            return FenceHandle{ id };
        }

        void DestroyFence(FenceHandle fence) noexcept override
        {
            fences_.erase(fence.id);
        }

        void SignalFence(FenceHandle fence) override
        {
            fences_[fence.id] = true;
        }

        void WaitFence(FenceHandle) override {}

        bool IsFenceSignaled(FenceHandle fence) override
        {
            auto it = fences_.find(fence.id);
            return it != fences_.end() && it->second;
        }

        ID3D12Device* NativeDevice() const
        {
            return core_.device.Get();
        }
        ID3D12CommandQueue* NativeQueue() const
        {
            return core_.cmdQueue.Get();
        }
        ID3D12DescriptorHeap* NativeSRVHeap() const
        {
            return srvHeap_.Get();
        }
        UINT NativeSRVInc() const
        {
            return srvInc_;
        }

    private:
        friend class DX12SwapChain;

        struct BufferEntry
        {
            BufferDesc desc{};
            ComPtr<ID3D12Resource> resource;

            // Track state for proper COPY_DEST transitions when uploading.
            D3D12_RESOURCE_STATES state{ D3D12_RESOURCE_STATE_COMMON };

            // Optional SRV for StructuredBuffer reads (t2 in the demo).
            bool hasSRV{ false };
            UINT srvIndex{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
        };

        struct InputLayoutEntry
        {
            std::vector<std::string> semanticStorage;
            std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
            std::uint32_t strideBytes{ 0 };
        };

        struct ShaderEntry
        {
            ShaderStage stage{};
            std::string name;
            ComPtr<ID3DBlob> blob;
        };

        struct PipelineEntry
        {
            std::string debugName;
            ShaderHandle vs{};
            ShaderHandle ps{};
        };

        struct TextureEntry
        {
            enum class Type : std::uint8_t
            {
                Tex2D,
                Cube
            };

            TextureHandle handle{};
            Extent2D extent{};
            Format format{ Format::Unknown };
            Type type{ Type::Tex2D };

            ComPtr<ID3D12Resource> resource;

            DXGI_FORMAT resourceFormat{ DXGI_FORMAT_UNKNOWN };
            DXGI_FORMAT srvFormat{ DXGI_FORMAT_UNKNOWN };
            DXGI_FORMAT rtvFormat{ DXGI_FORMAT_UNKNOWN };
            DXGI_FORMAT dsvFormat{ DXGI_FORMAT_UNKNOWN };

            D3D12_RESOURCE_STATES state{ D3D12_RESOURCE_STATE_COMMON };

            bool hasSRV{ false };
            UINT srvIndex{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};

            // For Tex2D render targets
            bool hasRTV{ false };
            UINT rtvIndex{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};

            // For cubemap render targets (one RTV per face)
            bool hasRTVFaces{ false };
            std::array<UINT, 6> rtvIndexFaces{};
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 6> rtvFaces{};

            bool hasDSV{ false };
            UINT dsvIndex{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        };


        struct FramebufferEntry
        {
            TextureHandle color{};
            TextureHandle depth{};

            // UINT32_MAX means "regular 2D color attachment".
            std::uint32_t colorCubeFace{ 0xFFFFFFFFu };
        };

        static constexpr std::uint32_t kFramesInFlight = 3;
        static constexpr UINT kPerFrameCBUploadBytes = 256u * 1024u;
        static constexpr UINT kPerFrameBufUploadBytes = 8u * 1024u * 1024u; // 8 MB per frame buffer upload ring
        static constexpr UINT kMaxSRVSlots = 20; // t0..t19 (room for PBR maps + env)

        struct FrameResource
        {
            ComPtr<ID3D12CommandAllocator> cmdAlloc;

            // Small persistent upload buffer for per-draw constants.
            ComPtr<ID3D12Resource> cbUpload;
            std::byte* cbMapped{ nullptr };
            std::uint32_t cbCursor{ 0 };

            // Per-frame upload buffer for dynamic DEFAULT buffers (lights/instances/etc).
            ComPtr<ID3D12Resource> bufUpload;
            std::byte* bufMapped{ nullptr };
            std::uint32_t bufCursor{ 0 };

            // Fence value that marks when GPU finished using this frame resource.
            UINT64 fenceValue{ 0 };

            // Deferred lifetime management:
            //  - keep resources alive until GPU is done with this frame
            //  - recycle descriptor indices only after the same fence is completed
            std::vector<ComPtr<ID3D12Resource>> deferredResources;
            std::vector<UINT> deferredFreeSrv;
            std::vector<UINT> deferredFreeRtv;
            std::vector<UINT> deferredFreeDsv;

            void ResetForRecording() noexcept
            {
                cbCursor = 0;
                bufCursor = 0;
            }

            void ReleaseDeferred(
                std::vector<UINT>& globalFreeSrv,
                std::vector<UINT>& globalFreeRtv,
                std::vector<UINT>& globalFreeDsv)
            {
                deferredResources.clear();

                globalFreeSrv.insert(globalFreeSrv.end(), deferredFreeSrv.begin(), deferredFreeSrv.end());
                globalFreeRtv.insert(globalFreeRtv.end(), deferredFreeRtv.begin(), deferredFreeRtv.end());
                globalFreeDsv.insert(globalFreeDsv.end(), deferredFreeDsv.begin(), deferredFreeDsv.end());

                deferredFreeSrv.clear();
                deferredFreeRtv.clear();
                deferredFreeDsv.clear();
            }
        };

        void WaitForFence(UINT64 v)
        {
            if (v == 0)
                return;

            if (fence_->GetCompletedValue() < v)
            {
                ThrowIfFailed(fence_->SetEventOnCompletion(v, fenceEvent_), "DX12: SetEventOnCompletion failed");
                WaitForSingleObject(fenceEvent_, INFINITE);
            }
        }

        void ImmediateUploadBuffer(BufferEntry& dst, std::span<const std::byte> data, std::size_t dstOffsetBytes)
        {
            if (!dst.resource || data.empty())
                return;

            // Temp upload resource
            ComPtr<ID3D12Resource> upload;

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resourceDesc.Width = std::max<UINT64>(1, static_cast<UINT64>(data.size()));
            resourceDesc.Height = 1;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&upload)),
                "DX12: ImmediateUploadBuffer - Create upload resource failed");

            void* mapped = nullptr;
            ThrowIfFailed(upload->Map(0, nullptr, &mapped), "DX12: ImmediateUploadBuffer - Map upload failed");
            std::memcpy(mapped, data.data(), data.size());
            upload->Unmap(0, nullptr);

            // Record tiny copy list
            ComPtr<ID3D12CommandAllocator> alloc;
            ThrowIfFailed(NativeDevice()->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&alloc)),
                "DX12: ImmediateUploadBuffer - CreateCommandAllocator failed");

            ComPtr<ID3D12GraphicsCommandList> cl;
            ThrowIfFailed(NativeDevice()->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                alloc.Get(),
                nullptr,
                IID_PPV_ARGS(&cl)),
                "DX12: ImmediateUploadBuffer - CreateCommandList failed");

            auto Transition = [&](D3D12_RESOURCE_STATES desired)
                {
                    if (dst.state == desired) return;
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = dst.resource.Get();
                    b.Transition.StateBefore = dst.state;
                    b.Transition.StateAfter = desired;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    cl->ResourceBarrier(1, &b);
                    dst.state = desired;
                };

            Transition(D3D12_RESOURCE_STATE_COPY_DEST);

            cl->CopyBufferRegion(
                dst.resource.Get(),
                static_cast<UINT64>(dstOffsetBytes),
                upload.Get(),
                0,
                static_cast<UINT64>(data.size()));

            Transition(D3D12_RESOURCE_STATE_GENERIC_READ);

            ThrowIfFailed(cl->Close(), "DX12: ImmediateUploadBuffer - Close failed");

            ID3D12CommandList* lists[] = { cl.Get() };
            NativeQueue()->ExecuteCommandLists(1, lists);

            const UINT64 v = ++fenceValue_;
            ThrowIfFailed(NativeQueue()->Signal(fence_.Get(), v), "DX12: ImmediateUploadBuffer - Signal failed");
            WaitForFence(v);
        }

        void FlushPendingBufferUpdates()
        {
            if (pendingBufferUpdates_.empty())
                return;

            FrameResource& fr = CurrentFrame();

            for (const PendingBufferUpdate& u : pendingBufferUpdates_)
            {
                auto it = buffers_.find(u.buffer.id);
                if (it == buffers_.end()) continue;

                BufferEntry& dst = it->second;
                if (!dst.resource || u.data.empty()) continue;

                const std::uint32_t size = static_cast<std::uint32_t>(u.data.size());
                const std::uint32_t aligned = AlignUp(size, 16u);

                if (fr.bufCursor + aligned > kPerFrameBufUploadBytes)
                {
                    throw std::runtime_error("DX12: per-frame buffer upload ring overflow (increase kPerFrameBufUploadBytes)");
                }

                std::memcpy(fr.bufMapped + fr.bufCursor, u.data.data(), size);

                auto Transition = [&](D3D12_RESOURCE_STATES desired)
                    {
                        if (dst.state == desired) return;
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = dst.resource.Get();
                        b.Transition.StateBefore = dst.state;
                        b.Transition.StateAfter = desired;
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        cmdList_->ResourceBarrier(1, &b);
                        dst.state = desired;
                    };

                Transition(D3D12_RESOURCE_STATE_COPY_DEST);

                cmdList_->CopyBufferRegion(
                    dst.resource.Get(),
                    static_cast<UINT64>(u.dstOffsetBytes),
                    fr.bufUpload.Get(),
                    static_cast<UINT64>(fr.bufCursor),
                    static_cast<UINT64>(size));

                Transition(D3D12_RESOURCE_STATE_GENERIC_READ);

                fr.bufCursor += aligned;
            }

            pendingBufferUpdates_.clear();
        }

        FrameResource& CurrentFrame() noexcept
        {
            return frames_[activeFrameIndex_];
        }

        void BeginFrame()
        {
            const std::uint32_t swapIdx = static_cast<std::uint32_t>(swapChain_->FrameIndex());
            activeFrameIndex_ = swapIdx % kFramesInFlight;

            FrameResource& fr = frames_[activeFrameIndex_];

            // Wait until GPU is done with this frame resource, then recycle deferred objects/indices.
            WaitForFence(fr.fenceValue);
            fr.ReleaseDeferred(freeSrv_, freeRTV_, freeDSV_);

            ThrowIfFailed(fr.cmdAlloc->Reset(), "DX12: cmdAlloc reset failed");
            ThrowIfFailed(cmdList_->Reset(fr.cmdAlloc.Get(), nullptr), "DX12: cmdList reset failed");

            fr.ResetForRecording();
        }

        void EndFrame()
        {
            ThrowIfFailed(cmdList_->Close(), "DX12: cmdList close failed");

            ID3D12CommandList* lists[] = { cmdList_.Get() };
            NativeQueue()->ExecuteCommandLists(1, lists);

            const UINT64 v = ++fenceValue_;
            ThrowIfFailed(NativeQueue()->Signal(fence_.Get(), v), "DX12: Signal failed");
            frames_[activeFrameIndex_].fenceValue = v;
        }

        void FlushGPU()
        {
            const UINT64 v = ++fenceValue_;
            ThrowIfFailed(NativeQueue()->Signal(fence_.Get(), v), "DX12: Signal failed");
            WaitForFence(v);
        }

        void CreateRootSignature()
        {
            // Root signature layout:
            //  [0]  CBV(b0)   - per-draw constants
            //  [1+] SRV(t0+)  - individual SRV descriptor tables (1 descriptor each)
            //
            // We deliberately use one-descriptor tables per register to allow binding arbitrary
            // SRV heap entries without requiring contiguous descriptor ranges.
            //
            // SRV registers used by shaders:
            //  t0      - albedo (Texture2D)
            //  t1      - directional shadow map (Texture2D<float>)
            //  t2      - lights (StructuredBuffer<GPULight>)
            //  t3..t6  - spot shadow maps [0..3] (Texture2D<float>)
            //  t7..t10 - point distance cubemaps [0..3] (TextureCube<float>)
            //  t11     - shadow metadata (StructuredBuffer<ShadowDataSB>)
            //
            //  DX12 PBR extras (main forward shader):
            //  t12 normal (Texture2D)
            //  t13 metalness (Texture2D)
            //  t14 roughness (Texture2D)
            //  t15 ao (Texture2D)
            //  t16 emissive (Texture2D)
            //  t17 env cube (TextureCube)
            //
            // Samplers:

            //  s0 - linear wrap
            //  s1 - comparison sampler for shadow maps (clamp)
            //  s2 - point clamp (used by point shadows / env sampling)
            std::array<D3D12_DESCRIPTOR_RANGE, kMaxSRVSlots> ranges{};
            for (UINT i = 0; i < kMaxSRVSlots; ++i)
            {
                ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                ranges[i].NumDescriptors = 1;
                ranges[i].BaseShaderRegister = i; // ti
                ranges[i].RegisterSpace = 0;
                ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            }

            std::array<D3D12_ROOT_PARAMETER, 1 + kMaxSRVSlots> rootParams{};

            // b0
            rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rootParams[0].Descriptor.ShaderRegister = 0;
            rootParams[0].Descriptor.RegisterSpace = 0;
            rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            // t0..t11
            for (UINT i = 0; i < kMaxSRVSlots; ++i)
            {
                rootParams[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rootParams[1 + i].DescriptorTable.NumDescriptorRanges = 1;
                rootParams[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
                rootParams[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            }

            D3D12_STATIC_SAMPLER_DESC samplers[3]{};

            auto MakeStaticSampler =
                [](UINT reg,
                    D3D12_FILTER filter,
                    D3D12_TEXTURE_ADDRESS_MODE addrU,
                    D3D12_TEXTURE_ADDRESS_MODE addrV,
                    D3D12_TEXTURE_ADDRESS_MODE addrW,
                    D3D12_COMPARISON_FUNC cmp,
                    D3D12_STATIC_BORDER_COLOR borderColor)
                {
                    D3D12_STATIC_SAMPLER_DESC s{};
                    s.ShaderRegister = reg;
                    s.Filter = filter;
                    s.AddressU = addrU;
                    s.AddressV = addrV;
                    s.AddressW = addrW;
                    s.MipLODBias = 0.0f;
                    s.MaxAnisotropy = 1;
                    s.ComparisonFunc = cmp;
                    s.BorderColor = borderColor;
                    s.MinLOD = 0.0f;
                    s.MaxLOD = D3D12_FLOAT32_MAX;
                    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                    return s;
                };

            // s0: linear wrap
            samplers[0] = MakeStaticSampler(
                0,
                D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                D3D12_COMPARISON_FUNC_ALWAYS,
                D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

            // s1: shadow comparison sampler (clamp)
            samplers[1] = MakeStaticSampler(
                1,
                D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                D3D12_COMPARISON_FUNC_LESS_EQUAL,
                D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

            // s2: point clamp
            samplers[2] = MakeStaticSampler(
                2,
                D3D12_FILTER_MIN_MAG_MIP_POINT,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                D3D12_COMPARISON_FUNC_ALWAYS,
                D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

            D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
            rootSigDesc.NumParameters = static_cast<UINT>(rootParams.size());
            rootSigDesc.pParameters = rootParams.data();
            rootSigDesc.NumStaticSamplers = 3;
            rootSigDesc.pStaticSamplers = samplers;
            rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

            ComPtr<ID3DBlob> serialized;
            ComPtr<ID3DBlob> error;

            HRESULT hr = D3D12SerializeRootSignature(
                &rootSigDesc,
                D3D_ROOT_SIGNATURE_VERSION_1,
                serialized.GetAddressOf(),
                error.GetAddressOf());

            if (FAILED(hr))
            {
                std::string msg = "DX12: D3D12SerializeRootSignature failed";
                if (error)
                {
                    msg += ": ";
                    msg += static_cast<const char*>(error->GetBufferPointer());
                }
                throw std::runtime_error(msg);
            }

            ThrowIfFailed(NativeDevice()->CreateRootSignature(
                0,
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(rootSig_.ReleaseAndGetAddressOf())),
                "DX12: CreateRootSignature failed");
        }


        void EnsureRTVHeap()
        {
            if (rtvHeap_)
            {
                return;
            }
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = 256;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            ThrowIfFailed(NativeDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_)),
                "DX12: Create RTV heap failed");
            rtvInc_ = NativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            nextRTV_ = 0;
            freeRTV_.clear();
        }

        void EnsureDSVHeap()
        {
            if (dsvHeap_)
            {
                return;
            }
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            heapDesc.NumDescriptors = 256;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            ThrowIfFailed(NativeDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap_)),
                "DX12: Create DSV heap failed");
            dsvInc_ = NativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            nextDSV_ = 0;
            freeDSV_.clear();
        }

        D3D12_CPU_DESCRIPTOR_HANDLE AllocateRTV(ID3D12Resource* res, DXGI_FORMAT fmt, UINT& outIndex)
        {
            UINT idx = 0;
            if (!freeRTV_.empty())
            {
                idx = freeRTV_.back();
                freeRTV_.pop_back();
            }
            else
            {
                idx = nextRTV_++;
            }
            outIndex = idx;

            if (idx >= 256u)
            {
                throw std::runtime_error("DX12: RTV heap exhausted (increase EnsureRTVHeap() NumDescriptors).");
            }

            D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(idx) * rtvInc_;

            D3D12_RENDER_TARGET_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            viewDesc.Format = fmt;
            viewDesc.Texture2D.MipSlice = 0;
            viewDesc.Texture2D.PlaneSlice = 0;
            NativeDevice()->CreateRenderTargetView(res, &viewDesc, handle);
            return handle;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE AllocateRTVTexture2DArraySlice(ID3D12Resource* res, DXGI_FORMAT fmt, UINT arraySlice, UINT& outIndex)
        {
            EnsureRTVHeap();

            UINT idx = 0;
            if (!freeRTV_.empty())
            {
                idx = freeRTV_.back();
                freeRTV_.pop_back();
            }
            else
            {
                idx = nextRTV_++;
            }

            D3D12_RENDER_TARGET_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Format = fmt;
            viewDesc.Texture2DArray.MipSlice = 0;
            viewDesc.Texture2DArray.FirstArraySlice = arraySlice;
            viewDesc.Texture2DArray.ArraySize = 1;
            viewDesc.Texture2DArray.PlaneSlice = 0;

            const D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            {
                rtvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(idx) * static_cast<SIZE_T>(rtvInc_)
            };

            NativeDevice()->CreateRenderTargetView(res, &viewDesc, cpu);
            outIndex = idx;
            return cpu;
        }


        D3D12_CPU_DESCRIPTOR_HANDLE AllocateDSV(ID3D12Resource* res, DXGI_FORMAT fmt, UINT& outIndex)
        {
            UINT idx = 0;
            if (!freeDSV_.empty())
            {
                idx = freeDSV_.back();
                freeDSV_.pop_back();
            }
            else
            {
                idx = nextDSV_++;
            }
            outIndex = idx;

            if (idx >= 256u)
            {
                throw std::runtime_error("DX12: DSV heap exhausted (increase EnsureDSVHeap() NumDescriptors).");
            }

            D3D12_CPU_DESCRIPTOR_HANDLE handle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(idx) * dsvInc_;

            D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            viewDesc.Format = fmt;
            viewDesc.Flags = D3D12_DSV_FLAG_NONE;
            viewDesc.Texture2D.MipSlice = 0;
            NativeDevice()->CreateDepthStencilView(res, &viewDesc, handle);
            return handle;
        }

        UINT AllocateSrvIndex()
        {
            UINT idx = 0;

            if (!freeSrv_.empty())
            {
                idx = freeSrv_.back();
                freeSrv_.pop_back();
            }
            else
            {
                idx = nextSrvIndex_++;
            }

            if (idx >= 4096u)
            {
                throw std::runtime_error("DX12: SRV heap exhausted (increase SRV heap NumDescriptors).");
            }

            return idx;
        }

        void AllocateStructuredBufferSRV(BufferEntry& entry)
        {
            if (entry.hasSRV)
            {
                return;
            }

            const auto stride = entry.desc.structuredStrideBytes;
            if (stride == 0)
            {
                throw std::runtime_error("DX12: StructuredBuffer SRV requested but structuredStrideBytes == 0");
            }

            const UINT64 totalBytes = static_cast<UINT64>(entry.desc.sizeInBytes);
            const UINT numElems = static_cast<UINT>(totalBytes / static_cast<UINT64>(stride));
            if (numElems == 0)
            {
                throw std::runtime_error("DX12: StructuredBuffer SRV requested but NumElements == 0");
            }

            const UINT idx = AllocateSrvIndex();

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(idx) * srvInc_;

            D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(idx) * static_cast<UINT64>(srvInc_);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN; // structured
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = numElems;
            srvDesc.Buffer.StructureByteStride = stride;
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

            NativeDevice()->CreateShaderResourceView(entry.resource.Get(), &srvDesc, cpu);

            entry.hasSRV = true;
            entry.srvIndex = idx;
            entry.srvCpu = cpu;
            entry.srvGpu = gpu;
        }

        void AllocateSRV(TextureEntry& entry, DXGI_FORMAT fmt, UINT mipLevels)
        {
            if (entry.hasSRV)
            {
                return;
            }

            const UINT idx = AllocateSrvIndex();

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(idx) * srvInc_;

            D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(idx) * static_cast<UINT64>(srvInc_);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = fmt;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            if (entry.type == TextureEntry::Type::Cube)
            {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = 0;
                srvDesc.TextureCube.MipLevels = mipLevels;
                srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
            }
            else
            {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = 0;
                srvDesc.Texture2D.MipLevels = mipLevels;
                srvDesc.Texture2D.PlaneSlice = 0;
                srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            }

            NativeDevice()->CreateShaderResourceView(entry.resource.Get(), &srvDesc, cpu);

            entry.hasSRV = true;
            entry.srvIndex = idx;
            entry.srvCpu = cpu;
            entry.srvGpu = gpu;
        }

    private:
        dx12::Core core_{};

        // Frame resources (allocator + per-frame constant upload ring)
        std::array<FrameResource, kFramesInFlight> frames_{};
        std::uint32_t activeFrameIndex_{ 0 };

        ComPtr<ID3D12GraphicsCommandList> cmdList_;

        ComPtr<ID3D12Fence> fence_;
        HANDLE fenceEvent_{ nullptr };
        UINT64 fenceValue_{ 0 };

        // Shared root signature
        ComPtr<ID3D12RootSignature> rootSig_;

        // SRV heap (shader visible)
        ComPtr<ID3D12DescriptorHeap> srvHeap_;
        UINT srvInc_{ 0 };

        static constexpr UINT kImGuiFontSrvIndex = 2; // reserved SRV slot for ImGui font texture
        bool imguiInitialized_{ false };

        UINT nextSrvIndex_{ 1 };
        std::vector<UINT> freeSrv_;

        // RTV/DSV heaps for transient textures (swapchain has its own RTV/DSV)
        ComPtr<ID3D12DescriptorHeap> rtvHeap_;
        ComPtr<ID3D12DescriptorHeap> dsvHeap_;
        UINT rtvInc_{ 0 };
        UINT dsvInc_{ 0 };
        UINT nextRTV_{ 0 };
        UINT nextDSV_{ 0 };
        std::vector<UINT> freeRTV_;
        std::vector<UINT> freeDSV_;

        // Pointers
        DX12SwapChain* swapChain_{ nullptr };

        // Resource tables
        std::uint32_t nextBufId_{ 1 };
        std::uint32_t nextTexId_{ 1 };
        std::uint32_t nextShaderId_{ 1 };
        std::uint32_t nextPsoId_{ 1 };
        std::uint32_t nextLayoutId_{ 1 };
        std::uint32_t nextFBId_{ 1 };
        std::uint32_t nextDescId_{ 0 };
        std::uint32_t nextFenceId_{ 1 };

        std::unordered_map<std::uint32_t, BufferEntry> buffers_;
        std::unordered_map<std::uint32_t, TextureEntry> textures_;
        std::unordered_map<std::uint32_t, ShaderEntry> shaders_;
        std::unordered_map<std::uint32_t, PipelineEntry> pipelines_;
        std::unordered_map<std::uint32_t, InputLayoutEntry> layouts_;
        std::unordered_map<std::uint32_t, FramebufferEntry> framebuffers_;

        std::unordered_map<TextureDescIndex, TextureHandle> descToTex_;
        std::vector<TextureDescIndex> freeTexDesc_{};
        uint32_t nextTexDesc_ = 1;
        std::unordered_map<std::uint32_t, bool> fences_;


        std::vector<PendingBufferUpdate> pendingBufferUpdates_;

        std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> psoCache_;
    };

    DX12SwapChain::DX12SwapChain(DX12Device& owner, DX12SwapChainDesc desc)
        : device_(owner)
        , chainSwapDesc_(std::move(desc))
    {
        if (!chainSwapDesc_.hwnd)
        {
            throw std::runtime_error("DX12SwapChain: hwnd is null");
        }

        ComPtr<IDXGIFactory6> factory;
        ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DX12: CreateDXGIFactory2 failed");

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
        swapChainDesc.Width = chainSwapDesc_.base.extent.width;
        swapChainDesc.Height = chainSwapDesc_.base.extent.height;
        swapChainDesc.Format = ToDXGIFormat(chainSwapDesc_.base.backbufferFormat);
        bbFormat_ = swapChainDesc.Format;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = std::max(2u, chainSwapDesc_.bufferCount);
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGISwapChain1> swapChain1;
        ThrowIfFailed(factory->CreateSwapChainForHwnd(
            device_.NativeQueue(),
            chainSwapDesc_.hwnd,
            &swapChainDesc,
            nullptr, nullptr,
            &swapChain1),
            "DX12: CreateSwapChainForHwnd failed");

        ThrowIfFailed(swapChain1.As(&swapChain_), "DX12: swapchain As IDXGISwapChain4 failed");
        ThrowIfFailed(factory->MakeWindowAssociation(chainSwapDesc_.hwnd, DXGI_MWA_NO_ALT_ENTER), "DX12: MakeWindowAssociation failed");

        // RTV heap for backbuffers
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = swapChainDesc.BufferCount;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            ThrowIfFailed(device_.NativeDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_)),
                "DX12: Create swapchain RTV heap failed");
            rtvInc_ = device_.NativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }

        backBuffers_.resize(swapChainDesc.BufferCount);
        for (UINT i = 0; i < swapChainDesc.BufferCount; ++i)
        {
            ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])),
                "DX12: GetBuffer failed");

            D3D12_CPU_DESCRIPTOR_HANDLE descHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            descHandle.ptr += static_cast<SIZE_T>(i) * rtvInc_;
            device_.NativeDevice()->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, descHandle);
        }

        currBackBuffer_ = swapChain_->GetCurrentBackBufferIndex();

        // Depth (D32)
        depthFormat_ = DXGI_FORMAT_D32_FLOAT;
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            heapDesc.NumDescriptors = 1;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            ThrowIfFailed(device_.NativeDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap_)),
                "DX12: Create swapchain DSV heap failed");

            D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resourceDesc.Width = chainSwapDesc_.base.extent.width;
            resourceDesc.Height = chainSwapDesc_.base.extent.height;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = depthFormat_;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE clearValue{};
            clearValue.Format = depthFormat_;
            clearValue.DepthStencil.Depth = 1.0f;
            clearValue.DepthStencil.Stencil = 0;

            ThrowIfFailed(device_.NativeDevice()->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                &clearValue,
                IID_PPV_ARGS(&depth_)),
                "DX12: Create depth buffer failed");

            D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            viewDesc.Format = depthFormat_;
            viewDesc.Flags = D3D12_DSV_FLAG_NONE;
            viewDesc.Texture2D.MipSlice = 0;

            device_.NativeDevice()->CreateDepthStencilView(depth_.Get(), &viewDesc, dsv);
            dsv_ = dsv;
        }

        backBufferStates_.resize(backBuffers_.size());
        ResetBackBufferStates(D3D12_RESOURCE_STATE_PRESENT);
        currBackBuffer_ = swapChain_->GetCurrentBackBufferIndex();
    }

    SwapChainDesc DX12SwapChain::GetDesc() const
    {
        return chainSwapDesc_.base;
    }

    FrameBufferHandle DX12SwapChain::GetCurrentBackBuffer() const
    {
        // similar to GL: 0 stands to swapchain backbuffer
        return FrameBufferHandle{ 0 };
    }

    void DX12SwapChain::EnsureSizeUpToDate()
    {
        // TODO: could be extended
    }

    void DX12SwapChain::Present()
    {
        const UINT syncInterval = chainSwapDesc_.base.vsync ? 1u : 0u;
        ThrowIfFailed(swapChain_->Present(syncInterval, 0), "DX12: Present failed");
        currBackBuffer_ = swapChain_->GetCurrentBackBufferIndex();
    }

    // Public factory functions
    inline std::unique_ptr<IRHIDevice> CreateDX12Device()
    {
        return std::make_unique<DX12Device>();
    }

    inline std::unique_ptr<IRHISwapChain> CreateDX12SwapChain(IRHIDevice& device, DX12SwapChainDesc desc)
    {
        auto* dxDev = dynamic_cast<DX12Device*>(&device);
        if (!dxDev)
        {
            throw std::runtime_error("CreateDX12SwapChain: device is not DX12Device");
        }

        auto swapChainDesc = std::make_unique<DX12SwapChain>(*dxDev, std::move(desc));
        dxDev->SetSwapChain(swapChainDesc.get());
        return swapChainDesc;
    }

#else
    inline std::unique_ptr<IRHIDevice> CreateDX12Device() { return CreateNullDevice(); }
#endif
} // namespace rhi