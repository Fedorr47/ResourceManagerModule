module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

export module core:assimp_loader;

import :mesh;
import :file_system;

export namespace rendern
{
    inline MeshCPU LoadAssimp(
        const std::filesystem::path& pathIn,
        bool flipUVs = true,
        std::optional<std::uint32_t> submeshIndex = std::nullopt,
        bool bakeNodeTransforms = true)
    {
        namespace fs = std::filesystem;

        fs::path path = pathIn;
        if (!path.is_absolute())
        {
            path = corefs::ResolveAsset(path);
        }

        unsigned flags =
            aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_GenSmoothNormals |
            aiProcess_ImproveCacheLocality;

        if (bakeNodeTransforms)
        {
            flags |= aiProcess_PreTransformVertices;
        }

        if (flipUVs)
        {
            flags |= aiProcess_FlipUVs;
        }

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path.string(), flags);
        if (!scene || !scene->HasMeshes())
        {
            const char* err = importer.GetErrorString();
            std::string msg = "Assimp failed to load mesh: " + path.string();
            if (err && *err)
            {
                msg += " | ";
                msg += err;
            }
            throw std::runtime_error(msg);
        }

        MeshCPU out{};

        const auto appendMesh = [&](const aiMesh* m)
            {
                if (!m || m->mNumVertices == 0)
                {
                    return;
                }

                const std::uint32_t baseVertex = static_cast<std::uint32_t>(out.vertices.size());
                out.vertices.reserve(out.vertices.size() + m->mNumVertices);

                const bool hasNormals = m->HasNormals();
                const bool hasUV0 = m->HasTextureCoords(0);

                for (unsigned vi = 0; vi < m->mNumVertices; ++vi)
                {
                    const aiVector3D p = m->mVertices[vi];
                    const aiVector3D n = hasNormals ? m->mNormals[vi] : aiVector3D(0.0f, 0.0f, 1.0f);
                    const aiVector3D uv = hasUV0 ? m->mTextureCoords[0][vi] : aiVector3D(0.0f, 0.0f, 0.0f);

                    VertexDesc v{};
                    v.px = p.x; v.py = p.y; v.pz = p.z;
                    v.nx = n.x; v.ny = n.y; v.nz = n.z;
                    v.u = uv.x; v.v = uv.y;
                    out.vertices.push_back(v);
                }

                for (unsigned fi = 0; fi < m->mNumFaces; ++fi)
                {
                    const aiFace& f = m->mFaces[fi];
                    if (f.mNumIndices != 3)
                    {
                        continue;
                    }

                    out.indices.push_back(baseVertex + static_cast<std::uint32_t>(f.mIndices[0]));
                    out.indices.push_back(baseVertex + static_cast<std::uint32_t>(f.mIndices[1]));
                    out.indices.push_back(baseVertex + static_cast<std::uint32_t>(f.mIndices[2]));
                }
            };

        if (submeshIndex)
        {
            const std::size_t idx = static_cast<std::size_t>(*submeshIndex);
            if (idx >= scene->mNumMeshes)
            {
                throw std::runtime_error("Assimp submesh index out of range: " + std::to_string(*submeshIndex) + " in " + path.string());
            }
            appendMesh(scene->mMeshes[idx]);
        }
        else
        {
            for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
            {
                appendMesh(scene->mMeshes[mi]);
            }
        }

        if (out.vertices.empty() || out.indices.empty())
        {
            throw std::runtime_error("Assimp mesh is empty after import: " + path.string());
        }

        return out;
    }
}