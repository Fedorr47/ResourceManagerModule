module;

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>

export module core:texture_decoder_stb;

import :resource_manager_core;
import :file_system;

export class StbTextureDecoder final : public ITextureDecoder
{
	std::optional<TextureCPUData> Decode(const TextureProperties& properties, std::string_view resolvedPath)
	{
		namespace fs = std::filesystem;

		// stb flip is global; set it per-decode.
		stbi_set_flip_vertically_on_load(properties.flipY ? 1 : 0);

		auto resolvePath = [](std::string_view pth) -> fs::path
			{
				fs::path p = fs::path(std::string(pth));
				if (!p.is_absolute())
				{
					p = corefs::ResolveAsset(p);
				}
				return p;
			};

		auto loadFaceRGBA8 = [&](const fs::path& facePath, int& outW, int& outH) -> std::vector<unsigned char>
			{
				if (!fs::exists(facePath))
				{
					throw std::runtime_error("StbTextureDecoder couldn't find file: " + facePath.string());
				}

				int w = 0, h = 0, comp = 0;
				stbi_uc* data = stbi_load(facePath.string().c_str(), &w, &h, &comp, 4 /*force RGBA*/);
				if (!data)
				{
					throw std::runtime_error(std::string("stbi_load failed: ") + stbi_failure_reason());
				}

				outW = w;
				outH = h;

				const std::size_t size = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
				std::vector<unsigned char> pixels;
				pixels.resize(size);
				std::memcpy(pixels.data(), data, size);
				stbi_image_free(data);
				return pixels;
			};

		// ---------------------- Cubemap ----------------------
		if (properties.dimension == TextureDimension::Cube)
		{
			// Expect explicit face paths in properties.cubeFacePaths.
			for (std::size_t i = 0; i < 6; ++i)
			{
				if (properties.cubeFacePaths[i].empty())
				{
					throw std::runtime_error("StbTextureDecoder: cubemap face path is empty (index " + std::to_string(i) + ")");
				}
			}

			TextureCPUData out{};
			out.dimension = TextureDimension::Cube;
			out.format = TextureFormat::RGBA;
			out.channels = 4;

			int w0 = 0, h0 = 0;
			for (std::size_t face = 0; face < 6; ++face)
			{
				const fs::path facePath = resolvePath(properties.cubeFacePaths[face]);

				int w = 0, h = 0;
				out.cubePixels[face] = loadFaceRGBA8(facePath, w, h);

				if (face == 0)
				{
					w0 = w; h0 = h;
				}
				else
				{
					if (w != w0 || h != h0)
					{
						throw std::runtime_error(
							"StbTextureDecoder: cubemap faces must have the same size. "
							"Face " + std::to_string(face) + " has " + std::to_string(w) + "x" + std::to_string(h) +
							", expected " + std::to_string(w0) + "x" + std::to_string(h0));
					}
				}
			}

			out.width = static_cast<std::uint32_t>(w0);
			out.height = static_cast<std::uint32_t>(h0);
			return out;
		}

		// ---------------------- Tex2D ----------------------
		fs::path correctPath = resolvePath(resolvedPath);

		if (!fs::exists(correctPath))
		{
			throw std::runtime_error("StbTextureDecoder couldn't find file: " + correctPath.string());
		}

		int width = 0, height = 0, comp = 0;
		stbi_uc* data = stbi_load(correctPath.string().c_str(), &width, &height, &comp, 0);
		if (!data)
		{
			throw std::runtime_error(std::string("stbi_load failed: ") + stbi_failure_reason());
		}

		TextureFormat format = TextureFormat::RGBA;
		int channels = comp;

		if (channels == 1) format = TextureFormat::GRAYSCALE;
		else if (channels == 3) format = TextureFormat::RGB;
		else if (channels == 4) format = TextureFormat::RGBA;
		else
		{
			// Unsupported component count -> reload as RGBA.
			stbi_image_free(data);
			data = stbi_load(correctPath.string().c_str(), &width, &height, &comp, 4);
			if (!data)
			{
				throw std::runtime_error(std::string("stbi_load failed: ") + stbi_failure_reason());
			}
			channels = 4;
			format = TextureFormat::RGBA;
		}

		TextureCPUData out{};
		out.dimension = TextureDimension::Tex2D;
		out.width = static_cast<std::uint32_t>(width);
		out.height = static_cast<std::uint32_t>(height);
		out.channels = channels;
		out.format = format;

		const std::size_t size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels);
		out.pixels.resize(size);
		std::memcpy(out.pixels.data(), data, size);

		stbi_image_free(data);
		return out;
	}
};