module;

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <memory>
#include <array>
#include <variant>
#include <span>
#include <unordered_map>
#include <cstring>
#include <stdexcept>

export module core:rhi;

export namespace rhi
{
	enum class Backend
	{
		Null,
		OpenGL,
		DirectX12
	};

	template <typename Tag>
	struct Handle
	{
		std::uint32_t id{ 0 };
		explicit operator bool() const noexcept { return id != 0; }
	};

	struct BufferTag {};
	struct TextureTag {};
	struct ShaderTag {};
	struct PipelineTag {};
	struct FrameBufferTag {};
	struct FenceTag {};
	struct InputLayoutTag {};

	using BufferHandle = Handle<BufferTag>;
	using TextureHandle = Handle<TextureTag>;
	using ShaderHandle = Handle<ShaderTag>;
	using PipelineHandle = Handle<PipelineTag>;
	using FrameBufferHandle = Handle<FrameBufferTag>;
	using FenceHandle = Handle<FenceTag>;
	using InputLayoutHandle = Handle<InputLayoutTag>;

	enum class Format : std::uint8_t
	{
		Unknown,
		RGBA8_UNORM,
		BGRA8_UNORM,
		R32_FLOAT,
		D32_FLOAT,
		D24_UNORM_S8_UINT
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

	enum class VertexFormat : std::uint8_t
	{
		R32G32B32_FLOAT,
		R32G32_FLOAT,
		R32G32B32A32_FLOAT,
		R8G8B8A8_UNORM
	};

	enum class VertexSemantic : std::uint8_t
	{
		Position,
		Normal,
		TexCoord,
		Color,
		Tangent
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

	enum class ShaderStage : std::uint8_t
	{
		Vertex,
		Pixel, // Fragment
		Geometry,
		Compute
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
		std::uint32_t structuredStrideBytes{ 0 }; // For StructuredBuffer SRV (bytes per element)
		std::string debugName{};
	};

	struct VertexAttributeDesc
	{
		VertexSemantic semantic{};
		std::uint8_t semanticIndex{ 0 };
		VertexFormat format{};
		std::uint32_t inputSlot{ 0 };
		std::uint32_t offsetBytes{ 0 };
		bool normalized{ false };
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
		FrameBufferHandle frameBuffer{};
		Extent2D extent{};
		ClearDesc clearDesc{};
	};

	struct InputLayoutDesc
	{
		std::vector<VertexAttributeDesc> attributes;
		std::uint32_t strideBytes{ 0 };
		std::string debugName{};
	};

	//------------------------ Command Stream ------------------------/

	struct CommandBeginPass
	{
		BeginPassDesc desc{};
	};
	struct CommandEndPass
	{
	};
	struct CommandSetViewport
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
	struct CommandBindInputLayout
	{
		InputLayoutHandle layout{};
	};
	struct CommandBindVertexBuffer
	{
		std::uint32_t slot{ 0 };
		BufferHandle buffer{};
		std::uint32_t strideBytes{ 0 };
		std::uint32_t offsetBytes{ 0 };
	};
	struct CommandBindIndexBuffer
	{
		BufferHandle buffer{};
		IndexType indexType{ IndexType::UINT16 };
		std::uint32_t offsetBytes{ 0 };
	};
	struct CommnadBindTexture2D
	{
		std::uint32_t slot{ 0 };
		TextureHandle texture{};
	};
	struct CommandBindTextureCube
	{
		std::uint32_t slot{ 0 };
		TextureHandle texture{};
	};
	struct CommandTextureDesc
	{
		std::uint32_t slot{ 0 };
		TextureDescIndex texture{};
	};
	struct CommandBindStructuredBufferSRV
	{
		std::uint32_t slot{ 0 };
		BufferHandle buffer{};
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
	struct CommandUniformMat4
	{
		std::string name{};
		std::array<float, 16> value{};
	};

	// Backend-agnostic small constant block ("push constants" style).
	// DX12 uses it to feed a per-draw constant buffer without interpreting names.
	// OpenGL can ignore it or emulate it later via UBOs.
	struct CommandSetConstants
	{
		std::uint32_t slot{ 0 };   // backend-defined slot (DX12 root parameter index)
		std::uint32_t size{ 0 };   // bytes used in `data`
		std::array<std::byte, 256> data{};
	};

	// DX12-only: render Dear ImGui draw data into the current render target.
	// Other backends may ignore this command.
	struct CommandDX12ImGuiRender
	{
		const void* drawData{ nullptr }; // ImDrawData*
	};
	struct CommandDrawIndexed
	{
		std::uint32_t indexCount{ 0 };
		IndexType indexType{ IndexType::UINT16 };
		std::uint32_t firstIndex{ 0 };
		int baseVertex{ 0 };
		uint32_t instanceCount{ 1 };
		uint32_t firstInstance{ 0 };
	};
	struct CommandDraw
	{
		std::uint32_t vertexCount{ 0 };
		std::uint32_t firstVertex{ 0 };
		uint32_t instanceCount{ 1 };
		uint32_t firstInstance{ 0 };
	};

	using Command = std::variant<
		CommandBeginPass,
		CommandEndPass,
		CommandSetViewport,
		CommandSetState,
		CommandBindPipeline,
		CommandBindInputLayout,
		CommandBindVertexBuffer,
		CommandBindIndexBuffer,
		CommnadBindTexture2D,
		CommandBindTextureCube,
		CommandTextureDesc,
		CommandBindStructuredBufferSRV,
		CommandSetUniformInt,
		CommandUniformFloat4,
		CommandUniformMat4,
		CommandSetConstants,
		CommandDX12ImGuiRender,
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
			commands.emplace_back(CommandSetViewport{ x, y, width, height });
		}
		void SetState(const GraphicsState& state)
		{
			commands.emplace_back(CommandSetState{ state });
		}
		void BindPipeline(PipelineHandle pso)
		{
			commands.emplace_back(CommandBindPipeline{ pso });
		}
		void BindInputLayout(InputLayoutHandle layout)
		{
			commands.emplace_back(CommandBindInputLayout{ layout });
		}
		void BindVertexBuffer(std::uint32_t slot, BufferHandle buffer, std::uint32_t strideBytes = 0, std::uint32_t offsetBytes = 0)
		{
			commands.emplace_back(CommandBindVertexBuffer{ slot, buffer, strideBytes, offsetBytes });
		}
		void BindIndexBuffer(BufferHandle buffer, IndexType indexType, std::uint32_t offsetBytes = 0)
		{
			commands.emplace_back(CommandBindIndexBuffer{ buffer, indexType, offsetBytes });
		}
		void BindTexture2D(std::uint32_t slot, TextureHandle texture)
		{
			commands.emplace_back(CommnadBindTexture2D{ slot, texture });
		}
		void BindTextureCube(std::uint32_t slot, TextureHandle texture)
		{
			commands.emplace_back(CommandBindTextureCube{ slot, texture });
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
		void SetUniformMat4(std::string_view name, const std::array<float, 16>& v)
		{
			commands.emplace_back(CommandUniformMat4{ std::string(name), v });
		}
		void SetConstants(std::uint32_t slot, std::span<const std::byte> bytes)
		{
			if (bytes.size() > 256)
			{
				throw std::runtime_error("CommandList::SetConstants: payload too large (max 256 bytes)");
			}

			CommandSetConstants cmd{};
			cmd.slot = slot;
			cmd.size = static_cast<std::uint32_t>(bytes.size());
			if (!bytes.empty())
				std::memcpy(cmd.data.data(), bytes.data(), bytes.size());
			commands.emplace_back(std::move(cmd));
		}
		void DrawIndexed(
			std::uint32_t indexCount,
			IndexType indexType,
			std::uint32_t firstIndex = 0,
			int baseVertex = 0,
			uint32_t instanceCount = 1,
			uint32_t firstInstance = 0)
		{
			commands.emplace_back(CommandDrawIndexed{ indexCount, indexType, firstIndex, baseVertex, instanceCount, firstInstance });
		}
		void Draw(
			std::uint32_t vertexCount,
			std::uint32_t firstVertex = 0,
			uint32_t instanceCount = 1,
			uint32_t firstInstance = 0)
		{
			commands.emplace_back(CommandDraw{ vertexCount, firstVertex, instanceCount, firstInstance });
		}
		void BindStructuredBufferSRV(std::uint32_t slot, BufferHandle buffer)
		{
			commands.emplace_back(CommandBindStructuredBufferSRV{ slot, buffer });
		}
		void DX12ImGuiRender(const void* drawData)
		{
			commands.emplace_back(CommandDX12ImGuiRender{ drawData });
		}
	};

	// ------------------------ RHI interfaces ------------------------ //

	class IRHISwapChain
	{
	public:
		virtual ~IRHISwapChain() = default;
		virtual SwapChainDesc GetDesc() const = 0;
		virtual FrameBufferHandle GetCurrentBackBuffer() const = 0;
		virtual void Present() = 0;
	};

	class IRHIDevice
	{
	public:
		virtual ~IRHIDevice() = default;

		virtual Backend GetBackend() const noexcept = 0;

		virtual std::string_view GetName() const = 0;

		// Optional UI hooks (Dear ImGui). Default is no-op.
		virtual void InitImGui(void* hwnd, int framesInFlight, Format rtvFormat) { (void)hwnd; (void)framesInFlight; (void)rtvFormat; }
		virtual void ImGuiNewFrame() {}
		virtual void ShutdownImGui() {}

		// Textures
		virtual TextureHandle CreateTexture2D(Extent2D extendt, Format format) = 0;
		virtual TextureHandle CreateTextureCube(Extent2D extent, Format format) = 0;
		virtual void DestroyTexture(TextureHandle texture) noexcept = 0;

		// Framebuffers
		virtual FrameBufferHandle CreateFramebuffer(TextureHandle color, TextureHandle depth) = 0;
		virtual FrameBufferHandle CreateFramebufferCubeFace(TextureHandle colorCube, std::uint32_t faceIndex, TextureHandle depth) = 0;
		virtual void DestroyFramebuffer(FrameBufferHandle frameBuffer) noexcept = 0;

		// Buffers
		virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
		virtual void UpdateBuffer(BufferHandle buffer, std::span<const std::byte> data, std::size_t offsetBytes = 0) = 0;
		virtual void DestroyBuffer(BufferHandle buffer) noexcept = 0;

		// Input layouts (API-neutral)
		virtual InputLayoutHandle CreateInputLayout(const InputLayoutDesc& desc) = 0;
		virtual void DestroyInputLayout(InputLayoutHandle layout) noexcept = 0;

		// Shaders and Pipelines
		virtual ShaderHandle CreateShader(ShaderStage stage, std::string_view debugName, std::string_view sourceOrBytecode) = 0;
		virtual void DestroyShader(ShaderHandle shader) noexcept = 0;

		virtual PipelineHandle CreatePipeline(std::string_view debugName, ShaderHandle vertexShader, ShaderHandle pixelShader) = 0;
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
		FrameBufferHandle GetCurrentBackBuffer() const override
		{
			return FrameBufferHandle{ 0 };
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

		Backend GetBackend() const noexcept override
		{
			return Backend::Null;
		}

		TextureHandle CreateTexture2D(Extent2D, Format) override
		{
			TextureHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		TextureHandle CreateTextureCube(Extent2D, Format) override
		{
			TextureHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void DestroyTexture(TextureHandle) noexcept override {}

		FrameBufferHandle CreateFramebuffer(TextureHandle, TextureHandle) override
		{
			FrameBufferHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		FrameBufferHandle CreateFramebufferCubeFace(TextureHandle, std::uint32_t, TextureHandle) override
		{
			FrameBufferHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void DestroyFramebuffer(FrameBufferHandle) noexcept override {}

		BufferHandle CreateBuffer(const BufferDesc&) override
		{
			BufferHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void UpdateBuffer(BufferHandle, std::span<const std::byte>, std::size_t) override {}
		void DestroyBuffer(BufferHandle) noexcept override {}

		InputLayoutHandle CreateInputLayout(const InputLayoutDesc&) override
		{
			InputLayoutHandle handle{};
			handle.id = ++nextId_;
			return handle;
		}
		void DestroyInputLayout(InputLayoutHandle) noexcept override {}

		virtual void BindInputLayout(InputLayoutHandle layout) {}
		virtual void BindVertexBuffer(std::uint32_t slot, BufferHandle vertexBuffer, std::uint32_t strideBytes, std::uint32_t offsetBytes) {}
		virtual void BindIndexBuffer(BufferHandle indexBuffer, IndexType indexTtype, std::uint32_t offsetBytes) {}

		ShaderHandle CreateShader(ShaderStage stage, std::string_view, std::string_view) override
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