module;

#include <cstdint>
#include <string>
#include <optional>
#include <span>
#include <functional>

export module core:render_graph;

import :rhi;

export namespace renderGraph
{
	enum class ResourceUsage : std::uint8_t
	{
		Unknown,
		RenderTarget,
		DepthStencil,
		Sampled,
		Storage
	};

	struct RGTextureDesc
	{
		rhi::Extent2D extent{ 0, 0 };
		rhi::Format format{ rhi::Format::Unknown };
		ResourceUsage usage{ ResourceUsage::Unknown };
		std::string debugName;
	};

	struct RGTexture
	{
		std::uint32_t id{};
	};

	struct PassAttachments
	{
		bool useSwapChainBackbuffer{ false };

		std::optional<RGTexture> color;
		std::optional<RGTexture> depth;

		rhi::ClearDesc clearDesc{};
	};

	class RenderGraphResources
	{
	public:
		RenderGraphResources(std::span<const rhi::TextureHandle> textures)
			: textures_(textures)
		{
		}

		rhi::TextureHandle GetTexture(const RGTexture& texture) const
		{
			if (texture.id >= textures_.size())
			{
				return {};
			}
			return textures_[texture.id];
		}

	private:
		std::span<const rhi::TextureHandle> textures_;
	};

	struct PassContext
	{
		rhi::IRHIDevice& device;
		rhi::IRHISwapChain& swapChain;
		rhi::CommandList& commandList;
		const RenderGraphResources& resources;
		rhi::Extent2D passExtent;
	};

	using PassCallback = std::function<void(PassContext&)>;

	struct PassNode
	{
		std::string name;
		PassAttachments attachments;
		PassCallback execute;
	};

	class RenderGraph
	{
	public:
		RGTexture CreateTexture(RGTextureDesc desc)
		{
			const std::uint32_t id = static_cast<std::uint32_t>(textures_.size());
			textures_.emplace_back(std::move(desc));
			return RGTexture{ .id = id };
		}

		void AddPass(std::string_view name, PassAttachments attachments, PassCallback callback)
		{
			passes_.emplace_back(PassNode{.name = std::string(name), .attachments = std::move(attachments), .execute = std::move(callback) });
		}

		void AddSwapChainPass(std::string_view name, rhi::ClearDesc clearDesc, PassCallback callback)
		{
			PassAttachments attachments{};
			attachments.useSwapChainBackbuffer = true;
			attachments.clearDesc = clearDesc;
			passes_.emplace_back(PassNode{ .name = std::string(name), .attachments = std::move(attachments), .execute = std::move(callback) });
		}

		void Reset()
		{
			passes_.clear();
			textures_.clear();
		}

		void Execute(rhi::IRHIDevice& device, rhi::IRHISwapChain& swapChain)
		{
			std::vector<rhi::TextureHandle> allocatedTextures;
			allocatedTextures.reserve(textures_.size());
			for (const auto& texDesc : textures_)
			{
				rhi::TextureHandle texture = device.CreateTexture2D(texDesc.extent, texDesc.format);
				allocatedTextures.push_back(texture);
			}

			RenderGraphResources resources(allocatedTextures);
			rhi::CommandList commandList;

			std::vector<rhi::FramebufferHandle> transientFramebuffers;
			transientFramebuffers.reserve(passes_.size());

			for (auto& pass : passes_)
			{
				rhi::FramebufferHandle frameBuffer{};
				rhi::Extent2D passExtent{ 0, 0 };

				if (pass.attachments.useSwapChainBackbuffer)
				{
					frameBuffer = swapChain.GetCurrentBackBuffer();
					passExtent = swapChain.GetDesc().extent;
				}
				else
				{
					const auto color = pass.attachments.color ? resources.GetTexture(*pass.attachments.color) : rhi::TextureHandle();
					const auto depth = pass.attachments.depth ? resources.GetTexture(*pass.attachments.depth) : rhi::TextureHandle();

					if (pass.attachments.color)
					{
						passExtent = textures_[pass.attachments.color->id].extent;
					}
					else if (pass.attachments.depth)
					{
						passExtent = textures_[pass.attachments.depth->id].extent;
					}

					frameBuffer = device.CreateFramebuffer(color, depth);
					transientFramebuffers.push_back(frameBuffer);
				}

				rhi::BeginPassDesc begin{};
				begin.frameBuffer = frameBuffer;
				begin.extent = passExtent;
				begin.clearDesc = pass.attachments.clearDesc;

				commandList.BeginPass(begin);

				PassContext ctx{ device, swapChain, commandList, resources, passExtent };
				pass.execute(ctx);

				commandList.EndPass();
			}

			device.SubmitCommandList(std::move(commandList));

			for (auto frameBuffer : transientFramebuffers)
			{
				device.DestroyFramebuffer(frameBuffer);
			}
			for (auto texture : allocatedTextures)
			{
				device.DestroyTexture(texture);
			}
		}
	private:
		std::vector<PassNode> passes_;
		std::vector<RGTextureDesc> textures_;
	};
}