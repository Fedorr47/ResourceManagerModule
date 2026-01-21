module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <functional>

export module core:render_core;

import :resource_manager;
import :rhi;
import :shader_files;
import :file_system;

export namespace rendern
{
	struct FrameContext
	{
		std::uint64_t frameIndex{};
	};

	struct ShaderKey
	{
		rhi::ShaderStage stage{ rhi::ShaderStage::Vertex };
		std::string name;
		std::string filePath;
		std::vector<std::string> defines;

		friend bool operator==(const ShaderKey& lhs, const ShaderKey& rhs)
		{
			return 
				lhs.stage == rhs.stage
				&& lhs.name == rhs.name
				&& lhs.filePath == rhs.filePath
				&& lhs.defines == rhs.defines;
		}
	};

	struct ShaderKeyHash
	{
		std::size_t operator()(const ShaderKey& key) const noexcept
		{
			std::size_t h1 = std::hash<std::string>{}(key.name);
			std::size_t h2 = 0;
			for (const auto& def : key.defines)
			{
				h2 ^= std::hash<std::string>{}(def) + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
			}
			return h1 ^ (h2 << 1);
		}
	};

	class ShaderLibrary
	{
	public:
		explicit ShaderLibrary(rhi::IRHIDevice& device) : device_(device) {}

		rhi::ShaderHandle GetOrCreateShader(const ShaderKey& key)
		{
			
			if (auto it = shaderCache_.find(key); it != shaderCache_.end())
			{
				return it->second;
			}

			const auto textSource = FILE_UTILS::LoadTextFile(std::filesystem::path(key.filePath));
			rhi::ShaderHandle shader = device_.CreateShader(key.stage, key.name, textSource.text);
			shaderCache_.emplace(key, shader);
			return shader;
		}

		void ClearCache()
		{
			for (const auto& [key, shader] : shaderCache_)
			{
				device_.DestroyShader(shader);
			}
			shaderCache_.clear();
		}

	private:
		rhi::IRHIDevice& device_;
		std::unordered_map<ShaderKey, rhi::ShaderHandle, ShaderKeyHash> shaderCache_;
	};

	class PSOCache
	{
	public:
		explicit PSOCache(rhi::IRHIDevice& device) : device_(device) {}

		rhi::PipelineHandle GetOrCreate(std::string_view name, rhi::ShaderHandle vertexShader, rhi::ShaderHandle fragmentShader)
		{
			const std::string key = std::string(name) + "_" + std::to_string(vertexShader.id) + "_" + std::to_string(fragmentShader.id);

			if (auto it = psoCache_.find(key); it != psoCache_.end())
			{
				return it->second;
			}

			rhi::PipelineHandle pipeline = device_.CreatePipeline(name, vertexShader, fragmentShader);
			psoCache_.emplace(key, pipeline);
			return pipeline;
		}

		void ClearCache()
		{
			for (const auto& [key, pipeline] : psoCache_)
			{
				device_.DestroyPipeline(pipeline);
			}
			psoCache_.clear();
		}

	private:
		rhi::IRHIDevice& device_;
		std::unordered_map<std::string, rhi::PipelineHandle> psoCache_;
	};

	class RenderQueueImmediate final : public IRenderQueue
	{
	public:
		void Enqueue(std::function<void()> job) override
		{
			job();
		}
	};

	class JobSystemImmediate final : public IJobSystem
	{
	public:
		void Enqueue(std::function<void()> job)
		{
			job();
		}

		void WaitIdle()
		{
			// No-op for immediate execution
		}
	};

	class NullTextureUploader final : public ITextureUploader
	{
	public:
		explicit NullTextureUploader(rhi::IRHIDevice& device) : device_(device) {}

		std::optional<GPUTexture> CreateAndUpload([[maybe_unused]] const TextureCPUData& cpuData, const TextureProperties& properties) override
		{
			GPUTexture gpuTexture{};

			const rhi::Extent2D extent{
				properties.width,
				properties.height
			};

			rhi::TextureHandle textureHandle = device_.CreateTexture2D(extent, rhi::Format::RGBA8_UNORM);
			gpuTexture.id = textureHandle.id;
			return gpuTexture;
		}

		void Destroy(GPUTexture texture) noexcept override
		{
			rhi::TextureHandle handle{};
			handle.id = texture.id;
			device_.DestroyTexture(handle);
		}

	private:
		rhi::IRHIDevice& device_;
	};

	class UploadSystem
	{
	public:
		explicit UploadSystem(rhi::IRHIDevice& device) : device_(device) {}

		void BeginFrame(FrameContext&)
		{
			// Initialize upload resources if needed
		}

		void EndFrame(FrameContext&)
		{
			// Clean up upload resources if needed
		}

	private:
		rhi::IRHIDevice& device_;
	};
}