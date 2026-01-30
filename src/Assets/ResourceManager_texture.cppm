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

export module core:resource_manager_texture;

import :resource_manager_core;

export using TextureResource = Texture<GPUTexture>;

export struct TextureEntry
{
	using Handle = std::shared_ptr<TextureResource>;

	Handle textureHandle;
	ResourceState state{ ResourceState::Unloaded };
	std::uint64_t generation{ 0 };
	std::optional<TextureCPUData> pendingCpu{};
	std::string error{};
};

struct TextureUploadTicket
{
	std::string id;
	std::uint64_t generation{};
};

export template <>
class ResourceStorage<TextureResource>
{
public:
	using Resource = TextureResource;
	using Handle = std::shared_ptr<Resource>;
	using Id = std::string;

	template <typename PropertiesType>
		requires std::same_as<std::remove_cvref_t<PropertiesType>, typename Resource::Properties>
	Handle LoadOrGet(std::string_view id, TextureIO& io, PropertiesType&& properties)
	{
		return LoadAsync(id, io, std::forward<PropertiesType>(properties));
	}

	template <typename PropertiesType>
		requires std::same_as<std::remove_cvref_t<PropertiesType>, typename Resource::Properties>
	Handle LoadAsync(std::string_view id, TextureIO& io, PropertiesType&& properties)
	{
		Id stableKey = Id{ id };
		Handle handle{};
		std::uint64_t generation{};

		{
			std::scoped_lock lock(mutex_);
			if (auto it = entries_.find(stableKey); it != entries_.end())
			{
				// If the resource exists, return it. If it previously failed, restart loading.
				TextureEntry& existing = it->second;
				handle = existing.textureHandle;
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
				TextureEntry entry{};
				entry.textureHandle = std::make_shared<Resource>(std::forward<PropertiesType>(properties));
				entry.state = ResourceState::Loading;
				entry.generation = 1;

				handle = entry.textureHandle;
				generation = entry.generation;
				entries_.emplace(stableKey, std::move(entry));
			}
		}

		TextureProperties propertiesCopy = handle->GetProperties();
		std::string path = propertiesCopy.filePath.empty() ? std::string(stableKey) : propertiesCopy.filePath;

		TextureIO ioCopy = io;

		ioCopy.jobs.Enqueue([this,
			key = stableKey,
			generation,
			propertiesCopy = std::move(propertiesCopy),
			path = std::move(path),
			ioCopy]() mutable
			{
				auto cpuOpt = ioCopy.decoder.Decode(propertiesCopy, path);

				std::scoped_lock lock(mutex_);

				auto it = entries_.find(key);
				if (it == entries_.end())
				{
					return;
				}

				TextureEntry& entry = it->second;
				if (entry.generation != generation)
				{
					return;
				}

				if (entry.state != ResourceState::Loading)
				{
					return;
				}

				if (!cpuOpt)
				{
					entry.state = ResourceState::Failed;
					entry.error = "Texture decode failed";
					entry.pendingCpu.reset();
					return;
				}

				entry.pendingCpu = std::move(*cpuOpt);
				uploadQueue_.push_back(TextureUploadTicket{ key, generation });
			});

		return handle;
	}

	template <typename PropertiesType>
		requires std::same_as<std::remove_cvref_t<PropertiesType>, typename Resource::Properties>
	Handle LoadSync(std::string_view id, TextureIO& io, PropertiesType&& properties)
	{
		auto handle = LoadAsync(id, io, std::forward<PropertiesType>(properties));

		for (;;)
		{
			auto state = GetState(id);

			if (state == ResourceState::Loaded || state == ResourceState::Failed)
			{
				return handle;
			}

			io.jobs.WaitIdle();

			ProcessUploads(io, SyncLoadNumberPerCall, SyncLoadNumberPerCall);
		}
	}

	Handle Find(std::string_view id) const
	{
		Id key{ id };
		std::scoped_lock lock(mutex_);

		if (auto it = entries_.find(key); it != entries_.end())
		{
			return it->second.textureHandle;
		}
		return {};
	}

	void UnloadUnused()
	{
		std::scoped_lock lock(mutex_);

		for (auto it = entries_.begin(); it != entries_.end(); )
		{
			if (it->second.textureHandle.use_count() == 1)
			{
				if (it->second.state == ResourceState::Loaded)
				{
					EnqueueDestroy(it->second.textureHandle->GetResource());
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
				EnqueueDestroy(entry.textureHandle->GetResource());
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
		static const std::string errorStr{};
		Id key{ id };
		std::scoped_lock lock(mutex_);

		if (auto it = entries_.find(key); it != entries_.end())
		{
			return it->second.error;
		}
		return errorStr;
	}

	bool ProcessUploads(TextureIO& io, std::size_t maxPerCall = 8, std::size_t maxDestroyedPerCall = 32)
	{
		std::size_t uploaded = 0;
		std::size_t destroyed = 0;

		while (destroyed < maxDestroyedPerCall)
		{
			GPUTexture gTexture{};
			{
				std::scoped_lock lock(mutex_);
				if (destroyQueue_.empty())
				{
					break;
				}

				gTexture = destroyQueue_.front();
				destroyQueue_.pop_front();
			}

			if (gTexture.id != 0)
			{
				TextureIO ioCopy = io;
				ioCopy.render.Enqueue([ioCopy, gTexture]()
					{
						ioCopy.uploader.Destroy(gTexture);
					});
			}

			++destroyed;
		}

		while (uploaded < maxPerCall)
		{
			TextureUploadTicket ticket{};
			TextureProperties properties{};
			Handle handle{};
			TextureCPUData cpuData{};
			std::uint64_t generation{};

			{
				std::scoped_lock lock(mutex_);

				if (uploadQueue_.empty())
				{
					break;
				}

				ticket = std::move(uploadQueue_.front());
				uploadQueue_.pop_front();

				auto it = entries_.find(ticket.id);
				if (it == entries_.end())
				{
					continue;
				}

				TextureEntry& entry = it->second;
				generation = ticket.generation;
				if (entry.generation != generation)
				{
					continue;
				}

				if (entry.state != ResourceState::Loading || !entry.pendingCpu.has_value())
				{
					continue;
				}

				cpuData = std::move(*entry.pendingCpu);
				entry.pendingCpu.reset();

				handle = entry.textureHandle;
				properties = handle->GetProperties();
			}

			auto cpuPtr = std::make_shared<TextureCPUData>(std::move(cpuData));
			TextureIO ioCopy = io;
			std::string idCopy = ticket.id;
			auto props = std::move(properties);
			const std::uint64_t genCopy = generation;

			ioCopy.render.Enqueue([this, ioCopy, idCopy = std::move(idCopy), genCopy, cpuPtr, props = std::move(props), handle]() mutable
				{
					auto gpuOpt = ioCopy.uploader.CreateAndUpload(*cpuPtr, props);
					GPUTexture toDestroy{};
					bool shouldDestroy = false;

					{
						std::scoped_lock lock(mutex_);

						auto it = entries_.find(idCopy);
						if (it == entries_.end())
						{
							shouldDestroy = (gpuOpt && gpuOpt->id != 0);
							if (shouldDestroy)
							{
								toDestroy = *gpuOpt;
							}
						}
						else
						{
							TextureEntry& entry = it->second;
							if (entry.generation != genCopy || entry.textureHandle != handle || entry.state != ResourceState::Loading)
							{
								shouldDestroy = (gpuOpt && gpuOpt->id != 0);
								if (shouldDestroy)
								{
									toDestroy = *gpuOpt;
								}
							}
							else if (!gpuOpt)
							{
								entry.state = ResourceState::Failed;
								entry.error = "GPU texture upload failed";
							}
							else
							{
								entry.textureHandle->SetResource(*gpuOpt);
								entry.state = ResourceState::Loaded;
								entry.error.clear();
							}
						}
					}

					if (shouldDestroy)
					{
						ioCopy.uploader.Destroy(toDestroy);
					}
				});

			++uploaded;
		}

		return (uploaded + destroyed) > 0;
	}

private:

	void EnqueueDestroy(GPUTexture texture)
	{
		if (texture.id == 0)
		{
			return;
		}
		std::scoped_lock lock(mutex_);
		destroyQueue_.push_back(texture);
	}

	mutable std::mutex mutex_{};
	std::unordered_map<Id, TextureEntry> entries_;
	std::deque<TextureUploadTicket> uploadQueue_;
	std::deque<GPUTexture> destroyQueue_;
};
