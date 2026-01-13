#pragma once

#include <optional>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <cstdint>

import core;

struct FakeTextureDecoder final : ITextureDecoder
{
	bool succeed{ true };
	std::uint32_t nextWidth{ 4 };
	std::uint32_t nextHeight{ 4 };

	std::optional<TextureCPUData> Decode(
		const TextureProperties& properties, std::string_view) override
	{
		if (!succeed)
		{
			return std::nullopt;
		}

		const int channel =
			(properties.format == TextureFormat::GRAYSCALE) ? 1 :
			(properties.format == TextureFormat::RGB) ? 3 : 4;

		TextureCPUData cpuData{};
		cpuData.width = nextWidth;
		cpuData.height = nextHeight;
		cpuData.channels = channel;
		cpuData.format = properties.format;

		const std::size_t bytes = 
			static_cast<std::size_t>(cpuData.width) 
			* static_cast<std::size_t>(cpuData.height) 
			* static_cast<std::size_t>(cpuData.channels);

		cpuData.pixels.resize(bytes, 0xAB);

		return cpuData;
	}
};

struct FakeTextureUploader final : ITextureUploader
{
	bool succeed{ true };
	std::uint32_t nextId{ 1 };

	std::vector<std::uint32_t> createdIds{};
	std::vector<std::uint32_t> destroyedIds{};

	std::optional<GPUTexture> CreateAndUpload(
		const TextureCPUData& cpuData, const TextureProperties& properties) override
	{
		if (!succeed)
		{
			return std::nullopt;
		}

		if (cpuData.pixels.empty() || cpuData.width == 0 || cpuData.height == 0)
		{
			return std::nullopt;
		}

		GPUTexture texture{};
		texture.id = nextId++;

		createdIds.push_back(texture.id);
		return texture;
	}

	void Destroy(GPUTexture texture) noexcept override
	{
		if (texture.id != 0)
		{
			destroyedIds.push_back(texture.id);
		}
	}
};

struct FakeJobSystem final : IJobSystem
{
	void Enqueue(std::function<void()> job) override {};
	void WaitIdle() override {}
};

struct FakeRenderQueue final : IRenderQueue
{
	void Enqueue(std::function<void()> job) override {};
};

inline TextureIO MakeIO(FakeTextureDecoder& decoder, FakeTextureUploader& uploader, FakeJobSystem & jobSystem, FakeRenderQueue& renderQueue)
{
	return TextureIO{ decoder, uploader, jobSystem, renderQueue };
}