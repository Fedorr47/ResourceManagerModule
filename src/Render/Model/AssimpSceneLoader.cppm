module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <unordered_map>
#include <unordered_set>

export module core:assimp_scene_loader;

import :math_utils;
import :file_system;
import :scene;

export namespace rendern
{
    struct ImportedSubmeshInfo
    {
        std::uint32_t submeshIndex{ 0 };
        std::uint32_t materialIndex{ 0 };
        std::string name;
    };

    struct ImportedSceneNode
    {
        std::string name;
        int parent{ -1 };
        Transform localTransform{};
        std::vector<std::uint32_t> submeshes;
    };

    struct ImportedMaterialTextureRef
    {
        // For external textures:
        //   - scan-only mode: resolved normalized path (asset-relative if possible, otherwise absolute)
        //   - materialize mode: asset-relative path inside assets/imported/...
        //
        // For embedded textures:
        //   - scan-only mode: "*N"
        //   - materialize mode: exported asset-relative path inside assets/imported/...
        std::string path;
		bool embedded{ false }; // whether this texture was embedded in the model file (and thus needs export)
    };

    struct ImportedMaterialInfo
    {
        std::string name;
        std::optional<ImportedMaterialTextureRef> baseColor;
        std::optional<ImportedMaterialTextureRef> normal;
        std::optional<ImportedMaterialTextureRef> metallic;
        std::optional<ImportedMaterialTextureRef> roughness;
        std::optional<ImportedMaterialTextureRef> ao;
        std::optional<ImportedMaterialTextureRef> emissive;
    };

    struct ImportedModelScene
    {
        std::vector<ImportedSubmeshInfo> submeshes;
        std::vector<ImportedMaterialInfo> materials;
        std::vector<ImportedSceneNode> nodes;
    };

    inline mathUtils::Mat4 AiToMat4(const aiMatrix4x4& m)
    {
        mathUtils::Mat4 out{ 1.0f };
        out(0, 0) = m.a1; out(0, 1) = m.a2; out(0, 2) = m.a3; out(0, 3) = m.a4;
        out(1, 0) = m.b1; out(1, 1) = m.b2; out(1, 2) = m.b3; out(1, 3) = m.b4;
        out(2, 0) = m.c1; out(2, 1) = m.c2; out(2, 2) = m.c3; out(2, 3) = m.c4;
        out(3, 0) = m.d1; out(3, 1) = m.d2; out(3, 2) = m.d3; out(3, 3) = m.d4;
        return out;
    }
        
    std::filesystem::path NormalizeLexically(const std::filesystem::path& p)
    {
        std::error_code ec;
        const auto weak = std::filesystem::weakly_canonical(p, ec);
        if (!ec)
        {
            return weak;
        }
        return p.lexically_normal();
    }

    std::string NormalizeNameForCompare(std::string_view s)
    {
        std::filesystem::path p{ std::string(s) };
        std::string out = p.filename().string();
        if (out.empty())
        {
            out = std::string(s);
        }

        for (char& c : out)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    std::string MakeAssetRelativePath(const std::filesystem::path& absolutePath)
    {
        namespace fs = std::filesystem;
        const fs::path assetRoot = NormalizeLexically(corefs::FindAssetRoot());
        const fs::path absNorm = NormalizeLexically(absolutePath);

        std::error_code ec;
        fs::path rel = fs::relative(absNorm, assetRoot, ec);
        if (!ec && !rel.empty())
        {
            return rel.generic_string();
        }

        return absNorm.generic_string();
    }

    std::optional<std::filesystem::path> TryResolveTexturePath(
        const std::filesystem::path& modelAbsPath,
        std::string_view rawTexturePath)
    {
        namespace fs = std::filesystem;
        if (rawTexturePath.empty())
        {
            return std::nullopt;
        }

        fs::path raw{ std::string(rawTexturePath) };

        std::vector<fs::path> candidates;
        if (raw.is_absolute())
        {
            candidates.push_back(raw);
        }
        else
        {
            const fs::path modelDir = modelAbsPath.parent_path();
            const fs::path assetRoot = corefs::FindAssetRoot();
            const std::string modelStem = modelAbsPath.stem().string();

            candidates.push_back(modelDir / raw);
            candidates.push_back(assetRoot / raw);

            if (raw.has_filename())
            {
                const fs::path filenameOnly = raw.filename();

                candidates.push_back(assetRoot / "textures" / filenameOnly);
                candidates.push_back(modelDir / filenameOnly);

                // FBX embedded-media / .fbm style fallback
                candidates.push_back(modelDir / (modelStem + ".fbm") / filenameOnly);
                candidates.push_back(assetRoot / "models" / (modelStem + ".fbm") / filenameOnly);
            }
        }

        for (const fs::path& c : candidates)
        {
            std::error_code ec;
            if (fs::exists(c, ec) && !ec)
            {
                return NormalizeLexically(c);
            }
        }
        return std::nullopt;
    }

    std::optional<ImportedMaterialTextureRef> ResolveExternalTextureRefNoCopy(
        const std::filesystem::path& modelAbsPath,
        std::string_view rawTexturePath)
    {
        const auto resolved = TryResolveTexturePath(modelAbsPath, rawTexturePath);
        if (!resolved)
        {
            return std::nullopt;
        }

        ImportedMaterialTextureRef out{};
        out.path = MakeAssetRelativePath(*resolved);
        out.embedded = false;
        return out;
    }

    std::optional<std::uint32_t> TryFindEmbeddedTextureIndex(
        const aiScene* scene,
        std::string_view rawTexturePath)
    {
        if (!scene || scene->mNumTextures == 0)
        {
            return std::nullopt;
        }

        const std::string wanted = NormalizeNameForCompare(rawTexturePath);
        if (!wanted.empty())
        {
            for (std::uint32_t i = 0; i < scene->mNumTextures; ++i)
            {
                const aiTexture* tex = scene->mTextures[i];
                if (!tex)
                {
                    continue;
                }

                const std::string texName = NormalizeNameForCompare(tex->mFilename.C_Str());
                if (!texName.empty() && texName == wanted)
                {
                    return i;
                }
            }
        }

        // Conservative fallback:
        // if the scene has exactly one embedded texture, use it.
        if (scene->mNumTextures == 1)
        {
            return 0u;
        }

        return std::nullopt;
    }

        std::string SanitizeFilename(std::string s)
        {
            for (char& c : s)
            {
                const bool ok =
                    (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') ||
                    c == '_' || c == '-' || c == '.';
                if (!ok)
                {
                    c = '_';
                }
            }
            if (s.empty())
            {
                s = "asset";
            }
            return s;
        }

        std::uint64_t HashStringStable(std::string_view s)
        {
            std::uint64_t h = 14695981039346656037ull;
            for (const unsigned char c : s)
            {
                h ^= static_cast<std::uint64_t>(c);
                h *= 1099511628211ull;
            }
            return h;
        }

        std::string Hex64(std::uint64_t value)
        {
            char buf[17]{};
            std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
            return std::string(buf);
        }

        std::string NormalizeExt(std::string ext);

        std::string MakeModelImportKey(const std::filesystem::path& modelAbsPath)
        {
            namespace fs = std::filesystem;
            const fs::path absNorm = NormalizeLexically(modelAbsPath);
            const fs::path assetRoot = NormalizeLexically(corefs::FindAssetRoot());

            std::error_code ec;
            fs::path rel = fs::relative(absNorm, assetRoot, ec);
            const std::string stableId = (!ec && !rel.empty())
                ? rel.generic_string()
                : absNorm.generic_string();

            return SanitizeFilename(modelAbsPath.stem().string()) + "_" + Hex64(HashStringStable(stableId));
        }

        std::string MakeImportedTextureFilename(
            const std::filesystem::path& modelAbsPath,
            std::string_view slotName,
            std::string_view sourceTag,
            std::string_view extension)
        {
            const std::string ext = NormalizeExt(std::string(extension));
            return SanitizeFilename(
                modelAbsPath.stem().string() + "_" +
                std::string(slotName) + "_" +
                std::string(sourceTag) + "." +
                ext);
        }

        std::filesystem::path MakeImportedDirForModel(const std::filesystem::path& modelAbsPath)
        {
            const std::filesystem::path assetRoot = corefs::FindAssetRoot();
            return assetRoot / "imported" / MakeModelImportKey(modelAbsPath);
        }

        struct ImportedTextureWriteTracker
        {
            std::unordered_set<std::string> writtenPaths;
            std::unordered_map<std::string, std::size_t> writeAttempts;
        };

        std::string MakeWriteDiagnostic(const std::filesystem::path& path, std::size_t attempt)
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const bool exists = fs::exists(path, ec) && !ec;
            std::string diag = " | attempt=" + std::to_string(attempt);
            diag += " | exists=" + std::string(exists ? "true" : "false");
            if (exists)
            {
                ec.clear();
                const auto fileSize = fs::file_size(path, ec);
                diag += " | file_size=";
                diag += ec ? std::string("<error:") + ec.message() + ">" : std::to_string(fileSize);
            }
            return diag;
        }

        void CopyFileIfDifferent(const std::filesystem::path& src, const std::filesystem::path& dst, ImportedTextureWriteTracker* tracker = nullptr)
        {
            namespace fs = std::filesystem;
            const std::string key = NormalizeLexically(dst).generic_string();
            std::size_t attempt = 1;
            if (tracker)
            {
                attempt = ++tracker->writeAttempts[key];
                if (tracker->writtenPaths.contains(key))
                {
                    return;
                }
            }

            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);

            ec.clear();
            const bool dstExists = fs::exists(dst, ec) && !ec;
            if (dstExists)
            {
                std::error_code srcEc;
                const auto srcSize = fs::file_size(src, srcEc);
                std::error_code dstEc;
                const auto dstSize = fs::file_size(dst, dstEc);
                if (!srcEc && !dstEc && srcSize == dstSize)
                {
                    if (tracker)
                    {
                        tracker->writtenPaths.insert(key);
                    }
                    return;
                }
            }

            ec.clear();
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                throw std::runtime_error(
                    "Failed to copy imported texture from " + src.string() + " to " + dst.string() + ": " + ec.message() +
                    MakeWriteDiagnostic(dst, attempt));
            }
            if (tracker)
            {
                tracker->writtenPaths.insert(key);
            }
        }

        void WriteBytesToFile(const std::filesystem::path& path, const void* data, std::size_t size, ImportedTextureWriteTracker* tracker = nullptr)
        {
            const std::string key = NormalizeLexically(path).generic_string();
            std::size_t attempt = 1;
            if (tracker)
            {
                attempt = ++tracker->writeAttempts[key];
                if (tracker->writtenPaths.contains(key))
                {
                    return;
                }
            }

            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);

            ec.clear();
            const bool exists = std::filesystem::exists(path, ec) && !ec;
            if (exists)
            {
                ec.clear();
                const auto fileSize = std::filesystem::file_size(path, ec);
                if (!ec && fileSize == size)
                {
                    if (tracker)
                    {
                        tracker->writtenPaths.insert(key);
                    }
                    return;
                }
            }

            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (!f)
            {
                throw std::runtime_error("Failed to open imported texture for write: " + path.string() + MakeWriteDiagnostic(path, attempt));
            }
            f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            if (!f)
            {
                throw std::runtime_error("Failed to write imported texture: " + path.string() + MakeWriteDiagnostic(path, attempt));
            }
            if (tracker)
            {
                tracker->writtenPaths.insert(key);
            }
        }

        void WriteUncompressedAiTextureAsTga(const std::filesystem::path& path, const aiTexture* tex, ImportedTextureWriteTracker* tracker = nullptr)
        {
            if (!tex || tex->mHeight == 0)
            {
                throw std::runtime_error("Invalid raw aiTexture for TGA export");
            }

            const std::uint16_t width = static_cast<std::uint16_t>(tex->mWidth);
            const std::uint16_t height = static_cast<std::uint16_t>(tex->mHeight);
            std::vector<unsigned char> bytes;
            bytes.resize(18u + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0);
            bytes[2] = 2;
            bytes[12] = static_cast<unsigned char>(width & 0xFF);
            bytes[13] = static_cast<unsigned char>((width >> 8) & 0xFF);
            bytes[14] = static_cast<unsigned char>(height & 0xFF);
            bytes[15] = static_cast<unsigned char>((height >> 8) & 0xFF);
            bytes[16] = 32;
            bytes[17] = 0x20;

            unsigned char* dst = bytes.data() + 18;
            for (std::size_t i = 0; i < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++i)
            {
                const aiTexel& s = tex->pcData[i];
                dst[i * 4 + 0] = s.b;
                dst[i * 4 + 1] = s.g;
                dst[i * 4 + 2] = s.r;
                dst[i * 4 + 3] = s.a;
            }
            WriteBytesToFile(path, bytes.data(), bytes.size(), tracker);
        }

        std::string NormalizeExt(std::string ext)
        {
            if (!ext.empty() && ext[0] == '.')
            {
                ext.erase(ext.begin());
            }
            for (char& c : ext)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (ext.empty())
            {
                ext = "bin";
            }
            return ext;
        }

        std::optional<std::string> CopyExternalTextureToImportedFolder(
            const std::filesystem::path& modelAbsPath,
            std::string_view rawTexturePath,
            std::string_view slotName,
            ImportedTextureWriteTracker* tracker = nullptr)
        {
            const auto resolved = TryResolveTexturePath(modelAbsPath, rawTexturePath);
            if (!resolved)
            {
                return std::nullopt;
            }

            const std::filesystem::path dstDir = MakeImportedDirForModel(modelAbsPath);

            const std::string sourceTag =
                Hex64(HashStringStable(resolved->generic_string()));

            const std::string filename = MakeImportedTextureFilename(
                modelAbsPath,
                slotName,
                sourceTag,
                resolved->extension().string());

            const std::filesystem::path dst = dstDir / filename;
            CopyFileIfDifferent(*resolved, dst, tracker);
            return MakeAssetRelativePath(dst);
        }

        std::string ExportEmbeddedTextureToImportedFolder(
            const aiScene* scene,
            const std::filesystem::path& modelAbsPath,
            std::uint32_t embeddedIndex,
            std::string_view slotName,
            ImportedTextureWriteTracker* tracker = nullptr)
        {
            if (!scene || embeddedIndex >= scene->mNumTextures)
            {
                throw std::runtime_error("Embedded texture index out of range");
            }

            const aiTexture* tex = scene->mTextures[embeddedIndex];
            if (!tex)
            {
                throw std::runtime_error("Embedded texture is null");
            }

            const std::filesystem::path outDir = MakeImportedDirForModel(modelAbsPath);
            std::filesystem::path outPath;

            if (tex->mHeight == 0)
            {
                std::string ext = NormalizeExt(tex->achFormatHint);
                if (ext == "bin")
                {
                    ext = "png";
                }

                const std::string filename = MakeImportedTextureFilename(
                    modelAbsPath,
                    slotName,
                    "embedded_" + std::to_string(embeddedIndex),
                    ext);

                outPath = outDir / filename;
                WriteBytesToFile(outPath, tex->pcData, static_cast<std::size_t>(tex->mWidth), tracker);
            }
            else
            {
                const std::string filename = MakeImportedTextureFilename(
                    modelAbsPath,
                    slotName,
                    "embedded_" + std::to_string(embeddedIndex),
                    "tga");

                outPath = outDir / filename;
                WriteUncompressedAiTextureAsTga(outPath, tex, tracker);
            }

            return MakeAssetRelativePath(outPath);
        }

        std::optional<ImportedMaterialTextureRef> ReadAndNormalizeTextureRef(
            const aiScene* scene,
            const std::filesystem::path& modelAbsPath,
            aiMaterial* mat,
            aiTextureType type,
            std::string_view slotName,
            bool materializeTextures,
            ImportedTextureWriteTracker* tracker = nullptr)
        {
            if (!mat)
            {
                return std::nullopt;
            }

            aiString tex;
            if (mat->GetTexture(type, 0, &tex) != AI_SUCCESS || tex.length == 0)
            {
                return std::nullopt;
            }

            const std::string raw = tex.C_Str();

            // Explicit embedded reference: "*0", "*1", ...
            if (!raw.empty() && raw[0] == '*')
            {
                try
                {
                    const std::uint32_t idx =
                        static_cast<std::uint32_t>(std::stoul(raw.substr(1)));

                    ImportedMaterialTextureRef out{};
                    out.embedded = true;

                    if (materializeTextures)
                    {
                        out.path = ExportEmbeddedTextureToImportedFolder(scene, modelAbsPath, idx, slotName, tracker);
                    }
                    else
                    {
                        out.path = raw;
                    }

                    return out;
                }
                catch (...)
                {
                    return std::nullopt;
                }
            }

            // External texture path
            if (materializeTextures)
            {
                if (auto copied = CopyExternalTextureToImportedFolder(modelAbsPath, raw, slotName, tracker))
                {
                    ImportedMaterialTextureRef out{};
                    out.path = *copied;
                    out.embedded = false;
                    return out;
                }
            }
            else
            {
                if (auto resolved = ResolveExternalTextureRefNoCopy(modelAbsPath, raw))
                {
                    return resolved;
                }
            }

            // Fallback: maybe importer exposed a weird external-looking string,
            // but the texture is actually embedded in the file.
            if (auto embeddedIdx = TryFindEmbeddedTextureIndex(scene, raw))
            {
                ImportedMaterialTextureRef out{};
                out.embedded = true;

                if (materializeTextures)
                {
                    out.path = ExportEmbeddedTextureToImportedFolder(
                        scene,
                        modelAbsPath,
                        *embeddedIdx,
                        slotName);
                }
                else
                {
                    out.path = "*" + std::to_string(*embeddedIdx);
                }

                return out;
            }

            return std::nullopt;
        }
        
    inline ImportedModelScene LoadAssimpScene(
        std::filesystem::path pathIn,
        bool flipUVs = true,
        bool importSkeletonNodes = false,
        bool materializeTextures = false)
    {
        namespace fs = std::filesystem;
        fs::path path = pathIn;
        if (!path.is_absolute())
        {
            path = corefs::ResolveAsset(path);
        }
        path = NormalizeLexically(path);

        unsigned flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals;
        if (flipUVs)
        {
            flags |= aiProcess_FlipUVs;
        }

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path.string(), flags);
        if (!scene || !scene->mRootNode)
        {
            const char* err = importer.GetErrorString();
            std::string msg = "Assimp failed to load scene: " + path.string();
            if (err && *err)
            {
                msg += " | ";
                msg += err;
            }
            throw std::runtime_error(msg);
        }

        ImportedModelScene out{};
        ImportedTextureWriteTracker textureWriteTracker{};
        out.submeshes.reserve(scene->mNumMeshes);
        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            const aiMesh* mesh = scene->mMeshes[mi];
            ImportedSubmeshInfo sm{};
            sm.submeshIndex = mi;
            sm.materialIndex = mesh ? mesh->mMaterialIndex : 0u;
            sm.name = (mesh && mesh->mName.length > 0) ? std::string(mesh->mName.C_Str()) : ("Submesh_" + std::to_string(mi));
            out.submeshes.push_back(std::move(sm));
        }

        out.materials.reserve(scene->mNumMaterials);
        for (unsigned mi = 0; mi < scene->mNumMaterials; ++mi)
        {
            aiMaterial* mat = scene->mMaterials[mi];
            ImportedMaterialInfo info{};
            aiString name;
            info.name = (mat && mat->Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0)
                ? std::string(name.C_Str())
                : ("Material_" + std::to_string(mi));

            info.baseColor = ReadAndNormalizeTextureRef(scene, path, mat, aiTextureType_BASE_COLOR, "albedo", materializeTextures, &textureWriteTracker);
            if (!info.baseColor) info.baseColor = ReadAndNormalizeTextureRef(scene, path, mat, aiTextureType_DIFFUSE, "albedo", materializeTextures, &textureWriteTracker);
            info.normal = ReadAndNormalizeTextureRef(scene, path, mat, aiTextureType_NORMAL_CAMERA, "normal", materializeTextures, &textureWriteTracker);
            if (!info.normal) info.normal = ReadAndNormalizeTextureRef(scene, path, mat, aiTextureType_NORMALS, "normal", materializeTextures, &textureWriteTracker);
            if (!info.normal) info.normal = ReadAndNormalizeTextureRef(scene, path, mat, aiTextureType_HEIGHT, "normal", materializeTextures, &textureWriteTracker);
            info.metallic = ReadAndNormalizeTextureRef(scene, path, mat, aiTextureType_METALNESS, "metallic", materializeTextures, &textureWriteTracker);
            info.roughness = ReadAndNormalizeTextureRef(scene, path, mat, aiTextureType_DIFFUSE_ROUGHNESS, "roughness", materializeTextures, &textureWriteTracker);
            info.ao = ReadAndNormalizeTextureRef(scene, path, mat, aiTextureType_AMBIENT_OCCLUSION, "ao", materializeTextures, &textureWriteTracker);
            if (!info.ao) info.ao = ReadAndNormalizeTextureRef(scene, path, mat, aiTextureType_LIGHTMAP, "ao", materializeTextures, &textureWriteTracker);
            info.emissive = ReadAndNormalizeTextureRef(scene, path, mat, aiTextureType_EMISSIVE, "emissive", materializeTextures, &textureWriteTracker);
            if (!info.emissive) info.emissive = ReadAndNormalizeTextureRef(scene, path, mat, aiTextureType_EMISSION_COLOR, "emissive", materializeTextures, &textureWriteTracker);
            out.materials.push_back(std::move(info));
        }

        const auto convertTransform = [](const mathUtils::Mat4& m)
            {
                Transform t{};
                t.useMatrix = true;
                t.matrix = m;
                return t;
            };

        std::vector<int> stackParents;
        std::vector<const aiNode*> stackNodes;
        stackNodes.push_back(scene->mRootNode);
        stackParents.push_back(-1);

        while (!stackNodes.empty())
        {
            const aiNode* node = stackNodes.back();
            const int parent = stackParents.back();
            stackNodes.pop_back();
            stackParents.pop_back();

            const bool hasMeshes = node->mNumMeshes > 0;
            const bool shouldKeepNode = importSkeletonNodes || hasMeshes || parent < 0;

            int thisIndex = parent;
            if (shouldKeepNode)
            {
                ImportedSceneNode dst{};
                dst.name = (node->mName.length > 0) ? std::string(node->mName.C_Str()) : std::string("Node_") + std::to_string(out.nodes.size());
                dst.parent = parent;
                dst.localTransform = convertTransform(AiToMat4(node->mTransformation));
                dst.submeshes.reserve(node->mNumMeshes);
                for (unsigned i = 0; i < node->mNumMeshes; ++i)
                {
                    dst.submeshes.push_back(node->mMeshes[i]);
                }
                thisIndex = static_cast<int>(out.nodes.size());
                out.nodes.push_back(std::move(dst));
            }

            for (int ci = static_cast<int>(node->mNumChildren) - 1; ci >= 0; --ci)
            {
                stackNodes.push_back(node->mChildren[ci]);
                stackParents.push_back(thisIndex);
            }
        }

        return out;
    }
}