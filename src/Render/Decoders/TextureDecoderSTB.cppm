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
import :rhi;

export class StbTextureDecoder final : public ITextureDecoder
{
	static void CopyRectRGBA8(
		const std::uint8_t* src, int srcW, int srcH,
		int x0, int y0, int size,
		std::vector<std::uint8_t>& outFace)
	{
		outFace.resize(size * size * 4);

		for (int y = 0; y < size; ++y)
		{
			const std::uint8_t* srcRow = src + ((y0 + y) * srcW + x0) * 4;
			std::uint8_t* dstRow = outFace.data() + (y * size) * 4;
			std::memcpy(dstRow, srcRow, size * 4);
		}
	}

	static bool TryDecodeCubeCrossRGBA8(
		const std::filesystem::path& filePath,
		TextureCPUData& outCpu,
		std::string& outError)
	{
		int width = 0;
		int height = 0;
		int channels = 0;

		std::uint8_t* pixels = stbi_load(filePath.string().c_str(), &width, &height, &channels, 4);
		if (!pixels)
		{
			outError = stbi_failure_reason() ? stbi_failure_reason() : "stbi_load failed";
			return false;
		}

		const int faceSize = width / 4;
		const bool isHorizontalCross = (width == faceSize * 4) && (height == faceSize * 3);

		if (!isHorizontalCross)
		{
			stbi_image_free(pixels);
			outError = "Not a 4x3 horizontal cross cubemap";
			return false;
		}

		// coords in pixels:
		struct Rect { int x; int y; };
		const Rect rects[6] = {
			{ 2 * faceSize, 1 * faceSize }, // +X
			{ 0 * faceSize, 1 * faceSize }, // -X
			{ 1 * faceSize, 0 * faceSize }, // +Y
			{ 1 * faceSize, 2 * faceSize }, // -Y
			{ 1 * faceSize, 1 * faceSize }, // +Z
			{ 3 * faceSize, 1 * faceSize }, // -Z
		};

		outCpu.width = faceSize;
		outCpu.height = faceSize;
		outCpu.format = TextureFormat::RGBA;

		for (int face = 0; face < 6; ++face)
		{
			CopyRectRGBA8(pixels, width, height, rects[face].x, rects[face].y, faceSize, outCpu.cubePixels[face]);
		}

		stbi_image_free(pixels);
		return true;
	}

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
			stbi_set_flip_vertically_on_load(0);

			TextureCPUData out{};
			out.dimension = TextureDimension::Cube;
			out.format = TextureFormat::RGBA;
			out.channels = 4;

			if (properties.cubeFromCross)
			{
				std::string error;
				if (!TryDecodeCubeCrossRGBA8(resolvePath(properties.filePath), out, error))
				{
					throw std::runtime_error("Cubemap cross decode failed: " + error);
				}
				return out;
			}
			else
			{
				// Expect explicit face paths in properties.cubeFacePaths.
				for (std::size_t i = 0; i < 6; ++i)
				{
					if (properties.cubeFacePaths[i].empty())
					{
						throw std::runtime_error("StbTextureDecoder: cubemap face path is empty (index " + std::to_string(i) + ")");
					}
				}


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
		}
		else
		{
			stbi_set_flip_vertically_on_load(properties.flipY ? 1 : 0);
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