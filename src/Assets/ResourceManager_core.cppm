module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map>
#include <memory>
#include <concepts>
#include <type_traits>
#include <utility>
#include <stdexcept>
#include <functional>
#include <mutex>

export module core:resource_manager_core;

export constexpr int SyncLoadNumberPerCall = 64;

export enum class TextureFormat : uint8_t
{
	RGB,
	RGBA,
	GRAYSCALE
};

export enum class ResourceState : uint8_t
{
	Unloaded,
	Loading,
	Loaded,
	Failed,
	Unknown
};

export enum class LoadMode : uint8_t
{
	Async,
	Sync
};

export struct TextureProperties
{
	std::uint32_t width{};
	std::uint32_t height{};
	TextureFormat format{ TextureFormat::RGBA };
	std::string filePath{};
	bool srgb{ true };
	bool generateMips{ true };
};

export struct TextureCPUData
{
	std::uint32_t width{};
	std::uint32_t height{};
	int channels{};
	TextureFormat format{ TextureFormat::RGBA };
	std::vector<unsigned char> pixels;
};

export struct GPUTexture
{
	unsigned int id{};
};

export class ITextureDecoder
{
public:
	virtual ~ITextureDecoder() = default;
	virtual std::optional<TextureCPUData> Decode(const TextureProperties& properties, std::string_view resolvedPath) = 0;
};

export class ITextureUploader
{
public:
	virtual ~ITextureUploader() = default;
	virtual std::optional<GPUTexture> CreateAndUpload(const TextureCPUData& cpuData, const TextureProperties& properties) = 0;
	virtual void Destroy(GPUTexture texture) noexcept = 0;
};

export class IJobSystem
{
public:
	virtual ~IJobSystem() = default;
	virtual void Enqueue(std::function<void()> job) = 0;
	virtual void WaitIdle() = 0;
};

export class IRenderQueue
{
public:
	virtual ~IRenderQueue() = default;
	virtual void Enqueue(std::function<void()> job) = 0;
};

export struct TextureIO
{
	ITextureDecoder& decoder;
	ITextureUploader& uploader;
	IJobSystem& jobs;
	IRenderQueue& render;
};

export template <class Resource>
class Texture
{
public:

	using Properties = TextureProperties;

	Texture() = default;

	template <typename PropertiesType>
		requires std::same_as<std::remove_cvref_t<PropertiesType>, Properties>
	explicit Texture(PropertiesType&& inProperties) : properties_(std::forward<PropertiesType>(inProperties))
	{
	}

	const Properties& GetProperties() const { return properties_; }
	const Resource& GetResource() const { return resource_; }

	template<typename PropertiesType>
		requires std::same_as<std::remove_cvref_t<PropertiesType>, Properties>
	void SetProperties(PropertiesType&& inProperties)
	{
		properties_ = std::forward<PropertiesType>(inProperties);
	}

	template<typename ResourceType>
		requires std::same_as<std::remove_cvref_t<ResourceType>, Resource>
	void SetResource(ResourceType&& inResource)
	{
		resource_ = std::forward<ResourceType>(inResource);
	}

private:
	Resource resource_{};
	Properties properties_{};
};

template <typename T>
concept TexturePropertyType = requires(const T & propertyType)
{
	{ propertyType.width }  -> std::convertible_to<uint32_t>;
	{ propertyType.height } -> std::convertible_to<uint32_t>;
	{ propertyType.format } -> std::convertible_to<TextureFormat>;
};

template<typename T>
struct ResourceTraits;

template <typename ResourceType>
class ResourceStorage
{
public:
	using Handle = std::shared_ptr<ResourceType>;
	using WeakHandle = std::weak_ptr<ResourceType>;
	using Id = std::string;

	template <typename... Args>
	Handle LoadOrGet(std::string_view id, Args&&... args)
		requires requires(const Id& sid)
	{
		{ ResourceTraits<ResourceType>::Load(sid, std::forward<Args>(args)...) } ->std::same_as<Handle>;
	}
	{
		Id key{ id };

		// Fast path: return an alive cached resource.
		{
			std::scoped_lock lock(mutex_);
			if (auto it = cache_.find(key); it != cache_.end())
			{
				if (auto alive = it->second.lock())
				{
					return alive;
				}
				cache_.erase(it);
			}
		}

		// Slow path: load outside the lock.
		Handle resource = ResourceTraits<ResourceType>::Load(key, std::forward<Args>(args)...);

		{
			std::scoped_lock lock(mutex_);
			cache_[std::move(key)] = resource;
		}
		return resource;
	}

	Handle Find(const Id& id) const
	{
		std::scoped_lock lock(mutex_);
		if (auto it = cache_.find(id); it != cache_.end())
		{
			return it->second.lock();
		}
		return {};
	}

	void UnloadUnused()
	{
		std::scoped_lock lock(mutex_);
		for (auto it = cache_.begin(); it != cache_.end(); )
		{
			if (it->second.expired())
			{
				it = cache_.erase(it);
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
		cache_.clear();
	}

private:
	mutable std::mutex mutex_{};
	std::unordered_map<Id, WeakHandle> cache_;
};

export class ResourceManager
{
public:
	using Id = std::string;

	template <typename T, typename... Args>
	std::shared_ptr<T> Load(std::string_view id, Args&&... argss)
	{
		return storage<T>().LoadOrGet(id, std::forward<Args>(argss)...);
	}

	template <typename T, typename... Args>
	std::shared_ptr<T> LoadAsync(std::string_view id, Args&&... args)
	{
		auto& stg = storage<T>();
		if constexpr (requires { stg.LoadAsync(id, std::forward<Args>(args)...); })
		{
			return stg.LoadAsync(id, std::forward<Args>(args)...);
		}
		else
		{
			return stg.LoadOrGet(id, std::forward<Args>(args)...);
		}
	}


	template <typename T, typename... Args>
	std::shared_ptr<T> LoadSync(std::string_view id, Args&&... args)
	{
		auto& stg = storage<T>();
		if constexpr (requires { stg.LoadSync(id, std::forward<Args>(args)...); })
		{
			return stg.LoadSync(id, std::forward<Args>(args)...);
		}
		else
		{
			return stg.LoadOrGet(id, std::forward<Args>(args)...);
		}
	}

	template <typename T>
	std::shared_ptr<T> Get(std::string_view id)
	{
		return storage<T>().Find(Id{ id });
	}

	// Optional helpers for storages that expose extended APIs.
	// These are intentionally template-based to avoid coupling the manager
	// to any particular resource type.
	template <typename T>
	ResourceState GetState(std::string_view id) const
	{
		const auto& stg = storage<T>();
		if constexpr (requires { stg.GetState(id); })
		{
			return stg.GetState(id);
		}
		return ResourceState::Unknown;
	}

	template <typename T>
	std::string_view GetError(std::string_view id) const
	{
		static constexpr std::string_view kEmpty{};
		const auto& stg = storage<T>();
		if constexpr (requires { stg.GetError(id); })
		{
			return stg.GetError(id);
		}
		return kEmpty;
	}

	template <typename T, typename IO, typename... Args>
	bool ProcessUploads(IO& io, Args&&... args)
	{
		auto& stg = storage<T>();
		if constexpr (requires { stg.ProcessUploads(io, std::forward<Args>(args)...); })
		{
			return stg.ProcessUploads(io, std::forward<Args>(args)...);
		}
		return false;
	}

	template <typename T>
	void UnloadUnused()
	{
		storage<T>().UnloadUnused();
	}

	template <typename T>
	void Clear()
	{
		storage<T>().Clear();
	}

	template <typename T>
	ResourceStorage<T>& GetStorage()
	{
		return storage<T>();
	}

private:
	template <typename T>
	static ResourceStorage<T>& StorageImpl()
	{
		static ResourceStorage<T> instance;
		return instance;
	}

	template <typename T>
	ResourceStorage<T>& storage()
	{
		return StorageImpl<T>();
	}

	template <typename T>
	const ResourceStorage<T>& storage() const
	{
		return StorageImpl<T>();
	}
};