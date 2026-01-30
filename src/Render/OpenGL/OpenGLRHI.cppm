module;

#include <GL/glew.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

export module core:rhi_gl;

import :rhi;

export namespace rhi
{
	// This keeps the GL backend independent from a specific windowing library (GLFW/SDL/etc.).
	struct GLSwapChainHooks
	{
		std::function<void()> present;
		std::function<Extent2D()> getDrawableExtent;
		std::function<void(bool)> setVsync;
	};

	struct GLDeviceDesc
	{
		bool enableDebug{ false };
	};

	struct GLSwapChainDesc
	{
		SwapChainDesc base{};
		GLSwapChainHooks hooks{};
	};

	std::unique_ptr<IRHIDevice> CreateGLDevice(GLDeviceDesc desc = {});
	std::unique_ptr<IRHISwapChain> CreateGLSwapChain(IRHIDevice& device, GLSwapChainDesc desc);
}

export namespace RHI_GL_UTILS
{
	inline void ThrowIfShaderCompilationFailed(GLuint shader, std::string_view debugName)
	{
		GLint success = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
		if (success == GL_FALSE)
		{
			GLint logLength = 0;
			glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
			std::string infoLog;
			infoLog.resize(std::max(0, logLength));
			if (logLength > 0)
			{
				glGetShaderInfoLog(shader, logLength, nullptr, infoLog.data());
			}
			throw std::runtime_error(std::string("OpenGL shader compile failed (") + std::string(debugName) + "): " + infoLog);
		}
	}

	inline void ThrowIfProgramLinkFailed(GLuint program, std::string_view debugName)
	{
		GLint success = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &success);
		if (success == GL_FALSE)
		{
			GLint logLength = 0;
			glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
			std::string infoLog;
			infoLog.resize(std::max(0, logLength));
			if (logLength > 0)
			{
				glGetProgramInfoLog(program, logLength, nullptr, infoLog.data());
			}
			throw std::runtime_error(std::string("OpenGL program link failed (") + std::string(debugName) + "): " + infoLog);
		}
	}
}

namespace
{
	static std::uint32_t DefaultLocation(rhi::VertexSemantic sem, std::uint8_t semIndex) noexcept
	{
		switch (sem)
		{
		case rhi::VertexSemantic::Position:
			return 0;
		case rhi::VertexSemantic::Normal:
			return 1;
		case rhi::VertexSemantic::TexCoord:
			return 2 + static_cast<std::uint32_t>(semIndex) * 4u;
		case rhi::VertexSemantic::Color:
			return 3;
		case rhi::VertexSemantic::Tangent:
			return 4;
		default:
			return 0;
		}
	}

	static GLenum ToGLInternalFormat(rhi::Format format)
	{
		switch (format)
		{
		case rhi::Format::RGBA8_UNORM:
			return GL_RGBA8;
		case rhi::Format::BGRA8_UNORM:
			return GL_RGBA8;
		case rhi::Format::D32_FLOAT:
			return GL_DEPTH_COMPONENT32F;
		default:
			return GL_RGBA8;
		}
	}

	static GLenum ToGLBaseFormat(rhi::Format format)
	{
		switch (format)
		{
		case rhi::Format::RGBA8_UNORM:
			return GL_RGBA;
		case rhi::Format::BGRA8_UNORM:
			return GL_BGRA;
		case rhi::Format::D32_FLOAT:
			return GL_DEPTH_COMPONENT;
		default:
			return GL_RGBA;
		}
	}

	static GLenum ToGLType(rhi::Format format)
	{
		switch (format)
		{
		case rhi::Format::RGBA8_UNORM:
		case rhi::Format::BGRA8_UNORM:
			return GL_UNSIGNED_BYTE;
		case rhi::Format::D32_FLOAT:
			return GL_FLOAT;
		default:
			return GL_UNSIGNED_BYTE;
		}
	}

	static GLenum ToGLCompareOp(rhi::CompareOp op)
	{
		switch (op)
		{
		case rhi::CompareOp::Never:
			return GL_NEVER;
		case rhi::CompareOp::Less:
			return GL_LESS;
		case rhi::CompareOp::Equal:
			return GL_EQUAL;
		case rhi::CompareOp::LessEqual:
			return GL_LEQUAL;
		case rhi::CompareOp::Greater:
			return GL_GREATER;
		case rhi::CompareOp::NotEqual:
			return GL_NOTEQUAL;
		case rhi::CompareOp::GreaterEqual:
			return GL_GEQUAL;
		case rhi::CompareOp::Always:
			return GL_ALWAYS;
		default:

			return GL_LEQUAL;
		}
	}

	static GLenum ToGLCullMode(rhi::CullMode mode)
	{
		switch (mode)
		{

		case rhi::CullMode::Front:
			return GL_FRONT;
		case rhi::CullMode::Back:
			return GL_BACK;
		default:
			return GL_BACK;
		}
	}

	static GLenum ToGLFrontFace(rhi::FrontFace face)
	{
		return (face == rhi::FrontFace::Clockwise) ? GL_CW : GL_CCW;
	}

	static GLenum BufferTargetFor(rhi::BufferBindFlag bindFlag)
	{
		switch (bindFlag)
		{
		case rhi::BufferBindFlag::VertexBuffer:
			return GL_ARRAY_BUFFER;
		case rhi::BufferBindFlag::IndexBuffer:
			return GL_ELEMENT_ARRAY_BUFFER;
		case rhi::BufferBindFlag::ConstantBuffer:
			return GL_UNIFORM_BUFFER;
		case rhi::BufferBindFlag::UniformBuffer:
			return GL_UNIFORM_BUFFER;
		case rhi::BufferBindFlag::StructuredBuffer:
			return GL_SHADER_STORAGE_BUFFER;
		default:
			return GL_ARRAY_BUFFER;
		}
	}

	static GLenum BufferUsageFor(rhi::BufferUsageFlag usage)
	{
		switch (usage)
		{
		case rhi::BufferUsageFlag::Static:
			return GL_STATIC_DRAW;
		case rhi::BufferUsageFlag::Dynamic:
			return GL_DYNAMIC_DRAW;
		case rhi::BufferUsageFlag::Stream:
			return GL_STREAM_DRAW;
		default:
			return GL_STATIC_DRAW;
		}
	}

	static GLenum ToGLIndexType(rhi::IndexType t)
	{
		return (t == rhi::IndexType::UINT16) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
	}

	static std::uint32_t IndexSizeBytes(rhi::IndexType t)
	{
		return (t == rhi::IndexType::UINT16) ? 2u : 4u;
	}

	static std::string EnsureGLSLVersion(std::string_view source)
	{
		constexpr std::string_view kVersion = "#version";
		std::string_view vSource = source;
		while (!vSource.empty() && (
			vSource.front() == ' '
			|| vSource.front() == '\t'
			|| vSource.front() == '\r'
			|| vSource.front() == '\n')
			)
		{
			vSource.remove_prefix(1);
		}

		if (vSource.rfind(kVersion, 0) == 0)
		{
			return std::string(source);
		}

		std::string outStr;
		outStr.reserve(source.size() + 32);
		outStr += "#version 330 core\n";
		outStr += source;
		return outStr;
	}

	struct GLAttrib
	{
		GLuint location{};
		GLint componentCount{};
		GLenum type{};
		GLboolean normalized{};
		GLuint offsetBytes{};
		std::uint32_t inputSlot{};
	};

	struct GLInputLayout
	{
		std::uint32_t strideBytes{};
		std::vector<GLAttrib> attribs;
		std::string debugName;
	};

	static GLenum ToGLShaderStage(rhi::ShaderStage stage)
	{
		switch (stage)
		{
		case rhi::ShaderStage::Vertex:
			return GL_VERTEX_SHADER;
		case rhi::ShaderStage::Pixel:
			return GL_FRAGMENT_SHADER;
		case rhi::ShaderStage::Geometry:
			return GL_GEOMETRY_SHADER;
		case rhi::ShaderStage::Compute:
			return GL_COMPUTE_SHADER;
		default:
			return GL_FRAGMENT_SHADER;
		}
	}

	static void VertexFormatToGL(rhi::VertexFormat format, GLint& outComponents, GLenum& outType)
	{
		switch (format)
		{
		case rhi::VertexFormat::R32G32B32_FLOAT:
			outComponents = 3;
			outType = GL_FLOAT;
			break;
		case rhi::VertexFormat::R32G32_FLOAT:
			outComponents = 2;
			outType = GL_FLOAT;
			outType = GL_FLOAT;
			break;
		case rhi::VertexFormat::R32G32B32A32_FLOAT:
			outComponents = 4;
			outType = GL_FLOAT;
			break;
		case rhi::VertexFormat::R8G8B8A8_UNORM:
			outComponents = 4;
			outType = GL_UNSIGNED_BYTE;
			break;
		default:
			outComponents = 4;
			outType = GL_FLOAT;
			break;
		}
	}

	struct VaoKey
	{
		std::uint32_t layoutId{};
		std::uint32_t vbId{};
		std::uint32_t vbOffset{};
		std::uint32_t vbStride{};
		std::uint32_t ibId{};
		std::uint32_t ibOffset{};
		rhi::IndexType indexType{ rhi::IndexType::UINT16 };

		friend bool operator==(const VaoKey& a, const VaoKey& b) noexcept
		{
			return a.layoutId == b.layoutId
				&& a.vbId == b.vbId
				&& a.vbOffset == b.vbOffset
				&& a.vbStride == b.vbStride
				&& a.ibId == b.ibId
				&& a.ibOffset == b.ibOffset
				&& a.indexType == b.indexType;
		}
	};

	struct VaoKeyHash
	{
		std::size_t operator()(const VaoKey& key) const noexcept
		{
			std::size_t hash = 1469598103934665603ull;
			auto mix = [&](std::uint64_t val)
				{
					hash ^= static_cast<std::size_t>(val) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
				};
			mix(key.layoutId);
			mix(key.vbId);
			mix(key.vbOffset);
			mix(key.vbStride);
			mix(key.ibId);
			mix(key.ibOffset);
			mix(static_cast<std::uint32_t>(key.indexType));
			return hash;
		}
	};

	struct GLFence
	{
		GLsync sync{ nullptr };
		bool signaled{ false };
	};

	struct VertexBufferState
	{
		rhi::BufferHandle buffer{};
		std::uint32_t strideBytes{ 0 };
		std::uint32_t offsetBytes{ 0 };
	};
}

namespace rhi
{
	class GLSwapChain final : public IRHISwapChain
	{
	public:
		GLSwapChain(GLSwapChainDesc desc)
			: desc_(std::move(desc))
		{
			if (desc_.hooks.setVsync)
			{
				desc_.hooks.setVsync(desc_.base.vsync);
			}
		}
		~GLSwapChain() override = default;

		SwapChainDesc GetDesc() const override
		{
			SwapChainDesc outDesc = desc_.base;
			if (desc_.hooks.getDrawableExtent)
			{
				outDesc.extent = desc_.hooks.getDrawableExtent();
			}
			return outDesc;
		}

		FrameBufferHandle GetCurrentBackBuffer() const override
		{
			return FrameBufferHandle{ 0 };
		}

		void Present() override
		{
			if (desc_.hooks.present)
			{
				desc_.hooks.present();
			}
			else
			{
				glFlush();
			}
		}

	private:
		GLSwapChainDesc desc_{};
	};

	class GLDevice final : public IRHIDevice
	{
	public:
		explicit GLDevice([[maybe_unused]] GLDeviceDesc desc)
			: desc_(std::move(desc))
		{
			const GLubyte* vendor = glGetString(GL_VENDOR);
			const GLubyte* renderer = glGetString(GL_RENDERER);
			const GLubyte* version = glGetString(GL_VERSION);

			name_.reserve(256);
			name_ += "OpenGL";
			if (vendor)
			{
				name_ += " | ";
				name_ += reinterpret_cast<const char*>(vendor);
			}
			if (renderer)
			{
				name_ += " | ";
				name_ += reinterpret_cast<const char*>(renderer);
			}
			if (version)
			{
				name_ += " | ";
				name_ += reinterpret_cast<const char*>(version);
			}
		}

		~GLDevice()
		{
			InvalidateVaoCache();

			for (auto& [_, fence] : fences_)
			{
				if (fence.sync)
				{
					glDeleteSync(fence.sync);
				}
			}
			fences_.clear();
		}

		Backend GetBackend() const noexcept override
		{
			return Backend::OpenGL;
		}

		std::string_view GetName() const override
		{
			return name_;
		}

		// ---------------- Textures ----------------
		TextureHandle CreateTexture2D(Extent2D extent, Format format) override
		{
			GLuint textureId = 0;
			glGenTextures(1, &textureId);
			glBindTexture(GL_TEXTURE_2D, textureId);

			if (format == rhi::Format::D32_FLOAT)
			{
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			}
			else
			{
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			}
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			const GLenum internalFormat = ToGLInternalFormat(format);
			const GLenum baseFormat = ToGLBaseFormat(format);
			const GLenum type = ToGLType(format);

			glTexImage2D(
				GL_TEXTURE_2D
				, 0
				, static_cast<GLint>(internalFormat)
				, static_cast<GLsizei>(extent.width)
				, static_cast<GLsizei>(extent.height)
				, 0
				, baseFormat
				, type
				, nullptr);

			glBindTexture(GL_TEXTURE_2D, 0);
			return TextureHandle{ static_cast<std::uint32_t>(textureId) };
		}

		void DestroyTexture(TextureHandle texture) noexcept override
		{
			GLuint textureId = static_cast<GLuint>(texture.id);
			if (textureId != 0)
			{
				glDeleteTextures(1, &textureId);
			}
		}

		// ---------------- Framebuffers ----------------
		FrameBufferHandle CreateFramebuffer(TextureHandle color, TextureHandle depth) override
		{
			GLuint framebufferId = 0;
			glGenFramebuffers(1, &framebufferId);
			glBindFramebuffer(GL_FRAMEBUFFER, framebufferId);

			if (color.id != 0)
			{
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(color.id), 0);
				GLenum drawBuffers = GL_COLOR_ATTACHMENT0;
				glDrawBuffers(1, &drawBuffers);
			}
			else
			{
				glDrawBuffer(GL_NONE);
				glReadBuffer(GL_NONE);
			}

			if (depth.id != 0)
			{
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, static_cast<GLuint>(depth.id), 0);
			}

			const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);

			if (status != GL_FRAMEBUFFER_COMPLETE)
			{
				glDeleteFramebuffers(1, &framebufferId);
				throw std::runtime_error("Failed to create framebuffer: incomplete framebuffer");
			}

			return FrameBufferHandle{ static_cast<std::uint32_t>(framebufferId) };
		}

		void DestroyFramebuffer(FrameBufferHandle framebuffer) noexcept override
		{
			GLuint framebufferId = static_cast<GLuint>(framebuffer.id);
			if (framebufferId != 0)
			{
				glDeleteFramebuffers(1, &framebufferId);
			}
		}

		// ---------------- Buffers ----------------
		BufferHandle CreateBuffer(const BufferDesc& desc) override
		{
			GLuint bufferId = 0;
			glGenBuffers(1, &bufferId);

			const GLenum target = BufferTargetFor(desc.bindFlag);
			bufferTargets_[bufferId] = target;

			glBindBuffer(target, bufferId);
			glBufferData(target, static_cast<GLsizeiptr>(desc.sizeInBytes), nullptr, BufferUsageFor(desc.usageFlag));
			glBindBuffer(target, 0);

			InvalidateVaoCache();
			return BufferHandle{ static_cast<std::uint32_t>(bufferId) };
		}

		void UpdateBuffer(BufferHandle buffer, std::span<const std::byte> data, std::size_t offsetBytes) override
		{
			const GLuint bufferId = static_cast<GLuint>(buffer.id);
			if (bufferId == 0 || data.empty())
			{
				return;
			}

			const GLenum target = BufferTargetForId(bufferId);
			glBindBuffer(target, bufferId);
			glBufferSubData(target, static_cast<GLintptr>(offsetBytes), static_cast<GLsizeiptr>(data.size()), data.data());
			glBindBuffer(target, 0);
		}

		void DestroyBuffer(BufferHandle buffer) noexcept override
		{
			GLuint bufferId = static_cast<GLuint>(buffer.id);
			if (bufferId != 0)
			{
				glDeleteBuffers(1, &bufferId);
				bufferTargets_.erase(bufferId);
			}
			InvalidateVaoCache();
		}
		// ---------------- Input Layouts ----------------
		InputLayoutHandle CreateInputLayout(const InputLayoutDesc& desc) override
		{
			GLInputLayout glLayout{};
			glLayout.strideBytes = desc.strideBytes;
			glLayout.debugName = desc.debugName;
			glLayout.attribs.reserve(desc.attributes.size());

			for (const auto& attribute : desc.attributes)
			{
				if (attribute.inputSlot != 0)
				{
					throw std::runtime_error("OpenGLRHI: multiple vertex input slots are not supported yet (inputSlot != 0).");
				}

				GLint comps = 0;
				GLenum type = GL_FLOAT;
				VertexFormatToGL(attribute.format, comps, type);

				GLAttrib out{};
				out.location = static_cast<GLuint>(DefaultLocation(attribute.semantic, attribute.semanticIndex));
				out.componentCount = comps;
				out.type = type;
				out.normalized = (attribute.normalized ? GL_TRUE : GL_FALSE);
				out.offsetBytes = static_cast<GLuint>(attribute.offsetBytes);
				out.inputSlot = attribute.inputSlot;

				glLayout.attribs.push_back(out);
			}

			const std::uint32_t layoutId = static_cast<std::uint32_t>(inputLayouts_.size()) + 1u;
			inputLayouts_.push_back(std::move(glLayout));

			InvalidateVaoCache();
			return rhi::InputLayoutHandle{ layoutId };
		}

		void DestroyInputLayout(InputLayoutHandle layout) noexcept override
		{
			const std::uint32_t layoutId = layout.id;
			if (layoutId == 0)
			{
				return;
			}

			const std::size_t idx = static_cast<std::size_t>(layoutId - 1);
			if (idx < inputLayouts_.size())
			{
				inputLayouts_[idx] = GLInputLayout{};
			}

			InvalidateVaoCache();
		}

		// ---------------- Shaders / Pipeline ----------------
		ShaderHandle CreateShader(
			ShaderStage stage,
			std::string_view debugName,
			std::string_view sourceOrBytecode) override
		{
			const GLenum shaderType = ToGLShaderStage(stage);
			GLuint shaderId = glCreateShader(shaderType);
			if (shaderId == 0)
			{
				throw std::runtime_error("Failed to create (GL) shader object (" + std::string(debugName) + ")");
			}

			std::string sourceWithVersion = EnsureGLSLVersion(sourceOrBytecode);
			const char* sourceCStr = sourceWithVersion.c_str();
			GLint length = static_cast<GLint>(sourceWithVersion.size());
			glShaderSource(shaderId, 1, &sourceCStr, &length);
			glCompileShader(shaderId);

			RHI_GL_UTILS::ThrowIfShaderCompilationFailed(shaderId, debugName);
			return ShaderHandle{ static_cast<std::uint32_t>(shaderId) };
		}

		void DestroyShader(ShaderHandle shader) noexcept override
		{
			GLuint shaderId = static_cast<GLuint>(shader.id);
			if (shaderId != 0)
			{
				glDeleteShader(shaderId);
			}
		}

		PipelineHandle CreatePipeline(std::string_view debugName, ShaderHandle vertexShader, ShaderHandle pixelShader) override
		{
			GLuint programId = glCreateProgram();
			if (programId == 0)
			{
				throw std::runtime_error("Failed to create (GL) program object (" + std::string(debugName) + ")");
			}

			glAttachShader(programId, static_cast<GLuint>(vertexShader.id));
			glAttachShader(programId, static_cast<GLuint>(pixelShader.id));
			glLinkProgram(programId);

			RHI_GL_UTILS::ThrowIfProgramLinkFailed(programId, debugName);

			glDetachShader(programId, static_cast<GLuint>(vertexShader.id));
			glDetachShader(programId, static_cast<GLuint>(pixelShader.id));

			return PipelineHandle{ static_cast<std::uint32_t>(programId) };
		}

		void DestroyPipeline(PipelineHandle pso) noexcept override
		{
			GLuint programId = static_cast<GLuint>(pso.id);
			if (programId != 0)
			{
				glDeleteProgram(programId);
			}
		}

		// ---------------- Command submission ----------------
		void SubmitCommandList(CommandList&& commandList) override
		{
			for (const auto& command : commandList.commands)
			{
				std::visit([this](auto&& cmd) { ExecuteOnce(cmd); }, command);
			}
		}

		// ---------------- Texture descriptors ----------------
		TextureDescIndex AllocateTextureDesctiptor(TextureHandle texture) override
		{
			if (!freeTextureDescIndices_.empty())
			{
				const TextureDescIndex index = freeTextureDescIndices_.back();
				freeTextureDescIndices_.pop_back();
				textureDescriptions_[index] = texture;
				return index;

			}

			const TextureDescIndex index = static_cast<TextureDescIndex>(textureDescriptions_.size());
			textureDescriptions_.push_back(texture);
			return index;
		}

		void UpdateTextureDescriptor(TextureDescIndex index, TextureHandle texture) override
		{
			if (index == 0)
			{
				return;
			}
			const size_t vecIndex = static_cast<size_t>(index);
			if (vecIndex >= textureDescriptions_.size())
			{
				textureDescriptions_.resize(vecIndex + 1);
			}
			textureDescriptions_[vecIndex] = texture;
		}

		void FreeTextureDescriptor(TextureDescIndex index) noexcept override
		{
			if (index == 0)
			{
				return;
			}
			const size_t vecIndex = static_cast<size_t>(index);
			if (vecIndex < textureDescriptions_.size())
			{
				textureDescriptions_[vecIndex] = TextureHandle{};
				freeTextureDescIndices_.push_back(index);
			}
		}

		// ---------------- Fences ----------------
		FenceHandle CreateFence(bool signaled = false) override
		{
			const std::uint32_t fenceId = ++nextFenceId_;
			GLFence fence;
			fence.signaled = signaled;

			if (!signaled)
			{
				fence.sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
				glFlush();
			}

			fences_[fenceId] = fence;
			return FenceHandle{ fenceId };
		}

		void DestroyFence(FenceHandle fence) noexcept override
		{
			const std::uint32_t fenceId = fence.id;
			if (fenceId == 0)
			{
				return;
			}

			if (auto it = fences_.find(fenceId); it != fences_.end())
			{
				if (it->second.sync)
				{
					glDeleteSync(it->second.sync);
				}
				fences_.erase(it);
			}
		}

		void SignalFence(FenceHandle fence) override
		{
			GLFence* ptrFence = GetFence(fence);
			if (!ptrFence)
			{
				return;
			}

			if (ptrFence->sync)
			{
				glDeleteSync(ptrFence->sync);
			}

			ptrFence->sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			ptrFence->signaled = false;
			glFlush();
		}

		void WaitFence(FenceHandle fence) override
		{
			GLFence* ptrFence = GetFence(fence);
			if (!ptrFence)
			{
				return;
			}

			if (ptrFence->signaled)
			{
				return;
			}

			if (!ptrFence->sync)
			{
				ptrFence->signaled = true;
				return;
			}

			while (true)
			{
				const GLenum res = glClientWaitSync(ptrFence->sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000);
				if (res == GL_ALREADY_SIGNALED || res == GL_CONDITION_SATISFIED)
				{
					break;
				}
			}

			glDeleteSync(ptrFence->sync);
			ptrFence->sync = nullptr;
			ptrFence->signaled = true;
		}

		bool IsFenceSignaled(FenceHandle fence) override
		{
			GLFence* ptrFence = GetFence(fence);
			if (!ptrFence)
			{
				return true;
			}

			if (ptrFence->signaled)
			{
				return true;
			}

			if (!ptrFence->sync)
			{
				return true;
			}

			const GLenum res = glClientWaitSync(ptrFence->sync, 0, 0);
			if (res == GL_ALREADY_SIGNALED || res == GL_CONDITION_SATISFIED)
			{
				glDeleteSync(ptrFence->sync);
				ptrFence->sync = nullptr;
				ptrFence->signaled = true;
				return true;
			}
			return false;
		}

	private:
		GLenum BufferTargetForId(GLuint bufferId) const
		{
			if (auto it = bufferTargets_.find(bufferId); it != bufferTargets_.end())
			{
				return it->second;
			}
			return GL_ARRAY_BUFFER;
		}

		const GLInputLayout* GetLayout(rhi::InputLayoutHandle handle) const
		{
			const std::size_t idx = static_cast<std::size_t>(handle.id - 1);
			if (handle.id == 0 || idx >= inputLayouts_.size())
			{
				return nullptr;
			}
			if (inputLayouts_[idx].strideBytes == 0 && inputLayouts_[idx].attribs.empty())
			{
				return nullptr;
			}
			return &inputLayouts_[idx];
		}

		TextureHandle ResolveTextureDesc(TextureDescIndex index)
		{
			const std::size_t idx = static_cast<std::size_t>(index);
			if (index == 0 || idx >= textureDescriptions_.size())
			{
				return TextureHandle{};
			}
			return textureDescriptions_[idx];
		}

		GLFence* GetFence(rhi::FenceHandle handle)
		{
			if (handle.id == 0)
			{
				return nullptr;
			}
			if (auto it = fences_.find(handle.id); it != fences_.end())
			{
				return &it->second;
			}
			return nullptr;
		}

		void InvalidateVaoCache()
		{
			for (auto& [_, vao] : vaoCache_)
			{
				if (vao != 0)
				{
					glDeleteVertexArrays(1, &vao);
				}
			}
			vaoCache_.clear();
			boundVao_ = 0;
		}

		GLuint GetOrCreateVAO(bool requireIndexBuffer)
		{
			// Currently: support only slot 0.
			if (currentLayout_.id == 0)
			{
				throw std::runtime_error("OpenGLRHI: Draw called without InputLayout bound.");
			}
			if (vertexBuffer_[0].buffer.id == 0)
			{
				throw std::runtime_error("OpenGLRHI: Draw called without VertexBuffer bound.");
			}
			if (requireIndexBuffer && indexBuffer_.buffer.id == 0)
			{
				throw std::runtime_error("OpenGLRHI: DrawIndexed called without IndexBuffer bound.");
			}

			const GLInputLayout* layout = GetLayout(currentLayout_);
			if (!layout)
			{
				throw std::runtime_error("OpenGLRHI: invalid InputLayout handle.");
			}

			const GLuint vbId = static_cast<GLuint>(vertexBuffer_[0].buffer.id);
			const GLuint ibId = static_cast<GLuint>(indexBuffer_.buffer.id);

			VaoKey key{};
			key.layoutId = currentLayout_.id;
			key.vbId = vbId;
			key.vbOffset = vertexBuffer_[0].offsetBytes;
			key.vbStride = (vertexBuffer_[0].strideBytes != 0) ? vertexBuffer_[0].strideBytes : layout->strideBytes;
			key.ibId = requireIndexBuffer ? ibId : 0u;
			key.ibOffset = requireIndexBuffer ? indexBuffer_.offsetBytes : 0u;
			key.indexType = indexBuffer_.indexType;

			if (auto it = vaoCache_.find(key); it != vaoCache_.end())
			{
				return it->second;
			}

			GLuint vao = 0;
			glGenVertexArrays(1, &vao);
			glBindVertexArray(vao);

			glBindBuffer(GL_ARRAY_BUFFER, vbId);
			if (requireIndexBuffer)
			{
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibId);
			}

			const GLsizei stride = static_cast<GLsizei>(key.vbStride);

			for (const auto& attribute : layout->attribs)
			{
				const GLuint loc = attribute.location;
				glEnableVertexAttribArray(loc);

				const std::uintptr_t ptrOffset = static_cast<std::uintptr_t>(key.vbOffset) + static_cast<std::uintptr_t>(attribute.offsetBytes);
				glVertexAttribPointer(
					loc,
					attribute.componentCount,
					attribute.type,
					attribute.normalized,
					stride,
					reinterpret_cast<const void*>(ptrOffset));
			}

			glBindVertexArray(0);

			vaoCache_.emplace(key, vao);
			return vao;
		}

		// -------- State / uniforms --------
		void ApplyState(const GraphicsState& state)
		{
			// Depth state
			if (state.depth.testEnable)
			{
				glEnable(GL_DEPTH_TEST);
			}
			else
			{
				glDisable(GL_DEPTH_TEST);
			}

			glDepthMask(state.depth.writeEnable ? GL_TRUE : GL_FALSE);
			glDepthFunc(ToGLCompareOp(state.depth.depthCompareOp));

			// Raster
			if (state.rasterizer.cullMode != rhi::CullMode::None)
			{
				glEnable(GL_CULL_FACE);
				glCullFace(ToGLCullMode(state.rasterizer.cullMode));
			}
			else
			{
				glDisable(GL_CULL_FACE);
			}
			glFrontFace(ToGLFrontFace(state.rasterizer.frontFace));

			// Blend (simple)
			if (state.blend.enable)
			{
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glBlendEquation(GL_FUNC_ADD);
			}
			else
			{
				glDisable(GL_BLEND);
			}
		}

		GLint GetUniformLocationCached(const std::string& name)
		{
			if (currentProgram_ == 0)
			{
				return -1;
			}

			auto& uCache = uniformLocationCache_[currentProgram_];
			if (auto it = uCache.find(name); it != uCache.end())
			{
				return it->second;
			}

			GLint loc = glGetUniformLocation(currentProgram_, name.c_str());
			uCache.emplace(name, loc);
			return loc;
		}

		void SetUniformIntImpl(const std::string& name, int value)
		{
			if (currentProgram_ == 0)
			{
				return;
			}
			GLint location = glGetUniformLocation(currentProgram_, name.c_str());
			if (location != -1)
			{
				glUniform1i(location, value);
			}
		}

		void SetUniformFloat4Impl(const std::string& name, const std::array<float, 4>& value)
		{
			if (currentProgram_ == 0)
			{
				return;
			}
			GLint location = glGetUniformLocation(currentProgram_, name.c_str());
			if (location != -1)
			{
				glUniform4f(location, value[0], value[1], value[2], value[3]);
			}
		}

		void SetUniformMat4Impl(const std::string& name, const std::array<float, 16>& value)
		{
			if (currentProgram_ == 0)
			{
				return;
			}
			GLint location = glGetUniformLocation(currentProgram_, name.c_str());
			if (location != -1)
			{
				glUniformMatrix4fv(location, 1, GL_FALSE, value.data());
			}
		}

		//----------------------   ExecuteOnce section -------------------------------//
		void ExecuteOnce(const CommandBeginPass& cmd)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(cmd.desc.frameBuffer.id));
			glViewport(0, 0, static_cast<GLsizei>(cmd.desc.extent.width), static_cast<GLsizei>(cmd.desc.extent.height));

			GLbitfield clearMask = 0;
			if (cmd.desc.clearDesc.clearColor)
			{
				clearMask |= GL_COLOR_BUFFER_BIT;
				glClearColor(
					cmd.desc.clearDesc.color[0],
					cmd.desc.clearDesc.color[1],
					cmd.desc.clearDesc.color[2],
					cmd.desc.clearDesc.color[3]);
			}
			if (cmd.desc.clearDesc.clearDepth)
			{
				clearMask |= GL_DEPTH_BUFFER_BIT;
				glClearDepth(static_cast<GLdouble>(cmd.desc.clearDesc.depth));
			}
			if (clearMask != 0)
			{
				glClear(clearMask);
			}
		}

		void ExecuteOnce(const CommandEndPass& /*cmd*/) {}

		void ExecuteOnce(const CommandSetViewport& cmd)
		{
			glViewport(cmd.x, cmd.y, cmd.width, cmd.height);
		}

		void ExecuteOnce(const CommandSetState& cmd)
		{
			ApplyState(cmd.state);
		}

		void ExecuteOnce(const CommandBindPipeline& cmd)
		{
			const GLuint programId = static_cast<GLuint>(cmd.pso.id);
			if (currentProgram_ != programId)
			{
				glUseProgram(programId);
				currentProgram_ = programId;
			}
		}

		void ExecuteOnce(const rhi::CommandBindInputLayout& cmd)
		{
			currentLayout_ = cmd.layout;
		}

		void ExecuteOnce(const rhi::CommandBindVertexBuffer& cmd)
		{
			if (cmd.slot != 0)
			{
				throw std::runtime_error("OpenGLRHI: only vertex buffer slot 0 is supported right now.");
			}
			vertexBuffer_[0].buffer = cmd.buffer;
			vertexBuffer_[0].strideBytes = cmd.strideBytes;
			vertexBuffer_[0].offsetBytes = cmd.offsetBytes;
		}

		void ExecuteOnce(const rhi::CommandBindIndexBuffer& cmd)
		{
			indexBuffer_.buffer = cmd.buffer;
			indexBuffer_.indexType = cmd.indexType;
			indexBuffer_.offsetBytes = cmd.offsetBytes;
		}

		void ExecuteOnce(const CommnadBindTextue2D& cmd)
		{
			glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(cmd.slot));
			glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(cmd.texture.id));
		}

		void ExecuteOnce(const CommandTextureDesc& cmd)
		{
			const TextureHandle& texture = ResolveTextureDesc(cmd.texture);
			glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(cmd.slot));
			glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture.id));
		}

		 		void ExecuteOnce(const CommandBindStructuredBufferSRV& /*cmd*/)
		{
			// Stage-1: OpenGL backend ignores structured-buffer SRVs.
		}

void ExecuteOnce(const CommandSetUniformInt& cmd)
		{
			SetUniformIntImpl(cmd.name, cmd.value);
		}

		void ExecuteOnce(const CommandUniformFloat4& cmd)
		{
			SetUniformFloat4Impl(cmd.name, cmd.value);
		}

		void ExecuteOnce(const CommandUniformMat4& cmd)
		{
			SetUniformMat4Impl(cmd.name, cmd.value);
		}

		void ExecuteOnce(const CommandSetConstants& /*cmd*/)
		{
			// OpenGL backend currently uses the name-based uniform path.
			// The constant-block command is primarily for DX12 (root CBV update).
		}

		void ExecuteOnce(const CommandDrawIndexed& cmd)
		{
			const GLuint vao = GetOrCreateVAO(true);
			if (boundVao_ != vao)
			{
				glBindVertexArray(vao);
				boundVao_ = vao;
			}

			const GLenum indexType = ToGLIndexType(cmd.indexType);
			const std::uintptr_t start = static_cast<std::uintptr_t>(indexBuffer_.offsetBytes)
				+ static_cast<std::uintptr_t>(cmd.firstIndex) * static_cast<std::uintptr_t>(IndexSizeBytes(cmd.indexType));

			if (cmd.baseVertex != 0)
			{
				glDrawElementsBaseVertex(
					GL_TRIANGLES,
					static_cast<GLsizei>(cmd.indexCount),
					indexType,
					reinterpret_cast<const void*>(start),
					cmd.baseVertex);
			}
			else
			{
				glDrawElements(
					GL_TRIANGLES,
					static_cast<GLsizei>(cmd.indexCount),
					indexType,
					reinterpret_cast<const void*>(start));
			}
		}

		void ExecuteOnce(const CommandDraw& cmd)
		{
			const GLuint vao = GetOrCreateVAO(false);
			if (boundVao_ != vao)
			{
				glBindVertexArray(vao);
				boundVao_ = vao;
			}

			glDrawArrays(GL_TRIANGLES, static_cast<GLint>(cmd.firstVertex), static_cast<GLsizei>(cmd.vertexCount));
		}

		//---------------------------------------------------------------------//
		GLDeviceDesc desc_{};
		std::string name_;

		// Buffer targets: buffer id -> GL target
		std::unordered_map<GLuint, GLenum> bufferTargets_{};
		// Input layouts (1-based handle id -> vector[id-1])
		std::vector<GLInputLayout> inputLayouts_{};

		// Descriptor indices (0 invalid)
		std::vector<TextureHandle> textureDescriptions_{ TextureHandle{} };
		std::vector<TextureDescIndex> freeTextureDescIndices_;

		// Fence storage
		std::uint32_t nextFenceId_{ 0 };
		std::unordered_map<std::uint32_t, GLFence> fences_{};

		// Current program + uniform location cache per program
		std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> uniformLocationCache_{};
		GLuint currentProgram_{ 0 };

		// Current bindings for VAO build
		rhi::InputLayoutHandle currentLayout_{};
		std::array<VertexBufferState, 1> vertexBuffer_{};
		struct {
			rhi::BufferHandle buffer{};
			rhi::IndexType indexType{ rhi::IndexType::UINT16 };
			std::uint32_t offsetBytes{ 0 };
		} indexBuffer_{};

		// VAO cache
		std::unordered_map<VaoKey, GLuint, VaoKeyHash> vaoCache_{};
		GLuint boundVao_{ 0 };
	};

	std::unique_ptr<IRHIDevice> CreateGLDevice(GLDeviceDesc desc)
	{
		return std::make_unique<GLDevice>(std::move(desc));
	}

	std::unique_ptr<IRHISwapChain> CreateGLSwapChain([[maybe_unused]] IRHIDevice& device, GLSwapChainDesc desc)
	{
		return std::make_unique<GLSwapChain>(std::move(desc));
	}
}
