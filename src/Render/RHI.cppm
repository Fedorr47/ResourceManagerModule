module;

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <array>
#include <variant>
#include <span>
#include <unordered_map>

export module core:rhi;

export namespace rhi
{
	enum class Format : std::uint8_t
	{
		Unknown,
		RGBA8_UNORM,
		BGRA8_UNORM,
		D32_FLOAT
	};

	enum class IndexType : std::uint8_t
	{
		UINT16,
		UINT32
	};

	enum class BufferBindFlag : std::uint8_t
	{
		VertexBuffer,
		IndexBuffer,
		ConstantBuffer,
		UniformBuffer,
		StructuredBuffer
	};

	enum class BufferUsageFlag : std::uint8_t
	{
		Default,
		Static,
		Dynamic,
		Stream
	};

	enum class CompareOp : std::uint8_t
	{
		Never,
		Less,
		Equal,
		LessEqual,
		Greater,
		NotEqual,
		GreaterEqual,
		Always
	};

	enum class CullMode : std::uint8_t
	{
		None,
		Back,
		Front
	};

	enum class FrontFace : std::uint8_t
	{
		CounterClockwise,
		Clockwise
	};

	struct BufferHandle
	{
		std::uint32_t id{};
	};
	struct TextureHandle
	{
		std::uint32_t id{};
	};
	struct ShaderHandle
	{
		std::uint32_t id{};
	};
	struct PipelineHandle
	{
		std::uint32_t id{};
	};
	struct VertexArrayHandle
	{
		std::uint32_t id{};
	};
	struct FramebufferHandle
	{
		std::uint32_t id{};
	};
	struct FenceHandle
	{
		std::uint32_t id{};
	};

	// Vulkan/DX12 backends can map these indices into global descriptor tables.
	// The OpenGL backend emulates them by keeping a small mapping.
	using TextureDescIndex = std::uint32_t;

	struct Extent2D
	{
		std::uint32_t width{};
		std::uint32_t height{};
	};

	struct SwapChainDesc
	{
		Extent2D extent{};
		Format backbufferFormat{ Format::BGRA8_UNORM };
		bool vsync{ true };
	};

	struct BufferDesc
	{
		BufferBindFlag bindFlag{ BufferBindFlag::VertexBuffer };
		BufferUsageFlag usageFlag{ BufferUsageFlag::Default };
		std::size_t sizeInBytes{ 0 };
		std::string debugName{};
	};

	struct VertexAttributeDesc
	{
		uint32_t location{};
		int componentCount{ 0 };
		uint32_t glType{ 0 };
		bool normalized{ false };
		uint32_t strideBytes{ 0 };
		uint32_t offsetBytes{ 0 };
	};

	struct DepthState
	{
		bool testEnable{ true };
		bool writeEnable{ true };
		CompareOp depthCompareOp{ CompareOp::LessEqual };
	};

	struct RasterizerState
	{
		CullMode cullMode{ CullMode::Back };
		FrontFace frontFace{ FrontFace::CounterClockwise };
	};

	struct BlendState
	{
		bool enable{ false };
	};

	struct GraphicsState
	{
		DepthState depth{};
		RasterizerState rasterizer{};
		BlendState blend{};
	};

	struct ClearDesc
	{
		bool clearColor{ true };
		bool clearDepth{ false };
		std::array<float, 4> color{ 0.0f, 0.0f, 0.0f, 1.0f };
		float depth{ 1.0f };
	};

	struct BeginPassDesc {
		FramebufferHandle frameBuffer{};
		Extent2D extent{};
		ClearDesc clearDesc{};
	};

	//------------------------ Command Stream ------------------------/

	struct CommandBeginPass
	{
		BeginPassDesc desc{};
	};
	struct CommandEndPass
	{
	};
	struct CommandSetVieport
	{
		int x{ 0 };
		int y{ 0 };
		int width{ 0 };
		int height{ 0 };
	};
	struct CommandSetState
	{
		GraphicsState state{};
	};
	struct CommandBindPipeline
	{
		PipelineHandle pso{};
	};
	struct CommandVertexArray
	{
		VertexArrayHandle vao{};
	};
	struct CommnadBindTextue2D
	{
		std::uint32_t slot{ 0 };
		TextureHandle texture{};
	};
	struct CommandTextureDesc
	{
		std::uint32_t slot{ 0 };
		TextureDescIndex texture{};
	};
	struct CommandSetUniformInt
	{
		std::string name{};
		int value{ 0 };
	};
	struct CommandUniformFloat4
	{
		std::string name{};
		std::array<float, 4> value{};
	};
	struct CommandDrawIndexed
	{
		std::uint32_t indexCount{ 0 };
		IndexType indexType{ IndexType::UINT16 };
		std::uint32_t firstIndex{ 0 };
		int baseVertex{ 0 };
	};
	struct CommandDraw
	{
		std::uint32_t vertexCount{ 0 };
		std::uint32_t firstVertex{ 0 };
	};

	using Command = std::variant<
		CommandBeginPass,
		CommandEndPass,
		CommandSetVieport,
		CommandSetState,
		CommandBindPipeline,
		CommandVertexArray,
		CommnadBindTextue2D,
		CommandTextureDesc,
		CommandSetUniformInt,
		CommandUniformFloat4,
		CommandDrawIndexed,
		CommandDraw>;

	struct CommandList
	{
		std::vector<Command> commands;

		void BeginPass(const BeginPassDesc& desc)
		{
			commands.emplace_back(CommandBeginPass{ desc });
		}
		void EndPass()
		{
			commands.emplace_back(CommandEndPass{});
		}
		void SetViewport(int x, int y, int width, int height)
		{
			commands.emplace_back(CommandSetVieport{ x, y, width, height });
		}
		void SetState(const GraphicsState& state)
		{
			commands.emplace_back(CommandSetState{ state });
		}
		void BindPipeline(PipelineHandle pso)
		{
			commands.emplace_back(CommandBindPipeline{ pso });
		}
		void BindVertexArray(VertexArrayHandle vao)
		{
			commands.emplace_back(CommandVertexArray{ vao });
		}
		void BindTexture2D(std::uint32_t slot, TextureHandle texture)
		{
			commands.emplace_back(CommnadBindTextue2D{ slot, texture });
		}
		void BindTextureDesc(std::uint32_t slot, TextureDescIndex textureIndex)
		{
			commands.emplace_back(CommandTextureDesc{ slot, textureIndex });
		}
		void SetUniformInt(std::string name, int value)
		{
			commands.emplace_back(CommandSetUniformInt{ std::move(name), value });
		}
		void SetUniformFloat4(std::string name, std::array<float, 4> value)
		{
			commands.emplace_back(CommandUniformFloat4{ std::string(name), value });
		}
		void DrawIndexed(std::uint32_t indexCount, IndexType indexType, std::uint32_t firstIndex = 0, int baseVertex = 0)
		{
			commands.emplace_back(CommandDrawIndexed{ indexCount, indexType, firstIndex, baseVertex });
		}
		void Draw(std::uint32_t vertexCount, std::uint32_t firstVertex = 0)
		{
			commands.emplace_back(CommandDraw{ vertexCount, firstVertex });
		}
	};

	class IRHISwapChain
	{
	public:
		virtual ~IRHISwapChain() = default;
		virtual SwapChainDesc GetDesc() const = 0;
		virtual FramebufferHandle GetCurrentBackBuffer() const = 0;
		virtual void Present() = 0;
	};

	class IRHIDevice
	{
	public:
		virtual ~IRHIDevice() = default;

		virtual std::string_view GetName() const = 0;

		// Textures
		virtual TextureHandle CreateTexture2D(Extent2D extendt, Format format) = 0;
		virtual void DestroyTexture(TextureHandle texture) noexcept = 0;

		// Framebuffers
		virtual FramebufferHandle CreateFramebuffer(TextureHandle color, TextureHandle depth) = 0;
		virtual void DestroyFramebuffer(FramebufferHandle frameBuffer) noexcept = 0;

		// Buffers
		virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
		virtual void UpdateBuffer(BufferHandle buffer, std::span<const std::byte> data, std::size_t offsetBytes = 0) = 0;
		virtual void DestroyBuffer(BufferHandle buffer) noexcept = 0;

		// Vertex Arrays
		virtual VertexArrayHandle CreateVertexArray(std::string_view debugName = {}) = 0;
		virtual void SetVertexArrayLayout(VertexArrayHandle vao, BufferHandle vbo, std::span<const VertexAttributeDesc> attributes) = 0;
		virtual void SetVertexArrayIndexBuffer(VertexArrayHandle vao, BufferHandle ibo, IndexType indexType) = 0;
		virtual void DestroyVertexArray(VertexArrayHandle vao) noexcept = 0;

		// Shaders and Pipelines
		virtual ShaderHandle CreateShader(std::string_view debugName, std::string_view sourceOrBytecode) = 0;
		virtual void DestroyShader(ShaderHandle shader) noexcept = 0;

		virtual PipelineHandle CreatePipeline(std::string_view debugName, ShaderHandle vertexShader, ShaderHandle fragmentShader) = 0;
		virtual void DestroyPipeline(PipelineHandle pso) noexcept = 0;

		// Submission
		virtual void SubmitCommandList(CommandList&& commandList) = 0;

		// Bindless-style descriptor indices
		virtual TextureDescIndex AllocateTextureDesctiptor(TextureHandle texture) = 0;
		virtual void UpdateTextureDescriptor(TextureDescIndex index, TextureHandle texture) = 0;
		virtual void FreeTextureDescriptor(TextureDescIndex index) noexcept = 0;

		// Synchronization
		virtual FenceHandle CreateFence(bool signaled = false) = 0;
		virtual void DestroyFence(FenceHandle fence) noexcept = 0;
		virtual void SignalFence(FenceHandle fence) = 0;
		virtual void WaitFence(FenceHandle fence) = 0;
		virtual bool IsFenceSignaled(FenceHandle fence) = 0;
	};

	std::unique_ptr<IRHIDevice> CreateNullDevice();
	std::unique_ptr<IRHISwapChain> CreateNullSwapChain(IRHIDevice& device, SwapChainDesc desc);
}

namespace rhi
{
	class NullSwapChain : public rhi::IRHISwapChain
	{
	public:
		explicit NullSwapChain(SwapChainDesc desc) : desc_(std::move(desc)) {}
		~NullSwapChain() override = default;

		rhi::SwapChainDesc GetDesc() const override
		{
			return desc_;
		}
		FramebufferHandle GetCurrentBackBuffer() const override
		{
			return FramebufferHandle{ 0 };
		}
		void Present() override {}

	private:
		SwapChainDesc desc_;
	};

	class NullDevice : public rhi::IRHIDevice
	{
	public:
		std::string_view GetName() const override
		{
			return "Null RHI Device";
		}

		TextureHandle CreateTexture2D(Extent2D, Format) override
		{
			TextureHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void DestroyTexture(TextureHandle) noexcept override {}

		FramebufferHandle CreateFramebuffer(TextureHandle,TextureHandle) override
		{
			FramebufferHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void DestroyFramebuffer(FramebufferHandle) noexcept override {}

		BufferHandle CreateBuffer(const BufferDesc&) override
		{
			BufferHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void UpdateBuffer(BufferHandle, std::span<const std::byte>, std::size_t) override {}
		void DestroyBuffer(BufferHandle) noexcept override {}

		VertexArrayHandle CreateVertexArray(std::string_view) override
		{
			VertexArrayHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void SetVertexArrayLayout(VertexArrayHandle, BufferHandle, std::span<const VertexAttributeDesc>) override {}
		void SetVertexArrayIndexBuffer(VertexArrayHandle, BufferHandle, IndexType) override {}
		void DestroyVertexArray(VertexArrayHandle) noexcept override {}

		ShaderHandle CreateShader(std::string_view, std::string_view) override
		{
			ShaderHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void DestroyShader(ShaderHandle) noexcept override {}

		PipelineHandle CreatePipeline(std::string_view, ShaderHandle, ShaderHandle) override
		{
			PipelineHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void DestroyPipeline(PipelineHandle pipeline) noexcept override {}

		void SubmitCommandList(CommandList&& commandList) override {}

		TextureDescIndex AllocateTextureDesctiptor(TextureHandle tex) override
		{
			const auto idx = ++nextDescId_;
			descToTex_[idx] = tex;
			return idx;
		}

		void UpdateTextureDescriptor(TextureDescIndex index, TextureHandle texture) override
		{
			descToTex_[index] = texture;
		}

		void FreeTextureDescriptor(TextureDescIndex index) noexcept override
		{
			descToTex_.erase(index);
		}

		FenceHandle CreateFence(bool signaled = false) override
		{
			FenceHandle handle{};
			handle.id = ++nextFenceId_;
			fenceSignaled_[handle.id] = signaled;
			return handle;
		}

		void DestroyFence(FenceHandle fence) noexcept override
		{
			fenceSignaled_.erase(fence.id);
		}

		void SignalFence(FenceHandle fence) override
		{
			fenceSignaled_[fence.id] = true;
		}

		void WaitFence(FenceHandle fence) override
		{
			// Null backend: always immediate.
		}

		bool IsFenceSignaled(FenceHandle fence) override
		{
			if (auto it = fenceSignaled_.find(fence.id); it != fenceSignaled_.end())
			{
				return it->second;
			}
			return true;
		}

	private:
		std::uint32_t nextId_{ 100 };
		std::uint32_t nextDescId_{ 0 };
		std::uint32_t nextFenceId_{ 0 };
		std::unordered_map<TextureDescIndex, TextureHandle> descToTex_{};
		std::unordered_map<std::uint32_t, bool> fenceSignaled_{};
	};

	std::unique_ptr<IRHIDevice> CreateNullDevice()
	{
		return std::make_unique<NullDevice>();
	}

	std::unique_ptr<IRHISwapChain> CreateNullSwapChain(IRHIDevice& device, SwapChainDesc desc)
	{
		return std::make_unique<NullSwapChain>(std::move(desc));
	}
}