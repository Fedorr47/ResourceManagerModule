module;

#include <GL/glew.h>
#include <string>
#include <stdexcept>

export module core:renderer_gl;

import :rhi;
import :rhi_gl;
import :scene_bridge;
import :resource_manager_core;
import :shader_files;
import :file_system;

export namespace RHI_GL_UTILS
{
	GLuint CompileProgram(std::string_view vsSource, std::string_view fsSource)
	{
		const auto Compile = [](GLenum type, std::string_view src, std::string_view name) -> GLuint
			{
				GLuint Shader = glCreateShader(type);
				if (Shader == 0)
				{
					throw std::runtime_error("OpenGL: glCreateShader failed");
				}
				const char* sourceCStr = src.data();
				GLint length = static_cast<GLint>(src.size());
				glShaderSource(Shader, 1, &sourceCStr, &length);
				glCompileShader(Shader);
				RHI_GL_UTILS::ThrowIfShaderCompilationFailed(Shader, name);
				return Shader;
			};

		const GLuint vertexShader = Compile(GL_VERTEX_SHADER, vsSource, "VS");
		const GLuint pixelShader = Compile(GL_FRAGMENT_SHADER, fsSource, "FS");

		GLuint program = glCreateProgram();
		if (program == 0)
		{
			glDeleteShader(vertexShader);
			glDeleteShader(pixelShader);
			throw std::runtime_error("OpenGL: glCreateProgram failed");
		}

		glAttachShader(program, vertexShader);
		glAttachShader(program, pixelShader);
		glLinkProgram(program);

		RHI_GL_UTILS::ThrowIfProgramLinkFailed(program, "stub_name");

		glDetachShader(program, vertexShader);
		glDetachShader(program, pixelShader);
		glDeleteShader(vertexShader);
		glDeleteShader(pixelShader);

		return program;
	}
}

export namespace rendern
{
	class GLSimpleRenderer
	{
	public:
		GLSimpleRenderer() = default;
		~GLSimpleRenderer()
		{
			Shutdown();
		}

		GLSimpleRenderer(const GLSimpleRenderer&) = delete;
		GLSimpleRenderer& operator=(const GLSimpleRenderer&) = delete;

		void Initialize()
		{
			if (initialized_)
			{
				return;
			}

			const float verts[] = {
				-1.0f, -1.0f,  0.0f, 0.0f,
				 3.0f, -1.0f,  2.0f, 0.0f,
				-1.0f,  3.0f,  0.0f, 2.0f
			};

			auto pathToVertexShader = corefs::ResolveAsset(std::filesystem::path("shaders\\FullScreen.vert"));
			auto pathToPixelShader = corefs::ResolveAsset(std::filesystem::path("shaders\\FullScreen.frag"));

			const auto vsFile = rendern::LoadGLSLWithIncludes(pathToVertexShader);
			const auto fsFile = rendern::LoadGLSLWithIncludes(pathToPixelShader);

			const std::string vsSrc = vsFile.text;
			const std::string fsSrc = fsFile.text;

			program_ = RHI_GL_UTILS::CompileProgram(vsSrc, fsSrc);

			glGenVertexArrays(1, &vao_);
			glGenBuffers(1, &vbo_);

			glBindVertexArray(vao_);
			glBindBuffer(GL_ARRAY_BUFFER, vbo_);
			glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

			glEnableVertexAttribArray(0);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

			glEnableVertexAttribArray(1);
			glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

			glBindBuffer(GL_ARRAY_BUFFER, 0);
			glBindVertexArray(0);

			uTex_ = glGetUniformLocation(program_, "uTex");
			uFallbackColor_ = glGetUniformLocation(program_, "uFallbackColor");
			uHasTex_ = glGetUniformLocation(program_, "uHasTex");

			initialized_ = true;
		}

		void Shutdown() noexcept
		{
			if (program_ != 0)
			{
				glDeleteProgram(program_);
				program_ = 0;
			}
			if (vbo_ != 0)
			{
				glDeleteBuffers(1, &vbo_);
				vbo_ = 0;
			}
			if (vao_ != 0)
			{
				glDeleteVertexArrays(1, &vao_);
				vao_ = 0;
			}
			initialized_ = false;
		}

		void SetFallbackColor(float r, float g, float b, float a)
		{
			fallback_[0] = r; 
			fallback_[1] = g; 
			fallback_[2] = b; 
			fallback_[3] = a;
		}

		void SetTexture(GPUTexture texture)
		{
			texture_ = texture;
		}

		void Render(rhi::IRHISwapChain& swapChain)
		{
			Initialize();

			const auto desc = swapChain.GetDesc();
			glViewport(0, 0, static_cast<GLsizei>(desc.extent.width), static_cast<GLsizei>(desc.extent.height));
			glDisable(GL_DEPTH_TEST);

			glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);

			glUseProgram(program_);
			glBindVertexArray(vao_);

			const bool hasTex = (texture_.id != 0);
			if (uHasTex_ != -1)
			{
				glUniform1i(uHasTex_, hasTex ? 1 : 0);
			}
			if (uFallbackColor_ != -1)
			{
				glUniform4f(uFallbackColor_, fallback_[0], fallback_[1], fallback_[2], fallback_[3]);
			}

			if (hasTex)
			{
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_.id));
				if (uTex_ != -1)
				{
					glUniform1i(uTex_, 0);
				}
			}

			glDrawArrays(GL_TRIANGLES, 0, 3);

			if (hasTex)
			{
				glBindTexture(GL_TEXTURE_2D, 0);
			}
			glBindVertexArray(0);
			glUseProgram(0);

			swapChain.Present();
		}

	private:
		bool initialized_{ false };

		GLuint program_{ 0 };
		GLuint vao_{ 0 };
		GLuint vbo_{ 0 };

		GLint uTex_{ -1 };
		GLint uFallbackColor_{ -1 };
		GLint uHasTex_{ -1 };

		float fallback_[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
		GPUTexture texture_{};
	};
}
