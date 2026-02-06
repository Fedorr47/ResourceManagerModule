module;

#include <GL/glew.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

export module core:render_core_gl;

import :render_core;
import :resource_manager_core;

namespace
{
	static GLenum ToGLExternalFormat(TextureFormat format)
	{
		switch (format)
		{
		case TextureFormat::RGB:
			return GL_RGB;
		case TextureFormat::RGBA:
			return GL_RGBA;
		case TextureFormat::GRAYSCALE:
			return GL_RED;
		default:
			return GL_RGBA;
		}
	}

	static GLint ToGLInternalFormat(TextureFormat format, bool srgb)
	{
		switch (format)
		{
		case TextureFormat::RGB:
			return srgb ? GL_SRGB8 : GL_RGB8;
		case TextureFormat::RGBA:
			return srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
		case TextureFormat::GRAYSCALE:
			return GL_R8;
		default:
			return GL_RGBA8;
		}
	}

	static void SetDefaultTextureParameters(bool generateMips)
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

		if (generateMips)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		}
		else
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		}

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}

	static void SetDefaultCubemapParameters(bool generateMips)
	{
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

		if (generateMips)
		{
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		}
		else
		{
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		}

		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
}

export namespace rendern
{
	class GLTextureUploader final : public ITextureUploader
	{
	public:
		explicit GLTextureUploader(rhi::IRHIDevice& device) noexcept
		{
			//assert(device.GetBackend() == rhi::Backend::OpenGL);
		}
		std::optional<GPUTexture> CreateAndUpload(const TextureCPUData& cpuData, const TextureProperties& properties) override
		{
			// ---------------------- Cubemap ----------------------
			if (properties.dimension == TextureDimension::Cube)
			{
				// We expect cpuData.cubePixels[0..5] to be filled.
				const GLsizei width = static_cast<GLsizei>(cpuData.width ? cpuData.width : properties.width);
				const GLsizei height = static_cast<GLsizei>(cpuData.height ? cpuData.height : properties.height);

				if (width <= 0 || height <= 0)
				{
					return std::nullopt;
				}

				for (int i = 0; i < 6; ++i)
				{
					if (cpuData.cubePixels[static_cast<std::size_t>(i)].empty())
					{
						return std::nullopt;
					}
				}

				GLuint textureId = 0;
				glGenTextures(1, &textureId);
				if (textureId == 0)
				{
					return std::nullopt;
				}

				glBindTexture(GL_TEXTURE_CUBE_MAP, textureId);
				SetDefaultCubemapParameters(properties.generateMips);

				glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

				const TextureFormat fmt = (cpuData.format == TextureFormat::RGB || cpuData.format == TextureFormat::GRAYSCALE)
					? TextureFormat::RGBA
					: cpuData.format;

				const GLenum externalFormat = ToGLExternalFormat(fmt);
				const GLenum internalFormat = ToGLInternalFormat(fmt, properties.srgb);

				for (GLuint face = 0; face < 6; ++face)
				{
					const auto& facePixels = cpuData.cubePixels[face];
					glTexImage2D(
						GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
						0,
						internalFormat,
						width,
						height,
						0,
						externalFormat,
						GL_UNSIGNED_BYTE,
						facePixels.data());
				}

				if (properties.generateMips)
				{
					glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
				}

				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
				glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

				if (glGetError() != GL_NO_ERROR)
				{
					glDeleteTextures(1, &textureId);
					return std::nullopt;
				}

				return GPUTexture{ static_cast<unsigned int>(textureId) };
			}

			// ---------------------- Tex2D ----------------------
			if (cpuData.pixels.empty())
			{
				return std::nullopt;
			}

			GLuint textureId = 0;
			glGenTextures(1, &textureId);
			if (textureId == 0)
			{
				return std::nullopt;
			}

			glBindTexture(GL_TEXTURE_2D, textureId);
			SetDefaultTextureParameters(properties.generateMips);

			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

			const GLenum externalFormat = ToGLExternalFormat(cpuData.format);
			const GLenum internalFormat = ToGLInternalFormat(cpuData.format, properties.srgb);

			const GLsizei width = static_cast<GLsizei>(cpuData.width ? cpuData.width : properties.width);
			const GLsizei height = static_cast<GLsizei>(cpuData.height ? cpuData.height : properties.height);

			glTexImage2D(
				GL_TEXTURE_2D,
				0,
				internalFormat,
				width,
				height,
				0,
				externalFormat,
				GL_UNSIGNED_BYTE,
				cpuData.pixels.data());

			if (properties.generateMips)
			{
				glGenerateMipmap(GL_TEXTURE_2D);
			}

			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glBindTexture(GL_TEXTURE_2D, 0);

			if (glGetError() != GL_NO_ERROR)
			{
				glDeleteTextures(1, &textureId);
				return std::nullopt;
			}

			return GPUTexture{ static_cast<unsigned int>(textureId) };
		}

		void Destroy(GPUTexture texture) noexcept
		{
			const GLuint textureId = static_cast<GLuint>(texture.id);
			if (textureId != 0)
			{
				glDeleteTextures(1, &textureId);
			}
		}
	};
}