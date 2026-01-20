module;

#include <GL/glew.h>
#include <functional>
#include <string>
#include <stdexcept>
#include <memory>
#include <variant>

export module core:rhi_gl;

import :rhi;

export namespace rhi
{
	// This keeps the GL backend independent from a specific windowing library (GLFW/SDL/etc.).
	struct GLSwapChainHooks
	{
		// Called by presenting the swap chain.
		std::function<void()> present;

		// Optional: query the drawable extent (e.g. window size).
		std::function<Extent2D()> getDrawableExtent;

		// Optional: set swap interval (vsync). Use this if your window library supports it.
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
	void ThrowIfShaderCompilationFailed(GLuint shader, std::string_view debugName)
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

	void ThrowIfProgramLinkFailed(GLuint program, std::string_view debugName)
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
	static GLenum ToGLInternalFormat(rhi::Format format)
	{
		switch (format)
		{
		case rhi::Format::RGBA8_UNORM:
			return GL_RGBA8;
		case rhi::Format::BGRA8_UNORM:
			return GL_RGBA8; // OpenGL does not have BGRA8 internal format, use RGBA8 instead.
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
		
		case rhi::CompareOp::Less:
			return GL_LESS;
		case rhi::CompareOp::LessEqual:
			return GL_LEQUAL;
		default:
			return GL_ALWAYS;
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

	static GLenum ToGLBufferTarget(rhi::BufferBindFlag bindFlag)
	{
		switch (bindFlag)
		{
		case rhi::BufferBindFlag::IndexBuffer:
			return GL_ELEMENT_ARRAY_BUFFER;
		case rhi::BufferBindFlag::ConstantBuffer: [[fallthrough]];
		case rhi::BufferBindFlag::UniformBuffer:
			return GL_UNIFORM_BUFFER;
		default:
			return GL_ARRAY_BUFFER;
		}
	}

	static GLenum ToGLUsage(rhi::BufferUsageFlag usageFlag)
	{
		switch (usageFlag)
		{
		case rhi::BufferUsageFlag::Static:
			return GL_STATIC_DRAW;
		case rhi::BufferUsageFlag::Stream:
			return GL_STREAM_DRAW;
		default:
			return GL_DYNAMIC_DRAW;
		}
	}

	static GLenum ToGLIndexType(rhi::IndexType indexType)
	{
		return (indexType == rhi::IndexType::UINT32) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
	}

	static GLenum GuessShaderType(std::string_view debugName)
	{
		if (debugName.starts_with("VS") || debugName.starts_with("VS_"))
		{
			return GL_VERTEX_SHADER;
		}
		if (debugName.starts_with("PS") || debugName.starts_with("PS_") ||
			debugName.starts_with("FS") || debugName.starts_with("FS_"))
		{
			return GL_FRAGMENT_SHADER;
		}

		return GL_FRAGMENT_SHADER;
	}

	static std::string EnsureGLSLVersion(std::string_view source)
	{
		const std::string versionDirective = "#version";
		if (source.find(versionDirective) != std::string::npos)
		{
			return std::string(source);
		}
		else
		{
			const std::string defaultVersion = "#version 330 core\n";
			return defaultVersion + std::string(source);
		}
	}	
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

		rhi::SwapChainDesc GetDesc() const override
		{
			SwapChainDesc outDesc = desc_.base;
			if (desc_.hooks.getDrawableExtent)
			{
				outDesc.extent = desc_.hooks.getDrawableExtent();
			}
			return outDesc;
		}

		FramebufferHandle GetCurrentBackBuffer() const override
		{
			return FramebufferHandle{ 0 };
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

		std::string_view GetName() const override
		{
			return name_;
		}

		TextureHandle CreateTexture2D(Extent2D extent, Format format) override
		{
			GLuint textureId = 0;
			glGenTextures(1, &textureId);
			glBindTexture(GL_TEXTURE_2D, textureId);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			const GLenum internalFormat = ToGLInternalFormat(format);
			const GLenum baseFormat = ToGLBaseFormat(format);
			const GLenum type = ToGLType(format);

			glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), static_cast<GLsizei>(extent.width),
				static_cast<GLsizei>(extent.height), 0, baseFormat, type, nullptr);

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

		FramebufferHandle CreateFramebuffer(TextureHandle color, TextureHandle depth) override
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

			return FramebufferHandle{ static_cast<std::uint32_t>(framebufferId) };
		}

		void DestroyFramebuffer(FramebufferHandle framebuffer) noexcept override
		{
			GLuint framebufferId = static_cast<GLuint>(framebuffer.id);
			if (framebufferId != 0)
			{
				glDeleteFramebuffers(1, &framebufferId);
			}
		}

		BufferHandle CreateBuffer(const BufferDesc& desc)
		{
			GLuint bufferId = 0;
			glGenBuffers(1, &bufferId);
			const GLenum target = ToGLBufferTarget(desc.bindFlag);
			glBindBuffer(target, bufferId);
			glBufferData(target, static_cast<GLsizeiptr>(desc.sizeInBytes),nullptr, ToGLUsage(desc.usageFlag));
			glBindBuffer(target, 0);

			bufferTargets_.emplace(bufferId, target);
			return BufferHandle{ static_cast<std::uint32_t>(bufferId) };
		}

		void UpdateBuffer(BufferHandle buffer, std::span<const std::byte> data, std::size_t offsetBytes)
		{
			const GLuint bufferId = static_cast<GLuint>(buffer.id);
			if (bufferId == 0 || data.empty())
			{
				return;
			}
			const GLenum target = BufferTargetFor(bufferId);
			glBindBuffer(target, bufferId);
			glBufferSubData(target, static_cast<GLintptr>(offsetBytes), static_cast<GLsizeiptr>(data.size()), data.data());
			glBindBuffer(target, 0);
		}

		void DestroyBuffer(BufferHandle buffer) noexcept
		{
			GLuint bufferId = static_cast<GLuint>(buffer.id);
			if (bufferId != 0)
			{
				glDeleteBuffers(1, &bufferId);
				bufferTargets_.erase(bufferId);
			}
		}

		VertexArrayHandle CreateVertexArray(std::string_view debugName)
		{
			GLuint vaoId = 0;
			glGenVertexArrays(1, &vaoId);
			return VertexArrayHandle{ static_cast<std::uint32_t>(vaoId) };
		}

		void SetVertexArrayLayout(VertexArrayHandle vao, BufferHandle vbo, std::span<const VertexAttributeDesc> attributes)
		{
			glBindVertexArray(static_cast<GLuint>(vao.id));
			glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(vbo.id));

			for (auto attribute : attributes)
			{
				glEnableVertexAttribArray(static_cast<GLuint>(attribute.location));
				glVertexAttribPointer(
					static_cast<GLuint>(attribute.location),
					static_cast<GLint>(attribute.componentCount),
					static_cast<GLenum>(attribute.glType),
					attribute.normalized ? GL_TRUE : GL_FALSE,
					static_cast<GLsizei>(attribute.strideBytes),
					reinterpret_cast<const void*>(static_cast<std::intptr_t>(attribute.offsetBytes)));
			}
			glBindVertexArray(0);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
		}

		void SetVertexArrayIndexBuffer(VertexArrayHandle vao, BufferHandle ibo, IndexType indexType)
		{
			glBindVertexArray(static_cast<GLuint>(vao.id));
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(ibo.id));
			vaoIndexType_[static_cast<GLuint>(vao.id)] = indexType;
			glBindVertexArray(0);
		}

		void DestroyVertexArray(VertexArrayHandle vao) noexcept
		{
			GLuint vaoId = static_cast<GLuint>(vao.id);
			if (vaoId != 0)
			{
				glDeleteVertexArrays(1, &vaoId);
				vaoIndexType_.erase(vaoId);
			}
		}

		ShaderHandle CreateShader(std::string_view debugName, std::string_view sourceOrBytecode)
		{
			const GLenum shaderType = GuessShaderType(debugName);
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

		void DestroyShader(ShaderHandle shader) noexcept
		{
			GLuint shaderId = static_cast<GLuint>(shader.id);
			if (shaderId != 0)
			{
				glDeleteShader(shaderId);
			}
		}

		PipelineHandle CreatePipeline(std::string_view debugName, ShaderHandle vertexShader, ShaderHandle pixelShader)
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

		void DestroyPipeline(PipelineHandle pso) noexcept
		{
			GLuint programId = static_cast<GLuint>(pso.id);
			if (programId != 0)
			{
				glDeleteProgram(programId);
			}
		}

		void SubmitCommandList(CommandList&& commandList)
		{
			for (const auto& command : commandList.commands)
			{
				std::visit([this](auto&& cmd) { ExecuteOnce(cmd); }, command);
			}
		}

		TextureDescIndex AllocateTextureDesctiptor(TextureHandle texture) 
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

		void UpdateTextureDescriptor(TextureDescIndex index, TextureHandle texture) 
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

		void FreeTextureDescriptor(TextureDescIndex index) noexcept
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

		FenceHandle CreateFence(bool signaled = false)
		{
			const std::uint32_t fenceId = ++nextFenceId_;
			GLFence fence;
			fence.sync = nullptr;
			fence.signaled = signaled;
			fences_.emplace(fenceId, fence);
			return FenceHandle{ fenceId };
		}

		void DestroyFence(FenceHandle fence) noexcept
		{
			if (auto it = fences_.find(fence.id); it != fences_.end())
			{
				if (it->second.sync)
				{
					glDeleteSync(it->second.sync);
				}
				fences_.erase(it);
			}
		}

		void SignalFence(FenceHandle fence)
		{
			auto it = fences_.find(fence.id);
			if (it == fences_.end())
			{
				return;
			}if (it->second.sync)
			{
				glDeleteSync(it->second.sync);
				it->second.sync = nullptr;
			}
			it->second.sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			it->second.signaled = false;
		}

		void WaitFence(FenceHandle fence)
		{
			auto it = fences_.find(fence.id);
			if (it == fences_.end())
			{
				return;
			}
			if (it->second.signaled)
			{
				return;
			}
			if (!it->second.sync)
			{
				return;
			}

			while (true)
			{
				const GLenum result = glClientWaitSync(it->second.sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
				if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED)
				{
					break;
				}
			}
			glDeleteSync(it->second.sync);
			it->second.sync = nullptr;
			it->second.signaled = true;
		}

		bool IsFenceSignaled(FenceHandle fence)
		{
			auto it = fences_.find(fence.id);
			if (it == fences_.end())
			{
				return true;
			}
			if (it->second.signaled)
			{
				return true;
			}
			if (!it->second.sync)
			{
				return false;
			}

			const GLenum result = glClientWaitSync(it->second.sync, 0, 0);
			if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED)
			{
				glDeleteSync(it->second.sync);
				it->second.sync = nullptr;
				it->second.signaled = true;
				return true;
			}
			return false;
		}

	private:
		GLenum BufferTargetFor(GLuint bufferId)
		{
			
			if (auto it = bufferTargets_.find(bufferId); it != bufferTargets_.end())
			{
				return it->second;
			}
			return GL_ARRAY_BUFFER;
		}

		TextureHandle ResolveTextureDesc(TextureDescIndex index)
		{
			if (index == 0 || index >= textureDescriptions_.size())
			{
				return TextureHandle{};
			}
			return textureDescriptions_[index];
		}

		void ApplyState(const GraphicsState& state)
		{
			// Depth state
			if (state.depth.testEnable)
			{
				glEnable(GL_DEPTH_TEST);
				glDepthFunc(ToGLCompareOp(state.depth.depthCompareOp));
			}
			else
			{
				glDisable(GL_DEPTH_TEST);
			}
			glDepthMask(state.depth.writeEnable ? GL_TRUE : GL_FALSE);

			// Rasterizer state
			if (state.rasterizer.cullMode != CullMode::None)
			{
				glEnable(GL_CULL_FACE);
				glCullFace(ToGLCullMode(state.rasterizer.cullMode));
			}
			else
			{
				glDisable(GL_CULL_FACE);
			}
			glFrontFace(ToGLFrontFace(state.rasterizer.frontFace));

			// Blend state
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

		void ExecuteOnce(const CommandEndPass& /*cmd*/){}

		void ExecuteOnce(const CommandSetVieport& cmd)
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

		void ExecuteOnce(const CommandVertexArray& cmd)
		{
			glBindVertexArray(static_cast<GLuint>(cmd.vao.id));
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

		void ExecuteOnce(const CommandSetUniformInt& cmd)
		{
			SetUniformIntImpl(cmd.name, cmd.value);
		}

		void ExecuteOnce(const CommandUniformFloat4& cmd)
		{
			SetUniformFloat4Impl(cmd.name, cmd.value);
		}

		void ExecuteOnce(const CommandDrawIndexed& cmd)
		{
			const GLenum indexType = ToGLIndexType(cmd.indexType);
			const std::size_t elementSize = (indexType == GL_UNSIGNED_INT) ? sizeof(std::uint32_t) : sizeof(std::uint16_t);
			const void* offsetPtr = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(cmd.firstIndex * elementSize));

			glDrawElementsBaseVertex(
				GL_TRIANGLES,
				static_cast<GLsizei>(cmd.indexCount),
				indexType,
				offsetPtr,
				static_cast<GLint>(cmd.baseVertex));
		}

		void ExecuteOnce(const CommandDraw& cmd)
		{
			glDrawArrays(GL_TRIANGLES, static_cast<GLint>(cmd.firstVertex), static_cast<GLsizei>(cmd.vertexCount));
		}

		//---------------------- End of an executeOnce section -------------------------------//

		//---------------------------------------------------------------------//
		GLDeviceDesc desc_{};
		std::string name_;

		struct GLFence
		{
			GLsync sync{ nullptr };
			bool signaled{ true };
		};

		std::unordered_map<GLuint, GLenum> bufferTargets_{};
		std::unordered_map<GLuint, IndexType> vaoIndexType_{};
		GLuint currentProgram_{ 0 };

		std::vector<TextureHandle> textureDescriptions_{ TextureHandle{} };
		std::vector<TextureDescIndex> freeTextureDescIndices_;
		std::uint32_t nextFenceId_{ 0 };
		std::unordered_map<std::uint32_t, GLFence> fences_{};
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
