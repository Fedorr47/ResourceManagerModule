module;

#include <string>
#include <string_view>
#include <utility>
#include <filesystem>
#include <memory>
#include <cstddef>

export module core:asset_manager;

import :resource_manager;

// Mesh resource types live in rendern namespace.
import :resource_manager_mesh;

export class AssetManager
{
public:
	AssetManager(TextureIO& textureIO, rendern::MeshIO& meshIO)
		: textureIO_(&textureIO)
		, meshIO_(&meshIO)
	{}

	std::shared_ptr<TextureResource> LoadTextureAsync(std::string_view id, TextureProperties props)
	{
		return rm_.LoadAsync<TextureResource>(id, *textureIO_, std::move(props));
	}

	std::shared_ptr<TextureResource> LoadTextureSync(std::string_view id, TextureProperties props)
	{
		return rm_.LoadSync<TextureResource>(id, *textureIO_, std::move(props));
	}

	std::shared_ptr<rendern::MeshResource> LoadMeshAsync(std::string_view path)
	{
		rendern::MeshProperties p{};
		// Leave filePath empty: ResourceManager key will be treated as path.
		return rm_.LoadAsync<rendern::MeshResource>(path, *meshIO_, std::move(p));
	}

	std::shared_ptr<rendern::MeshResource> LoadMeshAsync(std::string_view id, rendern::MeshProperties props)
	{
		return rm_.LoadAsync<rendern::MeshResource>(id, *meshIO_, std::move(props));
	}

	std::shared_ptr<rendern::MeshResource> LoadMeshSync(std::string_view id, rendern::MeshProperties props = {})
	{
		return rm_.LoadSync<rendern::MeshResource>(id, *meshIO_, std::move(props));
	}

	// Drive GPU upload + destruction queues for all managed resource types.
	void ProcessUploads(std::size_t maxTexUploadsPerCall = 8,
		std::size_t maxTexDestroyedPerCall = 32,
		std::size_t maxMeshUploadsPerCall = 2,
		std::size_t maxMeshDestroyedPerCall = 32)
	{
		rm_.ProcessUploads<TextureResource>(*textureIO_, maxTexUploadsPerCall, maxTexDestroyedPerCall);
		rm_.ProcessUploads<rendern::MeshResource>(*meshIO_, maxMeshUploadsPerCall, maxMeshDestroyedPerCall);
	}

	void UnloadUnused()
	{
		rm_.UnloadUnused<TextureResource>();
		rm_.UnloadUnused<rendern::MeshResource>();
	}

	void ClearAll()
	{
		rm_.Clear<TextureResource>();
		rm_.Clear<rendern::MeshResource>();
	}

	ResourceManager& GetResourceManager() noexcept { return rm_; }
	const ResourceManager& GetResourceManager() const noexcept { return rm_; }

private:
	TextureIO* textureIO_{};
	rendern::MeshIO* meshIO_{};
	ResourceManager rm_{};
};
