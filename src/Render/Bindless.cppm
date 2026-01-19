module;

export module core:render_bindless;

import :rhi;

// Bindless rendering related classes and functions

// Higher level code should prefer descriptor indices instead of API-specific handles.
// This keeps the renderer portable across different RHI backends (OpenGL/Vulkan/DX12).

export namespace render
{
	class BindlessTable
	{
	public:
		explicit BindlessTable(rhi::IRHIDevice& device) 
			: device_(device) {}

		rhi::TextureDescIndex RegisterTexture(rhi::TextureHandle texture)
		{
			rhi::TextureDescIndex index = device_.AllocateTextureDesctiptor(texture);
			return index;
		}

		void UpdateTexture(rhi::TextureDescIndex index, rhi::TextureHandle texture)
		{
			device_.UpdateTextureDescriptor(index, texture);
		}

		void UnregisterTexture(rhi::TextureDescIndex index) noexcept
		{
			device_.FreeTextureDescriptor(index);
		}

	private:
		rhi::IRHIDevice& device_;
	};
}