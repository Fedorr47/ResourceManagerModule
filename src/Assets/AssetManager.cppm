module;

#include <string>
#include <string_view>
#include <utility>
#include <filesystem>
#include <memory>
#include <cstddef>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <optional>
#include <array>

export module core:asset_manager;

import :resource_manager;
import :file_system;

// Mesh resource types live in rendern namespace.
import :resource_manager_mesh;

namespace
{
	// Face order: +X, -X, +Y, -Y, +Z, -Z
	constexpr std::array<std::string_view, 6> kSuffix_rtlfupdnftbk = { "_rt", "_lf", "_up", "_dn", "_ft", "_bk" };
	constexpr std::array<std::string_view, 6> kSuffix_pxnxpynypznz = { "_px", "_nx", "_py", "_ny", "_pz", "_nz" };
	constexpr std::array<std::string_view, 6> kSuffix_rightleftupdownfrontback = { "_right", "_left", "_up", "_down", "_front", "_back" };

	inline bool EndsWith(std::string_view s, std::string_view suffix)
	{
		return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
	}

	inline std::filesystem::path ToAbsIfNeeded(const std::filesystem::path& p)
	{
		return p.is_absolute() ? p : corefs::ResolveAsset(p);
	}

	inline bool FileExists(const std::filesystem::path& maybeRel)
	{
		return std::filesystem::exists(ToAbsIfNeeded(maybeRel));
	}

	std::array<std::string, 6> ResolveCubemapFacesFromBase(std::filesystem::path basePath)
	{
		// basePath can be:
		// - "textures/skybox/cupertin-lake" (no extension)
		// - "textures/skybox/cupertin-lake.tga" (has extension)
		//
		// We try a few common suffix conventions + a few common extensions, and require all 6 faces to exist.

		const std::string explicitExt = basePath.has_extension() ? basePath.extension().string() : std::string{};
		std::filesystem::path baseNoExt = basePath;
		if (baseNoExt.has_extension())
		{
			baseNoExt.replace_extension();
		}

		const std::array<std::string_view, 4> exts = { ".tga", ".png", ".jpg", ".jpeg" };
		const std::array<const std::array<std::string_view, 6>*, 3> schemes = {
			&kSuffix_rtlfupdnftbk,
			&kSuffix_pxnxpynypznz,
			&kSuffix_rightleftupdownfrontback
		};

		for (const auto* scheme : schemes)
		{
			if (!explicitExt.empty())
			{
				std::array<std::string, 6> out{};
				bool ok = true;
				for (std::size_t face = 0; face < 6; ++face)
				{
					const std::filesystem::path rel = std::filesystem::path(baseNoExt.string() + std::string((*scheme)[face]) + explicitExt);
					if (!FileExists(rel))
					{
						ok = false;
						break;
					}
					out[face] = rel.string();
				}
				if (ok)
				{
					return out;
				}
			}
			else
			{
				for (std::string_view ext : exts)
				{
					std::array<std::string, 6> out{};
					bool ok = true;
					for (std::size_t face = 0; face < 6; ++face)
					{
						const std::filesystem::path rel = std::filesystem::path(baseNoExt.string() + std::string((*scheme)[face]) + std::string(ext));
						if (!FileExists(rel))
						{
							ok = false;
							break;
						}
						out[face] = rel.string();
					}
					if (ok)
					{
						return out;
					}
				}
			}
		}

		throw std::runtime_error(
			"AssetManager: can't resolve cubemap faces from base path '" + basePath.string() + "'. "
			"Expected one of the naming schemes: *_rt/_lf/_up/_dn/_ft/_bk OR *_px/_nx/_py/_ny/_pz/_nz, "
			"with extension .tga/.png/.jpg/.jpeg (or explicit extension in the base path).");
	}

	struct CubemapFaceGroup
	{
		std::array<std::filesystem::path, 6> relOrAbs{};
		std::array<bool, 6> has{};
	};

	std::optional<int> TryDetectFaceIndexByScheme(
		std::string_view stem,
		const std::array<std::string_view, 6>& scheme)
	{
		for (int i = 0; i < 6; ++i)
		{
			if (EndsWith(stem, scheme[static_cast<std::size_t>(i)]))
			{
				return i;
			}
		}
		return std::nullopt;
	}

	bool IsCompleteCubemapFaceGroup(const CubemapFaceGroup& g) noexcept
	{
		for (bool b : g.has)
		{
			if (!b)
			{
				return false;
			}
		}
		return true;
	}

	std::optional<int> DetectFaceIndexFromStem(std::string_view stem)
	{
		if (auto idx = TryDetectFaceIndexByScheme(stem, kSuffix_rtlfupdnftbk)) return idx;
		if (auto idx = TryDetectFaceIndexByScheme(stem, kSuffix_pxnxpynypznz)) return idx;
		if (auto idx = TryDetectFaceIndexByScheme(stem, kSuffix_rightleftupdownfrontback)) return idx;
		return std::nullopt;
	}

	std::string StripKnownFaceSuffix(std::string_view stem)
	{
		for (auto suf : kSuffix_rtlfupdnftbk) if (EndsWith(stem, suf)) return std::string(stem.substr(0, stem.size() - suf.size()));
		for (auto suf : kSuffix_pxnxpynypznz) if (EndsWith(stem, suf)) return std::string(stem.substr(0, stem.size() - suf.size()));
		for (auto suf : kSuffix_rightleftupdownfrontback) if (EndsWith(stem, suf)) return std::string(stem.substr(0, stem.size() - suf.size()));
		return std::string(stem);
	}

	std::array<std::string, 6> ResolveCubemapFacesFromDirectory(const std::filesystem::path& dirInput, std::optional<std::string_view> preferBase = std::nullopt)
	{
		// dirInput can be relative-to-assets or absolute.
		const std::filesystem::path absDir = ToAbsIfNeeded(dirInput);
		if (!std::filesystem::is_directory(absDir))
		{
			throw std::runtime_error("AssetManager: '" + absDir.string() + "' is not a directory");
		}

		std::unordered_map<std::string, CubemapFaceGroup> groups;

		for (const auto& it : std::filesystem::directory_iterator(absDir))
		{
			if (!it.is_regular_file())
				continue;

			const std::filesystem::path file = it.path();
			const std::string stem = file.stem().string(); // without extension
			const auto faceIdx = DetectFaceIndexFromStem(stem);
			if (!faceIdx)
				continue;

			const std::string base = StripKnownFaceSuffix(stem);
			auto& g = groups[base];
			g.relOrAbs[static_cast<std::size_t>(*faceIdx)] =
				(dirInput.is_absolute())
				? file
				: (dirInput / file.filename()); // keep relative if input was relative
			g.has[static_cast<std::size_t>(*faceIdx)] = true;
		}

		// Preferred base
		if (preferBase)
		{
			auto it = groups.find(std::string(*preferBase));
			if (it != groups.end() && IsCompleteCubemapFaceGroup(it->second))
			{
				std::array<std::string, 6> out{};
				for (int i = 0; i < 6; ++i)
				{
					out[static_cast<std::size_t>(i)] = it->second.relOrAbs[static_cast<std::size_t>(i)].string();
				}
				return out;
			}
		}

		// First complete group
		for (auto& [base, g] : groups)
		{
			if (!IsCompleteCubemapFaceGroup(g))
				continue;

			std::array<std::string, 6> out{};
			for (int i = 0; i < 6; ++i)
			{
				out[static_cast<std::size_t>(i)] = g.relOrAbs[static_cast<std::size_t>(i)].string();
			}
			return out;
		}

		throw std::runtime_error(
			"AssetManager: can't find a complete cubemap set in directory '" + absDir.string() + "'. "
			"Expected files with suffixes _rt/_lf/_up/_dn/_ft/_bk or _px/_nx/_py/_ny/_pz/_nz.");
	}

	inline std::array<std::string, 6> ResolveCubemapFaces(std::string_view baseOrDir)
	{
		const std::filesystem::path p = std::filesystem::path(std::string(baseOrDir));
		const std::filesystem::path abs = ToAbsIfNeeded(p);

		if (std::filesystem::is_directory(abs))
		{
			return ResolveCubemapFacesFromDirectory(p);
		}

		return ResolveCubemapFacesFromBase(p);
	}
} // namespace

export struct AssetStreamingStats
{
	ResourceStreamingStats textures{};
	ResourceStreamingStats meshes{};
	ResourceStreamingStats total{};

	[[nodiscard]] bool HasPendingWork() const noexcept
	{
		return total.HasPendingWork();
	}

	[[nodiscard]] float Completion01() const noexcept
	{
		return total.Completion01();
	}
};

export class AssetManager
{
public:
	AssetManager(TextureIO& textureIO, rendern::MeshIO& meshIO)
		: textureIO_(&textureIO)
		, meshIO_(&meshIO)
	{
	}

	std::shared_ptr<TextureResource> LoadTextureAsync(std::string_view id, TextureProperties props)
	{
		return LoadTexture_(id, std::move(props), false);
	}

	std::shared_ptr<TextureResource> LoadTextureSync(std::string_view id, TextureProperties props)
	{
		return LoadTexture_(id, std::move(props), true);
	}

	std::shared_ptr<TextureResource> LoadTextureCubeAsync(std::string_view id, std::string_view baseOrDir, TextureProperties props = {})
	{
		return LoadTexture_(id, PrepareCubeTexturePropsFromBaseOrDir_(baseOrDir, std::move(props)), false);
	}

	std::shared_ptr<TextureResource> LoadTextureCubeSync(std::string_view id, std::string_view baseOrDir, TextureProperties props = {})
	{
		return LoadTexture_(id, PrepareCubeTexturePropsFromBaseOrDir_(baseOrDir, std::move(props)), true);
	}

	std::shared_ptr<TextureResource> LoadTextureCubeAsync(std::string_view id, std::string_view dir, std::string_view preferBase, TextureProperties props = {})
	{
		return LoadTexture_(id, PrepareCubeTexturePropsFromDirectory_(dir, preferBase, std::move(props)), false);
	}

	std::shared_ptr<TextureResource> LoadTextureCubeSync(std::string_view id, std::string_view dir, std::string_view preferBase, TextureProperties props = {})
	{
		return LoadTexture_(id, PrepareCubeTexturePropsFromDirectory_(dir, preferBase, std::move(props)), true);
	}

	std::shared_ptr<TextureResource> LoadTextureCubeAsync(std::string_view id, const std::array<std::string, 6>& facePaths, TextureProperties props = {})
	{
		return LoadTexture_(id, PrepareCubeTexturePropsFromFaces_(facePaths, std::move(props)), false);
	}

	std::shared_ptr<TextureResource> LoadTextureCubeSync(std::string_view id, const std::array<std::string, 6>& facePaths, TextureProperties props = {})
	{
		return LoadTexture_(id, PrepareCubeTexturePropsFromFaces_(facePaths, std::move(props)), true);
	}

	std::shared_ptr<rendern::MeshResource> LoadMeshAsync(std::string_view path)
	{
		rendern::MeshProperties p{};
		// Leave filePath empty: ResourceManager key will be treated as path.
		return LoadMesh_(path, std::move(p), false);
	}

	std::shared_ptr<rendern::MeshResource> LoadMeshAsync(std::string_view id, rendern::MeshProperties props)
	{
		return LoadMesh_(id, std::move(props), false);
	}

	std::shared_ptr<rendern::MeshResource> LoadMeshSync(std::string_view id, rendern::MeshProperties props = {})
	{
		return LoadMesh_(id, std::move(props), true);
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

	AssetStreamingStats GetStreamingStats() const
	{
		AssetStreamingStats stats{};
		stats.textures = rm_.GetStorage<TextureResource>().GetStreamingStats();
		stats.meshes = rm_.GetStorage<rendern::MeshResource>().GetStreamingStats();

		stats.total.totalEntries = stats.textures.totalEntries + stats.meshes.totalEntries;
		stats.total.loadingEntries = stats.textures.loadingEntries + stats.meshes.loadingEntries;
		stats.total.loadedEntries = stats.textures.loadedEntries + stats.meshes.loadedEntries;
		stats.total.failedEntries = stats.textures.failedEntries + stats.meshes.failedEntries;
		stats.total.pendingCpuEntries = stats.textures.pendingCpuEntries + stats.meshes.pendingCpuEntries;
		stats.total.queuedUploads = stats.textures.queuedUploads + stats.meshes.queuedUploads;
		stats.total.queuedDestroys = stats.textures.queuedDestroys + stats.meshes.queuedDestroys;
		return stats;
	}

	ResourceManager& GetResourceManager() noexcept { return rm_; }
	const ResourceManager& GetResourceManager() const noexcept { return rm_; }

private:
	std::shared_ptr<TextureResource> LoadTexture_(
		std::string_view id,
		TextureProperties props,
		bool sync)
	{
		if (sync)
		{
			return rm_.LoadSync<TextureResource>(id, *textureIO_, std::move(props));
		}
		return rm_.LoadAsync<TextureResource>(id, *textureIO_, std::move(props));
	}

	std::shared_ptr<rendern::MeshResource> LoadMesh_(
		std::string_view id,
		rendern::MeshProperties props,
		bool sync)
	{
		if (sync)
		{
			return rm_.LoadSync<rendern::MeshResource>(id, *meshIO_, std::move(props));
		}
		return rm_.LoadAsync<rendern::MeshResource>(id, *meshIO_, std::move(props));
	}

	static TextureProperties PrepareCubeTexturePropsFromBaseOrDir_(
		std::string_view baseOrDir,
		TextureProperties props)
	{
		props.dimension = TextureDimension::Cube;
		if (props.filePath.empty())
		{
			props.filePath = std::string(baseOrDir);
		}
		props.cubeFacePaths = ResolveCubemapFaces(baseOrDir);
		return props;
	}

	static TextureProperties PrepareCubeTexturePropsFromDirectory_(
		std::string_view dir,
		std::string_view preferBase,
		TextureProperties props)
	{
		props.dimension = TextureDimension::Cube;
		if (props.filePath.empty())
		{
			props.filePath = std::string(dir);
		}
		props.cubeFacePaths = ResolveCubemapFacesFromDirectory(
			std::filesystem::path(std::string(dir)),
			preferBase);
		return props;
	}

	static TextureProperties PrepareCubeTexturePropsFromFaces_(
		const std::array<std::string, 6>& facePaths,
		TextureProperties props)
	{
		props.dimension = TextureDimension::Cube;
		props.cubeFacePaths = facePaths;
		return props;
	}

private:
	TextureIO* textureIO_{};
	rendern::MeshIO* meshIO_{};
	ResourceManager rm_{};
};