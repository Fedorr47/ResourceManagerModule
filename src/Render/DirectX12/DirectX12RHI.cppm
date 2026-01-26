module;

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <d3d12.h>
  #include <dxgi1_6.h>
  #include <wrl.h>
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>

export module core:rhi_dx12;

import :rhi;
import :dx12_core;

export namespace rhi
{
#if defined(_WIN32)
    class DX12Device final : public IRHIDevice
    {
    public:
        DX12Device()
        {
            core_.Init();
        }

        std::string_view GetName() const override
        {
            return "DirectX12 RHI";
        }

        Backend GetBackend() const noexcept override
        {
            return Backend::DirectX12;
        }

        // Textures
        TextureHandle CreateTexture2D(Extent2D, Format) override
        {
            return TextureHandle{ ++nextId_ };
        }
        void DestroyTexture(TextureHandle) noexcept override {}

        // Framebuffers
        FrameBufferHandle CreateFramebuffer(TextureHandle, TextureHandle) override
        {
            return FrameBufferHandle{ ++nextId_ };
        }
        void DestroyFramebuffer(FrameBufferHandle) noexcept override {}

        // Buffers
        BufferHandle CreateBuffer(const BufferDesc&) override
        {
            return BufferHandle{ ++nextId_ };
        }
        void UpdateBuffer(BufferHandle, std::span<const std::byte>, std::size_t) override {}
        void DestroyBuffer(BufferHandle) noexcept override {}

        // Input layouts
        InputLayoutHandle CreateInputLayout(const InputLayoutDesc&) override
        {
            return InputLayoutHandle{ ++nextId_ };
        }
        void DestroyInputLayout(InputLayoutHandle) noexcept override {}

        // Shaders / Pipelines
        ShaderHandle CreateShader(ShaderStage, std::string_view, std::string_view) override
        {
            return ShaderHandle{ ++nextId_ };
        }
        void DestroyShader(ShaderHandle) noexcept override {}

        PipelineHandle CreatePipeline(std::string_view, ShaderHandle, ShaderHandle) override
        {
            return PipelineHandle{ ++nextId_ };
        }
        void DestroyPipeline(PipelineHandle) noexcept override {}

        // Submission
        void SubmitCommandList(CommandList&&) override {}

        // Bindless
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

        // Fences
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

    private:
        dx12::Core core_{};
        std::uint32_t nextId_{ 1000 };
        std::uint32_t nextDescId_{ 1 };
        std::uint32_t nextFenceId_{ 1 };
        std::unordered_map<TextureDescIndex, TextureHandle> descToTex_;
        std::unordered_map<std::uint32_t, bool> fences_;
    };

    struct DX12SwapChainDesc
    {
        SwapChainDesc base{};
    };

    inline std::unique_ptr<IRHIDevice> CreateDX12Device()
    {
        return std::make_unique<DX12Device>();
    }

    // Skeleton: for now use NullSwapChain behavior.
    inline std::unique_ptr<IRHISwapChain> CreateDX12SwapChain(IRHIDevice& device, DX12SwapChainDesc desc)
    {
        (void)device;
        return CreateNullSwapChain(device, std::move(desc.base));
    }
#else
    inline std::unique_ptr<IRHIDevice> CreateDX12Device() { return CreateNullDevice(); }
#endif
}
