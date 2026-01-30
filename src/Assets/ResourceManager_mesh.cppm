module;

#include <deque>
#include <cstdint>
#include <utility>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <concepts>
#include <type_traits>
#include <filesystem>
#include <stdexcept>
#include <functional>

export module core:resource_manager_mesh;

import :resource_manager_core;
import :mesh;
import :obj_loader;
import :file_system;

// NOTE: Mesh loading is CPU-side (ObjLoader) and does NOT touch the renderer.
// GPU upload/destruction is deferred via IRenderQueue (same pattern as textures).

export namespace rendern
{
	// ------------------------------------------------------------
	// Public API
	// ------------------------------------------------------------
	export struct MeshProperties
	{
		// If empty, the ResourceManager key is treated as the file path.
		std::string filePath{};

		// Optional human-readable name (used for debug labels where available).
		std::string debugName{};
	};

	export class MeshResource
	{
	public:
		using Properties = MeshProperties;

		MeshResource() = default;

		template <typename PropertiesType>
			requires std::same_as<std::remove_cvref_t<PropertiesType>, Properties>
		explicit MeshResource(PropertiesType&& inProperties)
			: properties_(std::forward<PropertiesType>(inProperties))
		{}

		const Properties& GetProperties() const noexcept { return properties_; }
		const MeshRHI& GetResource() const noexcept { return resource_; }

		template <typename PropertiesType>
			requires std::same_as<std::remove_cvref_t<PropertiesType>, Properties>
		void SetProperties(PropertiesType&& inProperties)
		{
			properties_ = std::forward<PropertiesType>(inProperties);
		}

		// Replace the GPU resource and return the previous value.
		MeshRHI ReplaceResource(MeshRHI&& inResource) noexcept
		{
			MeshRHI old = std::move(resource_);
			resource_ = std::move(inResource);
			return old;
		}

	private:
		MeshRHI resource_{};
		Properties properties_{};
	};

	export struct MeshIO
	{
		rhi::IRHIDevice& device;
		IJobSystem& jobs;
		IRenderQueue& render;
	};
}

namespace rendern
{
	struct MeshEntry
	{
		using Handle = std::shared_ptr<MeshResource>;

		Handle meshHandle;
		ResourceState state{ ResourceState::Unloaded };
		std::uint64_t generation{ 0 };
		std::optional<MeshCPU> pendingCpu{};
		std::string error{};
	};

	struct MeshUploadTicket
	{
		std::string id;
		std::uint64_t generation{};
	};

	std::string DefaultDebugNameFromPath(std::string_view path)
	{
		try
		{
			std::filesystem::path p{ std::string(path) };
			return p.filename().string();
		}
		catch (...)
		{
			return std::string(path);
		}
	}
} // namespace rendern

export template <>
	class ResourceStorage<rendern::MeshResource>
	{
	public:
		using Resource = rendern::MeshResource;
		using MeshIO = rendern::MeshIO;
		using MeshProperties = rendern::MeshProperties;
		using MeshRHI = rendern::MeshRHI;
		using MeshCPU = rendern::MeshCPU;
		using MeshEntry = rendern::MeshEntry;
		using MeshUploadTicket = rendern::MeshUploadTicket;
		using Handle = std::shared_ptr<Resource>;
		using Id = std::string;

		template <typename PropertiesType>
			requires std::same_as<std::remove_cvref_t<PropertiesType>, typename Resource::Properties>
		Handle LoadOrGet(std::string_view id, MeshIO& io, PropertiesType&& properties)
		{
			return LoadAsync(id, io, std::forward<PropertiesType>(properties));
		}

		template <typename PropertiesType>
			requires std::same_as<std::remove_cvref_t<PropertiesType>, typename Resource::Properties>
		Handle LoadAsync(std::string_view id, MeshIO& io, PropertiesType&& properties)
		{
			Id stableKey = Id{ id };
			Handle handle{};
			std::uint64_t generation{};

			{
				std::scoped_lock lock(mutex_);
				if (auto it = entries_.find(stableKey); it != entries_.end())
				{
					MeshEntry& existing = it->second;
					handle = existing.meshHandle;

					// If already loading/loaded, just return.
					if (existing.state != ResourceState::Failed)
					{
						return handle;
					}

					// Restart failed load.
					existing.state = ResourceState::Loading;
					existing.error.clear();
					existing.pendingCpu.reset();
					++existing.generation;
					generation = existing.generation;
					handle->SetProperties(std::forward<PropertiesType>(properties));
				}
				else
				{
					MeshEntry entry{};
					entry.meshHandle = std::make_shared<Resource>(std::forward<PropertiesType>(properties));
					entry.state = ResourceState::Loading;
					entry.generation = 1;
					handle = entry.meshHandle;
					generation = entry.generation;
					entries_.emplace(stableKey, std::move(entry));
				}
			}

			MeshProperties propsCopy = handle->GetProperties();
			std::string path = propsCopy.filePath.empty() ? std::string(stableKey) : propsCopy.filePath;
			if (propsCopy.debugName.empty())
			{
				propsCopy.debugName = rendern::DefaultDebugNameFromPath(path);
			}

			MeshIO ioCopy = io;

			ioCopy.jobs.Enqueue([this,
				key = stableKey,
				generation,
				propsCopy = std::move(propsCopy),
				path = std::move(path),
				ioCopy]() mutable
				{
					std::optional<MeshCPU> cpuOpt;
					std::string error;
					try
					{
						// Resolve via assets/ root unless absolute.
						const auto abs = corefs::ResolveAsset(std::filesystem::path(path));
						cpuOpt = rendern::LoadObj(abs);
					}
					catch (const std::exception& e)
					{
						error = e.what();
					}
					catch (...)
					{
						error = "Unknown mesh load error";
					}

					std::scoped_lock lock(mutex_);
					auto it = entries_.find(key);
					if (it == entries_.end())
					{
						return;
					}

					MeshEntry& entry = it->second;
					if (entry.generation != generation)
					{
						// Outdated request.
						return;
					}

					if (!cpuOpt)
					{
						entry.state = ResourceState::Failed;
						entry.error = std::move(error);
						return;
					}

					entry.pendingCpu = std::move(*cpuOpt);
					uploadQueue_.push_back(MeshUploadTicket{ key, generation });
				});

			return handle;
		}

		template <typename PropertiesType>
			requires std::same_as<std::remove_cvref_t<PropertiesType>, typename Resource::Properties>
		Handle LoadSync(std::string_view id, MeshIO& io, PropertiesType&& properties)
		{
			Handle h = LoadAsync(id, io, std::forward<PropertiesType>(properties));
			io.jobs.WaitIdle();
			for (int i = 0; i < 10; ++i)
			{
				// Drain both upload and destroy queues in a few iterations.
				ProcessUploads(io, SyncLoadNumberPerCall, SyncLoadNumberPerCall);
			}
			return h;
		}

		Handle Find(std::string_view id) const
		{
			Id key{ id };
			std::scoped_lock lock(mutex_);
			if (auto it = entries_.find(key); it != entries_.end())
			{
				return it->second.meshHandle;
			}
			return {};
		}

		void UnloadUnused()
		{
			std::scoped_lock lock(mutex_);
			for (auto it = entries_.begin(); it != entries_.end(); )
			{
				if (it->second.meshHandle.use_count() == 1)
				{
					if (it->second.state == ResourceState::Loaded)
					{
						MeshRHI old = it->second.meshHandle->ReplaceResource(MeshRHI{});
						EnqueueDestroy(std::move(old));
					}
					it = entries_.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		void Clear()
		{
			std::scoped_lock lock(mutex_);
			for (auto& [id, entry] : entries_)
			{
				if (entry.state == ResourceState::Loaded)
				{
					MeshRHI old = entry.meshHandle->ReplaceResource(MeshRHI{});
					EnqueueDestroy(std::move(old));
				}
			}
			entries_.clear();
			uploadQueue_.clear();
		}

		ResourceState GetState(std::string_view id) const
		{
			Id key{ id };
			std::scoped_lock lock(mutex_);
			if (auto it = entries_.find(key); it != entries_.end())
			{
				return it->second.state;
			}
			return ResourceState::Unknown;
		}

		const std::string& GetError(std::string_view id) const
		{
			static const std::string empty{};
			Id key{ id };
			std::scoped_lock lock(mutex_);
			if (auto it = entries_.find(key); it != entries_.end())
			{
				return it->second.error;
			}
			return empty;
		}

		bool ProcessUploads(MeshIO& io, std::size_t maxPerCall = 2, std::size_t maxDestroyedPerCall = 32)
		{
			std::size_t uploaded = 0;
			std::size_t destroyed = 0;

			while (destroyed < maxDestroyedPerCall)
			{
				MeshRHI mesh{};
				{
					std::scoped_lock lock(mutex_);
					if (destroyQueue_.empty())
						break;
					mesh = std::move(destroyQueue_.front());
					destroyQueue_.pop_front();
				}

				// NOTE: we always destroy on render queue / device-owner thread.
				if (mesh.vertexBuffer.id != 0 || mesh.indexBuffer.id != 0)
				{
					MeshIO ioCopy = io;
					ioCopy.render.Enqueue([ioCopy, mesh = std::move(mesh)]() mutable
						{
							DestroyMesh(ioCopy.device, mesh);
						});
				}
				++destroyed;
			}

			while (uploaded < maxPerCall)
			{
				MeshUploadTicket ticket{};
				MeshCPU cpu{};
				Handle handle{};
				MeshProperties props{};

				{
					std::scoped_lock lock(mutex_);
					if (uploadQueue_.empty())
						break;

					ticket = std::move(uploadQueue_.front());
					uploadQueue_.pop_front();

					auto it = entries_.find(ticket.id);
					if (it == entries_.end())
						continue;

					MeshEntry& entry = it->second;
					if (entry.generation != ticket.generation)
						continue;

					if (!entry.pendingCpu)
						continue;

					cpu = std::move(*entry.pendingCpu);
					entry.pendingCpu.reset();
					handle = entry.meshHandle;
					props = handle->GetProperties();
					if (props.debugName.empty())
					{
						props.debugName = rendern::DefaultDebugNameFromPath(props.filePath.empty() ? ticket.id : props.filePath);
					}
				}

				auto cpuPtr = std::make_shared<MeshCPU>(std::move(cpu));
				MeshIO ioCopy = io;

				ioCopy.render.Enqueue([this,
					id = ticket.id,
					generation = ticket.generation,
					cpuPtr,
					props = std::move(props),
					ioCopy]() mutable
					{
						MeshRHI gpu{};
						try
						{
							gpu = UploadMesh(ioCopy.device, *cpuPtr, props.debugName);
						}
						catch (const std::exception& e)
						{
							// Mark failure (we are already on render queue thread).
							std::scoped_lock lock(mutex_);
							auto it = entries_.find(id);
							if (it != entries_.end() && it->second.generation == generation)
							{
								it->second.state = ResourceState::Failed;
								it->second.error = e.what();
							}
							return;
						}
						catch (...)
						{
							std::scoped_lock lock(mutex_);
							auto it = entries_.find(id);
							if (it != entries_.end() && it->second.generation == generation)
							{
								it->second.state = ResourceState::Failed;
								it->second.error = "Unknown GPU upload error";
							}
							return;
						}

						// Commit if still актуально.
						std::scoped_lock lock(mutex_);
						auto it = entries_.find(id);
						if (it == entries_.end() || it->second.generation != generation)
						{
							DestroyMesh(ioCopy.device, gpu);
							return;
						}

						MeshEntry& entry = it->second;
						MeshRHI old = entry.meshHandle->ReplaceResource(std::move(gpu));
						if (old.vertexBuffer.id != 0 || old.indexBuffer.id != 0)
						{
							DestroyMesh(ioCopy.device, old);
						}
						entry.state = ResourceState::Loaded;
						entry.error.clear();
					});

				++uploaded;
			}

			return (uploaded > 0) || (destroyed > 0);
		}

	private:
		void EnqueueDestroy(MeshRHI&& mesh)
		{
			if (mesh.vertexBuffer.id == 0 && mesh.indexBuffer.id == 0)
				return;
			destroyQueue_.push_back(std::move(mesh));
		}

		mutable std::mutex mutex_{};
		std::unordered_map<Id, MeshEntry> entries_;
		std::deque<MeshUploadTicket> uploadQueue_;
		std::deque<MeshRHI> destroyQueue_;
	};
