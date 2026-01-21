module;

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>

export module core:texture_decoder_stb;

import :resource_manager_core;
import :file_system;

export class StbTextureDecoder final : public ITextureDecoder
{
	std::optional<TextureCPUData> Decode(const TextureProperties& properties, std::string_view resolvedPath)
	{
		namespace fs = std::filesystem;

		fs::path correctPath = fs::path(std::string(resolvedPath));
		if (!correctPath.is_absolute())
		{
			correctPath = corefs::ResolveAsset(correctPath);
		}

		if (!fs::exists(correctPath))
		{
			throw std::runtime_error("StbTextureDecoder couldn't find file: " + correctPath.string());
		}

		int width = 0, height = 0, comp = 0;

		// stbi_set_flip_vertically_on_load(1);

		stbi_uc* data = stbi_load(correctPath.string().c_str(), &width, &height, &comp, 0);
		if (!data)
		{
			throw std::runtime_error(std::string("stbi_load failed: ") + stbi_failure_reason());
		}

		TextureCPUData outputData;
		outputData.width = static_cast<std::uint32_t>(width);
		outputData.height = static_cast<std::uint32_t>(height);

		TextureFormat format = TextureFormat::RGBA;
		int channels = comp;

		switch (channels)
		{
		case 1:
			format = TextureFormat::GRAYSCALE;
		case 3:
			format = TextureFormat::RGB;
		case 4:
			format = TextureFormat::RGBA;
		default:
			stbi_image_free(data);
			data = stbi_load(correctPath.string().c_str(), &width, &height, &comp, 4);
			channels = 4;
			format = TextureFormat::RGBA;
			break;
		}

		if (!data)
		{
			throw std::runtime_error(std::string("stbi_load failed: ") + stbi_failure_reason());
		}

		outputData.channels = static_cast<std::uint32_t>(channels);
		outputData.format = format;

		const std::size_t size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels);
		outputData.pixels.resize(size);
		std::memcpy(outputData.pixels.data(), data, size);

		stbi_image_free(data);
		return outputData;
	}
};

