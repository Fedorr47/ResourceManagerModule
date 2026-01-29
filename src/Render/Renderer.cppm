module;

#include <memory>
#include <utility>

export module core:render_renderer;

// Re-export shared settings so external code can keep using
// `rendern::RendererSettings` via `import core:render`.
export import :renderer_settings;

import :rhi;

#if defined(CORE_USE_GL)
import :renderer_mesh_gl;
#endif

#if defined(CORE_USE_DX12)
import :renderer_dx12;
#endif

namespace rendern
{
    namespace detail
    {
        // ---------------------------------------------------------------------
        // Facade implementation
        // ---------------------------------------------------------------------
        struct IRendererImpl
        {
            virtual ~IRendererImpl() = default;
            virtual void RenderFrame(rhi::IRHISwapChain& swapChain, rhi::TextureHandle sampledTexture, rhi::TextureDescIndex sampledTextureDescIndex) = 0;
            virtual void Shutdown() = 0;
        };

        class NullRendererImpl final : public IRendererImpl
        {
        public:
            void RenderFrame(rhi::IRHISwapChain& swapChain, rhi::TextureHandle, rhi::TextureDescIndex) override
            {
                swapChain.Present();
            }
            void Shutdown() override {}
        };

        #if defined(CORE_USE_GL)
        class GLRendererImpl final : public IRendererImpl
        {
        public:
            GLRendererImpl(rhi::IRHIDevice& device, RendererSettings settings)
                : impl_(device, std::move(settings))
            {}

            void RenderFrame(rhi::IRHISwapChain& swapChain, rhi::TextureHandle sampledTexture, rhi::TextureDescIndex sampledTextureDescIndex) override
            {
                impl_.RenderFrame(swapChain, sampledTexture, sampledTextureDescIndex);
            }
            void Shutdown() override
            {
                impl_.Shutdown();
            }

        private:
            GLRenderer impl_;
        };
        #endif

#if defined(CORE_USE_DX12)
        class DX12RendererImpl final : public IRendererImpl
        {
        public:
            DX12RendererImpl(rhi::IRHIDevice& device, RendererSettings settings)
                : impl_(device, std::move(settings))
            {}

            void RenderFrame(rhi::IRHISwapChain& swapChain, rhi::TextureHandle sampledTexture, rhi::TextureDescIndex sampledTextureDescIndex) override
            {
                impl_.RenderFrame(swapChain, sampledTexture, sampledTextureDescIndex);
            }
            void Shutdown() override
            {
                impl_.Shutdown();
            }

        private:
            DX12Renderer impl_;
        };
#endif
    }


    // -------------------------------------------------------------------------
    // Public facade
    // -------------------------------------------------------------------------
    export class Renderer
    {
    public:
        Renderer(rhi::IRHIDevice& device, RendererSettings settings = {})
            : device_(device)
        {
            switch (device_.GetBackend())
            {
            case rhi::Backend::OpenGL:
                #if defined(CORE_USE_GL)
                impl_ = std::make_unique<detail::GLRendererImpl>(device_, std::move(settings));
                break;
                #else
                impl_ = std::make_unique<detail::NullRendererImpl>();
                break;
                #endif

            case rhi::Backend::DirectX12:
#if defined(CORE_USE_DX12)
                impl_ = std::make_unique<detail::DX12RendererImpl>(device_, std::move(settings));
                break;
#else
                impl_ = std::make_unique<detail::NullRendererImpl>();
                break;
#endif

            default:
                impl_ = std::make_unique<detail::NullRendererImpl>();
                break;
            }
        }

        void RenderFrame(rhi::IRHISwapChain& swapChain, rhi::TextureHandle sampledTexture = {}, rhi::TextureDescIndex sampledTextureDescIndex = 0)
        {
            impl_->RenderFrame(swapChain, sampledTexture, sampledTextureDescIndex);
        }

        void Shutdown()
        {
            impl_->Shutdown();
        }

    private:
        rhi::IRHIDevice& device_;
        std::unique_ptr<detail::IRendererImpl> impl_;
    };
}
