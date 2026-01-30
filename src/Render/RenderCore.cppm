module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <functional>
#include <cctype>
#include <thread>
#include <stop_token>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>

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
		static void HashCombine(std::size_t& h, std::size_t v) noexcept
		{
			h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
		}

		std::size_t operator()(const ShaderKey& key) const noexcept
		{
			std::size_t h = 0;

			HashCombine(h, std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(key.stage)));
			HashCombine(h, std::hash<std::string>{}(key.name));
			HashCombine(h, std::hash<std::string>{}(key.filePath));

			for (const auto& def : key.defines)
			{
				HashCombine(h, std::hash<std::string>{}(def));
			}

			return h;
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

			const std::filesystem::path path = std::filesystem::path(key.filePath);

			auto IsGLSL = [](std::filesystem::path p) -> bool
			{
				auto ext = p.extension().string();
				for (char& c : ext) c = (char)std::tolower((unsigned char)c);
				return ext == ".vert" || ext == ".frag" || ext == ".glsl";
			};

			auto ApplyDefinesToHLSL = [](std::string_view source, const std::vector<std::string>& defines) -> std::string
			{
				if (defines.empty())
				{
					return std::string(source);
				}

				std::string out;
				out.reserve(source.size() + defines.size() * 24);

				for (std::string def : defines)
				{
					auto eq = def.find('=');
					if (eq != std::string::npos)
					{
						def[eq] = ' ';
					}
					out += "#define ";
					out += def;
					out += "";
				}

				out.append(source);
				return out;
			};

			FILE_UTILS::TextFile textSource;
			std::string finalText;

			if (IsGLSL(path))
			{
				textSource = LoadGLSLWithIncludes(path);
				finalText = AppplyDefinesToGLSL(textSource.text, key.defines);
			}
			else
			{
				textSource = FILE_UTILS::LoadTextFile(path);
				finalText = ApplyDefinesToHLSL(textSource.text, key.defines);
			}

			rhi::ShaderHandle shader = device_.CreateShader(key.stage, key.name, finalText);
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
		void Enqueue(std::function<void()> job) override
		{
			job();
		}

		void WaitIdle() override
		{
			// No-op for immediate execution
		}
	};

	// Minimal thread-pool implementation for async CPU work (decoding, etc.).
	// GPU work must still be scheduled via IRenderQueue.
	class JobSystemThreadPool final : public IJobSystem
	{
	public:
		explicit JobSystemThreadPool(std::uint32_t workerCount = 1)
		{
			if (workerCount == 0)
				workerCount = 1;

			workers_.reserve(workerCount);
			for (std::uint32_t i = 0; i < workerCount; ++i)
			{
				workers_.emplace_back([this](std::stop_token st) { Worker(st); });
			}
		}

		~JobSystemThreadPool() override
		{
			// Request stop & wake.
			{
				std::scoped_lock lock(mutex_);
				stopping_ = true;
			}
			cv_.notify_all();
			// jthread joins automatically.
		}

		void Enqueue(std::function<void()> job) override
		{
			{
				std::scoped_lock lock(mutex_);
				queue_.push_back(std::move(job));
			}
			cv_.notify_one();
		}

		void WaitIdle() override
		{
			std::unique_lock lock(mutex_);
			idleCv_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
		}

	private:
		void Worker(std::stop_token st)
		{
			while (!st.stop_requested())
			{
				std::function<void()> job;
				{
					std::unique_lock lock(mutex_);
					cv_.wait(lock, [this, &st] { return st.stop_requested() || stopping_ || !queue_.empty(); });
					if (st.stop_requested())
						break;
					if ((stopping_ || st.stop_requested()) && queue_.empty())
						break;
					if (queue_.empty())
						continue;
					job = std::move(queue_.front());
					queue_.pop_front();
					++active_;
				}

				try { job(); }
				catch (...) { /* swallow */ }

				{
					std::scoped_lock lock(mutex_);
					--active_;
					if (queue_.empty() && active_ == 0)
						idleCv_.notify_all();
				}
			}
		}

		std::mutex mutex_;
		std::condition_variable cv_;
		std::condition_variable idleCv_;
		std::deque<std::function<void()>> queue_;
		std::vector<std::jthread> workers_;
		std::size_t active_ = 0;
		bool stopping_ = false;
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