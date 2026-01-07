module;

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <string>
#include <concepts>
#include <unordered_map>
#include <memory>
#include <type_traits>
#include <utility>

export module texture_manager;

export enum class TextureFormat : uint8_t 
{
	RGB,
	RGBA,
	GRAYSCALE
};

template <typename T>
concept TexturePropertyType = requires(const T& propertyType)
{
	{ propertyType.width }  -> std::convertible_to<uint32_t>;
	{ propertyType.height } -> std::convertible_to<uint32_t>;
	propertyType.format;
};

template<typename T>
struct TextureTraits;

export class DefaultTexture {};

template <typename T>
concept TextureType = requires(const T & textureType)
{
	typename T::properties_;
	requires TexturePropertyType<typename T::Properties>;
	{ textureType.GetProperties() } -> std::same_as<const typename T::Properties&>;
};

export struct TextureProperties
{
	uint32_t width{};
	uint32_t height{};
	TextureFormat format{};
	std::string filePath{};
};

template <>
struct TextureTraits<DefaultTexture>
{
	using Properties = TextureProperties;
};

export template <class Resource, class Traits = TextureTraits<Resource>>
class Texture
{
public:

	using Properties = typename Traits::Properties;

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

export template <typename TextureType>
class TextureManager
{
public:

	using Id = std::string;
	using TextureT = ::Texture<TextureType>;
	using Handle = std::shared_ptr<TextureT>;

	template<typename PropertiesType>
		requires std::same_as<std::remove_cvref_t<PropertiesType>, TextureT::Properties>
	Handle LoadTexture(const Id& id, PropertiesType&& properties)
	{
		auto texture = std::make_shared<TextureT>(std::forward<PropertiesType>(properties));
		textures_[id] = texture;
		return texture;
	}

	bool Contains(const Id& id) const
	{
		return textures_.contains(id);
	}

	Handle Get(const Id& id) const
	{
		auto it = textures_.find(id);
		if (it != textures_.end())
		{
			return it->second;
		}
		return {};
	}

	void UnloadTexture(const Id& id)
	{
		textures_.erase(id);
	}
	
private:
	std::unordered_map<Id, Handle> textures_{};
};