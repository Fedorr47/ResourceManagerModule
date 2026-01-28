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

    class DX12SwapChain final : public IRHISwapChain
    {
    public:
        DX12SwapChain(DX12Device& device, DX12SwapChainDesc desc);
        ~DX12SwapChain() override = default;

        SwapChainDesc GetDesc() const override;
        FrameBufferHandle GetCurrentBackBuffer() const override;
        void Present() override;

        ID3D12Resource* CurrentBackBuffer() const { return backBuffers_[frameIndex_].Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTV() const
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(frameIndex_) * rtvInc_;
            return handle;
        }

        ID3D12Resource* DepthBuffer() const { return depth_.Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE DSV() const { return dsv_; }

        DXGI_FORMAT BackBufferFormat() const { return bbFormat_; }
        DXGI_FORMAT DepthFormat() const { return depthFormat_; }

        void EnsureSizeUpToDate();

    private:
        DX12Device& device_;
        DX12SwapChainDesc chainSwapDesc_;

        ComPtr<IDXGISwapChain4> swapChain_;
        ComPtr<ID3D12DescriptorHeap> rtvHeap_;
        UINT rtvInc_{ 0 };

        std::vector<ComPtr<ID3D12Resource>> backBuffers_;
        UINT frameIndex_{ 0 };
        DXGI_FORMAT bbFormat_{ DXGI_FORMAT_B8G8R8A8_UNORM };

        ComPtr<ID3D12Resource> depth_;
        ComPtr<ID3D12DescriptorHeap> dsvHeap_;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_{};
        DXGI_FORMAT depthFormat_{ DXGI_FORMAT_D32_FLOAT };
    };

#if defined(_WIN32)
    class DX12Device final : public IRHIDevice
    {
    public:
        DX12Device()
        {
            core_.Init();

            // Command allocator/list
            ThrowIfFailed(NativeDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc_)),
                "DX12: CreateCommandAllocator failed");
            ThrowIfFailed(NativeDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc_.Get(), nullptr, IID_PPV_ARGS(&cmdList_)),
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
                heapDesc.NumDescriptors = 1024;
                heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                ThrowIfFailed(NativeDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap_)),
                    "DX12: Create SRV heap failed");
                srvInc_ = NativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

                // null SRV in slot 0
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
                D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
                nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                nullSrv.Texture2D.MipLevels = 1;
                NativeDevice()->CreateShaderResourceView(nullptr, &nullSrv, cpu);

                nextSrvIndex_ = 1;
            }

            // Constant buffer (upload) – 64KB
            {
                const UINT constBufSize = 64u * 1024u;
                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

                D3D12_RESOURCE_DESC resourceDesc{};
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                resourceDesc.Width = constBufSize;
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
                    IID_PPV_ARGS(&constantBufferUpload_)),
                    "DX12: Create constant upload buffer failed");

                ThrowIfFailed(constantBufferUpload_->Map(0, nullptr, reinterpret_cast<void**>(&constantBufferMapped_)),
                    "DX12: Map constant upload buffer failed");
            }

            CreateRootSignature();
        }

        ~DX12Device() override
        {
            if (constantBufferUpload_)
            {
                constantBufferUpload_->Unmap(0, nullptr);
            }
            constantBufferMapped_ = nullptr;

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

        void SetSwapChain(DX12SwapChain* swapChain) 
        { 
            swapChain_ = swapChain; 
        }

        std::string_view GetName() const override 
        { 
            return "DirectX12 RHI"; 
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
                resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                clearValue.DepthStencil.Depth = 1.0f;
                clearValue.DepthStencil.Stencil = 0;
                initState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                    &heapProps, 
                    D3D12_HEAP_FLAG_NONE, 
                    &resourceDesc, 
                    initState, 
                    &clearValue, 
                    IID_PPV_ARGS(&textureEntry.resource)),
                    "DX12: Create depth texture failed");

                EnsureDSVHeap();
                textureEntry.dsv = AllocateDSV(textureEntry.resource.Get(), dxFmt);
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

                EnsureRTVHeap();
                textureEntry.rtv = AllocateRTV(textureEntry.resource.Get(), dxFmt);
            }

            textures_[textureHandle.id] = std::move(textureEntry);
            return textureHandle;
        }

        void DestroyTexture(TextureHandle texture) noexcept override
        {
            if (texture.id == 0) 
            {
                return;
            }
            textures_.erase(texture.id);
            // SRV slot reclaim
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

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resourceDesc.Width = std::max<UINT64>(1, sz);
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
                IID_PPV_ARGS(&bufferEntry.resource)),
                "DX12: CreateBuffer failed");

            buffers_[handle.id] = std::move(bufferEntry);
            return handle;
        }

        void UpdateBuffer(BufferHandle buffer, std::span<const std::byte> data, std::size_t offsetBytes = 0) override
        {
            auto it = buffers_.find(buffer.id);
            if (it == buffers_.end()) 
            {
                return;
            }

            void* ptrBuffer = nullptr;
            D3D12_RANGE readRange{ 0, 0 };
            ThrowIfFailed(it->second.resource->Map(0, &readRange, &ptrBuffer), "DX12: Map buffer failed");

            std::memcpy(static_cast<std::uint8_t*>(ptrBuffer) + offsetBytes, data.data(), data.size());

            it->second.resource->Unmap(0, nullptr);
        }

        void DestroyBuffer(BufferHandle buffer) noexcept override
        {
            buffers_.erase(buffer.id);
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

                D3D12_INPUT_ELEMENT_DESC elemDesc{};
                elemDesc.SemanticName = inputLayoutEntry.semanticStorage.back().c_str();
                elemDesc.SemanticIndex = attribute.semanticIndex;
                elemDesc.Format = ToDXGIVertexFormat(attribute.format);
                elemDesc.InputSlot = attribute.inputSlot;
                elemDesc.AlignedByteOffset = attribute.offsetBytes;
                elemDesc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                elemDesc.InstanceDataStepRate = 0;

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

            const char* entry = (stage == ShaderStage::Vertex) ? "VSMain" : "PSMain";
            const char* target = (stage == ShaderStage::Vertex) ? "vs_5_1" : "ps_5_1";

            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

            ComPtr<ID3DBlob> code;
            ComPtr<ID3DBlob> errors;

            HRESULT hr = D3DCompile(
                sourceOrBytecode.data(),
                sourceOrBytecode.size(),
                shaderEntry.name.c_str(),
                nullptr, nullptr,
                entry, target,
                flags, 0,
                &code, &errors);

            if (FAILED(hr))
            {
                std::string err = "DX12: shader compile failed: ";
                if (errors) 
                {
                    err += std::string(reinterpret_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
                }
                throw std::runtime_error(err);
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

            // Reset native command list
            ThrowIfFailed(cmdAlloc_->Reset(), "DX12: cmdAlloc reset failed");
            ThrowIfFailed(cmdList_->Reset(cmdAlloc_.Get(), nullptr), "DX12: cmdList reset failed");

            // Set descriptor heaps (SRV)
            ID3D12DescriptorHeap* heaps[] = { NativeSRVHeap() };
            cmdList_->SetDescriptorHeaps(1, heaps);

            // State while parsing high-level commands
            GraphicsState curState{};
            PipelineHandle curPipe{};
            InputLayoutHandle curLayout{};
            BufferHandle vertexBuffer{};
            std::uint32_t vbStride = 0;
            std::uint32_t vbOffset = 0;

            BufferHandle indexBuffer{};
            IndexType ibType = IndexType::UINT16;
            std::uint32_t ibOffset = 0;

            // Bound textures by slot (we actuallu use only slot 0)
            std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 8> boundTex{};
            for (auto& t : boundTex)
            {
                t = srvHeap_->GetGPUDescriptorHandleForHeapStart(); // null SRV slot0
            }

			// Per-draw constants (raw bytes).
			// The renderer is responsible for packing the layout expected by HLSL.
			std::array<std::byte, 256> perDrawBytes{};
			std::uint32_t perDrawSize = 0;
			std::uint32_t perDrawSlot = 0;

            constantBufferCursor_ = 0;

			auto WriteCBAndBind = [&]()
                {
					const std::uint32_t used = (perDrawSize == 0) ? 1u : perDrawSize;
					const std::uint32_t cbSize = AlignUp(used, 256);
                    if (constantBufferCursor_ + cbSize > (64u * 1024u))
                    {
                        constantBufferCursor_ = 0; // wrap 
                    }

					if (perDrawSize != 0)
					{
						std::memcpy(constantBufferMapped_ + constantBufferCursor_, perDrawBytes.data(), perDrawSize);
					}

                    const D3D12_GPU_VIRTUAL_ADDRESS gpuVA = constantBufferUpload_->GetGPUVirtualAddress() + constantBufferCursor_;
					cmdList_->SetGraphicsRootConstantBufferView(perDrawSlot, gpuVA);

                    constantBufferCursor_ += cbSize;
                };

            auto ResolveTextureHandleFromDesc = [&](TextureDescIndex idx) -> TextureHandle
                {
                    auto it = descToTex_.find(idx);
                    if (it == descToTex_.end()) 
                    {
                        return {};
                    }
                    return it->second;
                };

            auto GetTextureSRV = [&](TextureHandle textureHandle) -> D3D12_GPU_DESCRIPTOR_HANDLE
                {
                    if (!textureHandle) return srvHeap_->GetGPUDescriptorHandleForHeapStart();
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

            auto EnsurePSO = [&](PipelineHandle pipelineHandle, InputLayoutHandle layout) -> ID3D12PipelineState*
                {
                    const std::uint64_t key =
                        (static_cast<std::uint64_t>(pipelineHandle.id) << 32ull) |
                        (static_cast<std::uint64_t>(layout.id) & 0xffffffffull);

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

                    pipelineDesc.NumRenderTargets = 1;
                    pipelineDesc.RTVFormats[0] = swapChain_->BackBufferFormat();
                    pipelineDesc.DSVFormat = swapChain_->DepthFormat();

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
                            // frameBuffer.id == 0 => swapchain backbuffer
                            D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapChain_->CurrentRTV();
                            D3D12_CPU_DESCRIPTOR_HANDLE dsv = swapChain_->DSV();
                            cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

                            // viewport & scissor
                            D3D12_VIEWPORT viewport{};
                            viewport.TopLeftX = 0;
                            viewport.TopLeftY = 0;
                            viewport.Width = static_cast<float>(cmd.desc.extent.width);
                            viewport.Height = static_cast<float>(cmd.desc.extent.height);
                            viewport.MinDepth = 0.0f;
                            viewport.MaxDepth = 1.0f;
                            cmdList_->RSSetViewports(1, &viewport);

                            D3D12_RECT scissor{};
                            scissor.left = 0;
                            scissor.top = 0;
                            scissor.right = static_cast<LONG>(cmd.desc.extent.width);
                            scissor.bottom = static_cast<LONG>(cmd.desc.extent.height);
                            cmdList_->RSSetScissorRects(1, &scissor);

                            // Clear
                            if (cmd.desc.clearDesc.clearColor)
                            {
                                cmdList_->ClearRenderTargetView(rtv, cmd.desc.clearDesc.color.data(), 0, nullptr);
                            }
                            if (cmd.desc.clearDesc.clearDepth)
                            {
                                cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, cmd.desc.clearDesc.depth, 0, 0, nullptr);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandEndPass>)
                        {
                            // no-op
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
                            vertexBuffer = cmd.buffer;
                            vbStride = cmd.strideBytes;
                            vbOffset = cmd.offsetBytes;
                        }
                        else if constexpr (std::is_same_v<T, CommandBindIndexBuffer>)
                        {
                            indexBuffer = cmd.buffer;
                            ibType = cmd.indexType;
                            ibOffset = cmd.offsetBytes;
                        }
                        else if constexpr (std::is_same_v<T, CommnadBindTextue2D>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                boundTex[cmd.slot] = GetTextureSRV(cmd.texture);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandTextureDesc>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                TextureHandle textureHandle = ResolveTextureHandleFromDesc(cmd.texture);
                                boundTex[cmd.slot] = GetTextureSRV(textureHandle);
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

                            // IA bindings
                            auto vbIt = buffers_.find(vertexBuffer.id);
                            if (vbIt == buffers_.end())
                            {
                                throw std::runtime_error("DX12: vertex buffer not found");
                            }   

                            D3D12_VERTEX_BUFFER_VIEW vbv{};
                            vbv.BufferLocation = vbIt->second.resource->GetGPUVirtualAddress() + vbOffset;
                            vbv.SizeInBytes = static_cast<UINT>(vbIt->second.desc.sizeInBytes - vbOffset);
                            vbv.StrideInBytes = vbStride;

                            cmdList_->IASetVertexBuffers(0, 1, &vbv);
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
                            cmdList_->SetGraphicsRootDescriptorTable(1, boundTex[0]);

                            cmdList_->DrawIndexedInstanced(cmd.indexCount, 1, 0, cmd.baseVertex, 0);
                        }
                        else if constexpr (std::is_same_v<T, CommandDraw>)
                        {
                            ID3D12PipelineState* pso = EnsurePSO(curPipe, curLayout);
                            cmdList_->SetPipelineState(pso);
                            cmdList_->SetGraphicsRootSignature(rootSig_.Get());

                            auto vbIt = buffers_.find(vertexBuffer.id);
                            if (vbIt == buffers_.end())
                            {
                                throw std::runtime_error("DX12: vertex buffer not found");
                            }
                            
                            D3D12_VERTEX_BUFFER_VIEW vbv{};
                            vbv.BufferLocation = vbIt->second.resource->GetGPUVirtualAddress() + vbOffset;
                            vbv.SizeInBytes = static_cast<UINT>(vbIt->second.desc.sizeInBytes - vbOffset);
                            vbv.StrideInBytes = vbStride;

                            cmdList_->IASetVertexBuffers(0, 1, &vbv);
                            cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                            WriteCBAndBind();
                            cmdList_->SetGraphicsRootDescriptorTable(1, boundTex[0]);

                            cmdList_->DrawInstanced(cmd.vertexCount, 1, cmd.firstVertex, 0);
                        }
                        else
                        {
                            // other commands ignored
                        }

                    }, command);
            }

            ThrowIfFailed(cmdList_->Close(), "DX12: cmdList close failed");

            ID3D12CommandList* lists[] = { cmdList_.Get() };
            NativeQueue()->ExecuteCommandLists(1, lists);

            // Wait GPU (простая, но надежная синхронизация для демо)
            SignalAndWait();
        }

        // ---------------- Bindless descriptor indices ----------------
        TextureDescIndex AllocateTextureDesctiptor(TextureHandle tex) override
        {
            const auto idx = ++nextDescId_;
            descToTex_[idx] = tex;
            return idx;
        }

        void UpdateTextureDescriptor(TextureDescIndex idx, TextureHandle tex) override
        {
            descToTex_[idx] = tex;
        }

        void FreeTextureDescriptor(TextureDescIndex idx) noexcept override
        {
            descToTex_.erase(idx);
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
            Extent2D extent{};
            rhi::Format format{ rhi::Format::Unknown };
            ComPtr<ID3D12Resource> resource;

            // Render targets / depth
            bool hasRTV{ false };
            bool hasDSV{ false };
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
            D3D12_CPU_DESCRIPTOR_HANDLE dsv{};

            // Sampled
            bool hasSRV{ false };
            UINT srvIndex{ 0 };
            D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
        };

        struct FramebufferEntry
        {
            TextureHandle color{};
            TextureHandle depth{};
        };

        void SignalAndWait()
        {
            const UINT64 v = ++fenceValue_;
            ThrowIfFailed(NativeQueue()->Signal(fence_.Get(), v), "DX12: Signal failed");
            if (fence_->GetCompletedValue() < v)
            {
                ThrowIfFailed(fence_->SetEventOnCompletion(v, fenceEvent_), "DX12: SetEventOnCompletion failed");
                WaitForSingleObject(fenceEvent_, INFINITE);
            }
        }

        void CreateRootSignature()
        {
            // Root params: 0 = CBV(b0), 1 = SRV table (t0)
            D3D12_DESCRIPTOR_RANGE1 range{};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            range.BaseShaderRegister = 0;
            range.RegisterSpace = 0;
            range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
            range.OffsetInDescriptorsFromTableStart = 0;

            D3D12_ROOT_PARAMETER1 params[2]{};

            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[0].Descriptor.ShaderRegister = 0;
            params[0].Descriptor.RegisterSpace = 0;
            params[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            params[1].DescriptorTable.NumDescriptorRanges = 1;
            params[1].DescriptorTable.pDescriptorRanges = &range;

            // Static sampler s0
            D3D12_STATIC_SAMPLER_DESC samp{};
            samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samp.MipLODBias = 0;
            samp.MaxAnisotropy = 1;
            samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            samp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
            samp.MinLOD = 0.0f;
            samp.MaxLOD = D3D12_FLOAT32_MAX;
            samp.ShaderRegister = 0; // s0
            samp.RegisterSpace = 0;
            samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSignature{};
            rootSignature.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            rootSignature.Desc_1_1.NumParameters = 2;
            rootSignature.Desc_1_1.pParameters = params;
            rootSignature.Desc_1_1.NumStaticSamplers = 1;
            rootSignature.Desc_1_1.pStaticSamplers = &samp;
            rootSignature.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

            ComPtr<ID3DBlob> blob;
            ComPtr<ID3DBlob> err;
            ThrowIfFailed(D3D12SerializeVersionedRootSignature(&rootSignature, &blob, &err),
                "DX12: Serialize root signature failed");

            ThrowIfFailed(NativeDevice()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_)),
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
        }

        void EnsureDSVHeap()
        {
            if (dsvHeap_) 
            {
                return;
            }
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            heapDesc.NumDescriptors = 64;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            ThrowIfFailed(NativeDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap_)),
                "DX12: Create DSV heap failed");
            dsvInc_ = NativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            nextDSV_ = 0;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE AllocateRTV(ID3D12Resource* res, DXGI_FORMAT fmt)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(nextRTV_) * rtvInc_;
            ++nextRTV_;

            D3D12_RENDER_TARGET_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            viewDesc.Format = fmt;
            viewDesc.Texture2D.MipSlice = 0;
            viewDesc.Texture2D.PlaneSlice = 0;
            NativeDevice()->CreateRenderTargetView(res, &viewDesc, handle);
            return handle;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE AllocateDSV(ID3D12Resource* res, DXGI_FORMAT fmt)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(nextDSV_) * dsvInc_;
            ++nextDSV_;

            D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            viewDesc.Format = fmt;
            viewDesc.Flags = D3D12_DSV_FLAG_NONE;
            viewDesc.Texture2D.MipSlice = 0;
            NativeDevice()->CreateDepthStencilView(res, &viewDesc, handle);
            return handle;
        }

        void AllocateSRV(TextureEntry& texureEntry, DXGI_FORMAT fmt, UINT mipLevels)
        {
            const UINT idx = nextSrvIndex_++;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(idx) * srvInc_;

            D3D12_SHADER_RESOURCE_VIEW_DESC resViewDesc{};
            resViewDesc.Format = fmt;
            resViewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            resViewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            resViewDesc.Texture2D.MostDetailedMip = 0;
            resViewDesc.Texture2D.MipLevels = mipLevels;
            resViewDesc.Texture2D.ResourceMinLODClamp = 0.0f;

            NativeDevice()->CreateShaderResourceView(texureEntry.resource.Get(), &resViewDesc, cpu);

            D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<SIZE_T>(idx) * srvInc_;

            texureEntry.hasSRV = true;
            texureEntry.srvIndex = idx;
            texureEntry.srvGpu = gpu;
        }

    private:
        dx12::Core core_{};

        ComPtr<ID3D12CommandAllocator> cmdAlloc_;
        ComPtr<ID3D12GraphicsCommandList> cmdList_;

        ComPtr<ID3D12Fence> fence_;
        HANDLE fenceEvent_{ nullptr };
        UINT64 fenceValue_{ 0 };

        // Shared root signature
        ComPtr<ID3D12RootSignature> rootSig_;

        // SRV heap
        ComPtr<ID3D12DescriptorHeap> srvHeap_;
        UINT srvInc_{ 0 };
        UINT nextSrvIndex_{ 1 };

        // RTV/DSV heaps for transient textures (swapchain has its own RTV/DSV)
        ComPtr<ID3D12DescriptorHeap> rtvHeap_;
        ComPtr<ID3D12DescriptorHeap> dsvHeap_;
        UINT rtvInc_{ 0 };
        UINT dsvInc_{ 0 };
        UINT nextRTV_{ 0 };
        UINT nextDSV_{ 0 };

        // Constant upload
        ComPtr<ID3D12Resource> constantBufferUpload_;
        std::uint8_t* constantBufferMapped_{ nullptr };
        std::uint32_t constantBufferCursor_{ 0 };

        // Pointers
        DX12SwapChain* swapChain_{ nullptr };

        // Resource tables
        std::uint32_t nextBufId_{ 1 };
        std::uint32_t nextTexId_{ 1 };
        std::uint32_t nextShaderId_{ 1 };
        std::uint32_t nextPsoId_{ 1 };
        std::uint32_t nextLayoutId_{ 1 };
        std::uint32_t nextFBId_{ 1 };
        std::uint32_t nextDescId_{ 1 };
        std::uint32_t nextFenceId_{ 1 };

        std::unordered_map<std::uint32_t, BufferEntry> buffers_;
        std::unordered_map<std::uint32_t, TextureEntry> textures_;
        std::unordered_map<std::uint32_t, ShaderEntry> shaders_;
        std::unordered_map<std::uint32_t, PipelineEntry> pipelines_;
        std::unordered_map<std::uint32_t, InputLayoutEntry> layouts_;
        std::unordered_map<std::uint32_t, FramebufferEntry> framebuffers_;

        std::unordered_map<TextureDescIndex, TextureHandle> descToTex_;
        std::unordered_map<std::uint32_t, bool> fences_;

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

        frameIndex_ = swapChain_->GetCurrentBackBufferIndex();

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
        frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
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
