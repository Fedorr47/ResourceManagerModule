module;

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <array>

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

	struct Extend2D
	{
		std::uint32_t width{};
		std::uint32_t height{};
	};

	struct SwapChainDesc
	{
		Extend2D extent{};
		Format format{ Format::BGRA8_UNORM };
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
		bool depthTestEnable{ true };
		bool depthWriteEnable{ true };
		CompareOp depthCompareOp{ CompareOp::LessEqual };
	};

	struct RasterizerState
	{
		CullMode cullMode{ CullMode::Back };
		FrontFace frontFace{ FrontFace::CounterClockwise };
	};

	struct BlendState
	{
		bool blendEnable{ false };
	};

	struct GraphicsState
	{
		DepthState depthState{};
		RasterizerState rasterizerState{};
		BlendState blendState{};
	};

	struct ClearDesc
	{
		bool clearColor{ true };
		bool clearDepth{ false };
		std::array<float, 4> color{ 0.0f, 0.0f, 0.0f, 1.0f };
		float depth{ 1.0f };
	};

	struct BeginPassDesc {
		FramebufferHandle framebuffer{};
		Extend2D extent{};
		ClearDesc clearDesc{};
	};

	struct CommandList
	{
		std::vector<std::string> debug;

		void Add(std::string_view text)
		{
			debug.emplace_back(text);
		}
	};

	class IRHISwapChain
	{
	public:
		virtual ~IRHISwapChain() = default;
		virtual SwapChainDesc GetDesc() const = 0;
		virtual TextureHandle GetCurrentBackBuffer() const = 0;
		virtual void Present() = 0;
	};

	class IRHIDevice
	{
		public:
		virtual ~IRHIDevice() = default;

		virtual std::string_view GetName() const = 0;

		virtual TextureHandle CreateTexture2D(Extend2D extent, Format format) = 0;
		virtual void DestroyTexture(TextureHandle texture) noexcept = 0;

		virtual BufferHandle CreateBuffer(std::size_t sizeInBytes) = 0;
		virtual void DestroyBuffer(BufferHandle buffer) noexcept = 0;

		virtual ShaderHandle CreateShader(std::string_view debugName, std::string_view sourceOfByteCode) = 0;
		virtual void DestroyShader(ShaderHandle shader) noexcept = 0;

		virtual PipelineHandle CreatePipeline(std::string_view debugName, ShaderHandle vertexShader, ShaderHandle fragmentShader) = 0;
		virtual void DestroyPipeline(PipelineHandle pipeline) noexcept = 0;

		virtual CommandList BeginFrame() = 0;
		virtual void SubmitCommandList(CommandList&& commandList) = 0;
		virtual void EndFrame() = 0;
	};

	std::unique_ptr<IRHIDevice> CreateNullDevice();
	std::unique_ptr<IRHISwapChain> CreateNullSwapChain(IRHIDevice& device, SwapChainDesc desc);
}

namespace rhi
{
	class NullSwapChain : public rhi::IRHISwapChain
	{
	public:
		explicit NullSwapChain(SwapChainDesc desc) : desc_(desc) 
		{
			backBuffer_.id = 1;
		}
		~NullSwapChain() override = default;

		rhi::SwapChainDesc GetDesc() const override 
		{ 
			return desc_; 
		}
		rhi::TextureHandle GetCurrentBackBuffer() const override 
		{ 
			return backBuffer_; 
		}
		void Present() override {}

	private:
		SwapChainDesc desc_;
		TextureHandle backBuffer_{};
	};

	class NullDevice : public rhi::IRHIDevice
	{
		public:
		std::string_view GetName() const override 
		{ 
			return "Null RHI Device"; 
		}

		TextureHandle CreateTexture2D(Extend2D extent, Format format) override
		{
			TextureHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void DestroyTexture(TextureHandle texture) noexcept override {}

		BufferHandle CreateBuffer(std::size_t sizeInBytes) override
		{
			BufferHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void DestroyBuffer(BufferHandle buffer) noexcept override {}

		ShaderHandle CreateShader(std::string_view debugName, std::string_view sourceOfByteCode) override
		{
			ShaderHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void DestroyShader(ShaderHandle shader) noexcept override {}

		PipelineHandle CreatePipeline(std::string_view debugName, ShaderHandle vertexShader, ShaderHandle fragmentShader) override
		{
			PipelineHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void DestroyPipeline(PipelineHandle pipeline) noexcept override {}

		CommandList BeginFrame() override
		{
			CommandList commandList{};
			commandList.Add("Begin Frame");
			return commandList;
		}

		void SubmitCommandList(CommandList&& commandList) override 
		{
			last_ = std::move(commandList);
		}
		void EndFrame() override 
		{
			if (!last_.debug.empty())
			{
				last_.debug.clear();
			}
		}

	private:
		std::uint64_t nextId_{ 100 };
		CommandList last_;
	};

	std::unique_ptr<IRHIDevice> CreateNullDevice()
	{
		return std::make_unique<NullDevice>();
	}

	std::unique_ptr<IRHISwapChain> CreateNullSwapChain(IRHIDevice& device, SwapChainDesc desc)
	{
		return std::make_unique<NullSwapChain>(desc);
	}
}