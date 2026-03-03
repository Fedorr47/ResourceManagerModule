module;

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <bit>

export module core:texture_decoder_stb;

import :resource_manager_core;
import :file_system;
import :rhi;

export class StbTextureDecoder final : public ITextureDecoder
{
	static void CopyRectRGBA8(
		const std::uint8_t* src, int srcW, int srcH,
		int x0, int y0, int size,
		std::vector<unsigned char>& outFace)
	{
		outFace.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4u);

		for (int y = 0; y < size; ++y)
		{
			const std::uint8_t* srcRow = src + ((y0 + y) * srcW + x0) * 4;
			unsigned char* dstRow = outFace.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(size)) * 4u;
			std::memcpy(dstRow, srcRow, static_cast<std::size_t>(size) * 4u);
		}
	}

	static void FlipImageRowsRGBA8(std::vector<unsigned char>& pixels, int width, int height)
	{
		if (width <= 0 || height <= 1)
		{
			return;
		}

		const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
		std::vector<unsigned char> scratch(rowBytes);
		for (int y = 0; y < height / 2; ++y)
		{
			auto* rowA = pixels.data() + static_cast<std::size_t>(y) * rowBytes;
			auto* rowB = pixels.data() + static_cast<std::size_t>(height - 1 - y) * rowBytes;
			std::memcpy(scratch.data(), rowA, rowBytes);
			std::memcpy(rowA, rowB, rowBytes);
			std::memcpy(rowB, scratch.data(), rowBytes);
		}
	}

	static std::vector<TextureMipLevel> MakeMipChain_Box2x2(
		const std::vector<unsigned char>& mip0,
		std::uint32_t width0,
		std::uint32_t height0,
		int channels,
		bool genMips,
		bool srgb,
		bool isNormalMap)
	{
		std::vector<TextureMipLevel> chain;
		chain.reserve(1u + static_cast<std::size_t>(std::bit_width(std::max(width0, height0))));

		// Normal maps must stay in linear space.
		const bool effectiveSrgb = srgb && !isNormalMap;

		auto SrgbToLinear = [](float c) noexcept -> float
			{
				// IEC 61966-2-1:1999
				if (c <= 0.04045f)
				{
					return c / 12.92f;
				}
				return std::pow((c + 0.055f) / 1.055f, 2.4f);
			};

		auto LinearToSrgbU8 = [](float c) noexcept -> unsigned char
			{
				c = std::clamp(c, 0.0f, 1.0f);
				float s = 0.0f;
				if (c <= 0.0031308f)
				{
					s = c * 12.92f;
				}
				else
				{
					s = 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
				}

				const int iv = static_cast<int>(std::round(std::clamp(s, 0.0f, 1.0f) * 255.0f));
				return static_cast<unsigned char>(std::clamp(iv, 0, 255));
			};

		TextureMipLevel base{};
		base.width = width0;
		base.height = height0;
		base.pixels = mip0;
		chain.push_back(std::move(base));

		if (!genMips)
		{
			return chain;
		}

		std::uint32_t curW = width0;
		std::uint32_t curH = height0;

		while (curW > 1 || curH > 1)
		{
			const std::uint32_t nextW = std::max(1u, curW / 2u);
			const std::uint32_t nextH = std::max(1u, curH / 2u);

			const auto& prev = chain.back();

			TextureMipLevel next{};
			next.width = nextW;
			next.height = nextH;
			next.pixels.resize(
				static_cast<std::size_t>(nextW) * static_cast<std::size_t>(nextH) * static_cast<std::size_t>(channels));

			for (std::uint32_t y = 0; y < nextH; ++y)
			{
				for (std::uint32_t x = 0; x < nextW; ++x)
				{
					std::uint32_t acc[4] = { 0,0,0,0 };
					float accLin[3] = { 0.0f, 0.0f, 0.0f };
					std::uint32_t cnt = 0;

					for (std::uint32_t ky = 0; ky < 2; ++ky)
					{
						for (std::uint32_t kx = 0; kx < 2; ++kx)
						{
							const std::uint32_t sx = std::min(curW - 1u, x * 2u + kx);
							const std::uint32_t sy = std::min(curH - 1u, y * 2u + ky);

							const std::size_t si =
								(static_cast<std::size_t>(sy) * static_cast<std::size_t>(curW) + static_cast<std::size_t>(sx))
								* static_cast<std::size_t>(channels);

							for (int c = 0; c < channels; ++c)
							{
								const unsigned char v = prev.pixels[si + static_cast<std::size_t>(c)];
								acc[c] += v;
								if (effectiveSrgb && c < 3)
								{
									accLin[c] += SrgbToLinear(static_cast<float>(v) / 255.0f);
								}
							}
							++cnt;
						}
					}

					const std::size_t di =
						(static_cast<std::size_t>(y) * static_cast<std::size_t>(nextW) + static_cast<std::size_t>(x))
						* static_cast<std::size_t>(channels);

					if (isNormalMap && channels >= 3)
					{
						// Decode to [-1..1], average, renormalize, encode back to [0..255].
						auto DecodeN = [](unsigned char v) noexcept -> float
							{
								return (static_cast<float>(v) / 255.0f) * 2.0f - 1.0f;
							};

						float nx = 0.0f;
						float ny = 0.0f;
						float nz = 0.0f;

						for (std::uint32_t ky = 0; ky < 2; ++ky)
						{
							for (std::uint32_t kx = 0; kx < 2; ++kx)
							{
								const std::uint32_t sx = std::min(curW - 1u, x * 2u + kx);
								const std::uint32_t sy = std::min(curH - 1u, y * 2u + ky);
								const std::size_t si =
									(static_cast<std::size_t>(sy) * static_cast<std::size_t>(curW) + static_cast<std::size_t>(sx))
									* static_cast<std::size_t>(channels);

								nx += DecodeN(prev.pixels[si + 0]);
								ny += DecodeN(prev.pixels[si + 1]);
								nz += DecodeN(prev.pixels[si + 2]);
							}
						}

						nx /= static_cast<float>(cnt);
						ny /= static_cast<float>(cnt);
						nz /= static_cast<float>(cnt);

						const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
						if (len > 1e-6f)
						{
							nx /= len;
							ny /= len;
							nz /= len;
						}
						else
						{
							// Fallback to a flat normal.
							nx = 0.0f;
							ny = 0.0f;
							nz = 1.0f;
						}

						auto EncodeN = [](float v) noexcept -> unsigned char
							{
								const float n01 = std::clamp(v * 0.5f + 0.5f, 0.0f, 1.0f);
								const int iv = static_cast<int>(std::round(n01 * 255.0f));
								return static_cast<unsigned char>(std::clamp(iv, 0, 255));
							};

						next.pixels[di + 0] = EncodeN(nx);
						next.pixels[di + 1] = EncodeN(ny);
						next.pixels[di + 2] = EncodeN(nz);

						for (int c = 3; c < channels; ++c)
						{
							next.pixels[di + static_cast<std::size_t>(c)] = static_cast<unsigned char>(acc[c] / cnt);
						}
					}
					else
					{
						for (int c = 0; c < channels; ++c)
						{
							if (effectiveSrgb && c < 3)
							{
								const float lin = accLin[c] / static_cast<float>(cnt);
								next.pixels[di + static_cast<std::size_t>(c)] = LinearToSrgbU8(lin);
							}
							else
							{
								next.pixels[di + static_cast<std::size_t>(c)] = static_cast<unsigned char>(acc[c] / cnt);
							}
						}
					}
				}
			}

			chain.push_back(std::move(next));
			curW = nextW;
			curH = nextH;
		}

		return chain;
	}

	static bool TryDecodeCubeCrossRGBA8(
		const std::filesystem::path& filePath,
		bool generateMips,
		bool srgb,
		bool isNormalMap,
		bool flipY,
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

		outCpu.width = static_cast<std::uint32_t>(faceSize);
		outCpu.height = static_cast<std::uint32_t>(faceSize);
		outCpu.format = TextureFormat::RGBA;
		outCpu.channels = 4;

		for (int face = 0; face < 6; ++face)
		{
			std::vector<unsigned char> mip0;
			CopyRectRGBA8(pixels, width, height, rects[face].x, rects[face].y, faceSize, mip0);
			if (flipY)
			{
				FlipImageRowsRGBA8(mip0, faceSize, faceSize);
			}
			outCpu.cubeMips[static_cast<std::size_t>(face)] = MakeMipChain_Box2x2(
				mip0,
				static_cast<std::uint32_t>(faceSize),
				static_cast<std::uint32_t>(faceSize),
				4,
				generateMips,
				srgb,
				isNormalMap);
		}

		stbi_image_free(pixels);
		return true;
	}

	std::optional<TextureCPUData> Decode(const TextureProperties& properties, std::string_view resolvedPath)
	{
		namespace fs = std::filesystem;

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
			TextureCPUData out{};
			out.dimension = TextureDimension::Cube;
			out.format = TextureFormat::RGBA;
			out.channels = 4;

			if (properties.cubeFromCross)
			{
				std::string error;
				if (!TryDecodeCubeCrossRGBA8(resolvePath(properties.filePath), properties.generateMips, properties.srgb, properties.isNormalMap, properties.flipY, out, error))
				{
					throw std::runtime_error("Cubemap cross decode failed: " + error);
				}
				return out;
			}

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
				auto mip0 = loadFaceRGBA8(facePath, w, h);
				if (properties.flipY)
				{
					FlipImageRowsRGBA8(mip0, w, h);
				}

				if (face == 0)
				{
					w0 = w;
					h0 = h;
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

				out.cubeMips[face] = MakeMipChain_Box2x2(
					mip0,
					static_cast<std::uint32_t>(w),
					static_cast<std::uint32_t>(h),
					4,
					properties.generateMips,
					properties.srgb,
					properties.isNormalMap);
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
		stbi_uc* data = stbi_load(correctPath.string().c_str(), &width, &height, &comp, 4 /*force RGBA*/);
		if (!data)
		{
			throw std::runtime_error(std::string("stbi_load failed: ") + stbi_failure_reason());
		}

		TextureCPUData out{};
		out.dimension = TextureDimension::Tex2D;
		out.width = static_cast<std::uint32_t>(width);
		out.height = static_cast<std::uint32_t>(height);
		out.channels = 4;
		out.format = TextureFormat::RGBA;

		const std::size_t size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
		std::vector<unsigned char> mip0;
		mip0.resize(size);
		std::memcpy(mip0.data(), data, size);
		if (properties.flipY)
		{
			FlipImageRowsRGBA8(mip0, width, height);
		}

		stbi_image_free(data);

		out.mips = MakeMipChain_Box2x2(
			mip0,
			out.width,
			out.height,
			out.channels,
			properties.generateMips,
			properties.srgb,
			properties.isNormalMap);
		return out;
	}
};
