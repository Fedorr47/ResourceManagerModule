module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module core:assimp_loader;

import :mesh;
import :skinned_mesh;
import :skeleton;
import :math_utils;
import :file_system;

namespace
{
    [[nodiscard]] mathUtils::Mat4 AiToMat4(const aiMatrix4x4& m)
    {
        mathUtils::Mat4 out{ 1.0f };
        out(0, 0) = m.a1; out(0, 1) = m.a2; out(0, 2) = m.a3; out(0, 3) = m.a4;
        out(1, 0) = m.b1; out(1, 1) = m.b2; out(1, 2) = m.b3; out(1, 3) = m.b4;
        out(2, 0) = m.c1; out(2, 1) = m.c2; out(2, 2) = m.c3; out(2, 3) = m.c4;
        out(3, 0) = m.d1; out(3, 1) = m.d2; out(3, 2) = m.d3; out(3, 3) = m.d4;
        return out;
    }

    struct RawVertexInfluence
    {
        std::string boneName;
        float weight{ 0.0f };
    };

    [[nodiscard]] const aiNode* FindNodeByNameRecursive(const aiNode* node, std::string_view wantedName)
    {
        if (!node)
        {
            return nullptr;
        }
        if (wantedName == node->mName.C_Str())
        {
            return node;
        }
        for (unsigned childIndex = 0; childIndex < node->mNumChildren; ++childIndex)
        {
            if (const aiNode* found = FindNodeByNameRecursive(node->mChildren[childIndex], wantedName))
            {
                return found;
            }
        }
        return nullptr;
    }

    void CollectRequiredBoneNames(
        const aiScene* scene,
        const std::unordered_set<std::string>& weightedBoneNames,
        std::unordered_set<std::string>& outRequiredNames)
    {
        outRequiredNames.clear();
        for (const std::string& boneName : weightedBoneNames)
        {
            outRequiredNames.insert(boneName);
            const aiNode* node = scene ? FindNodeByNameRecursive(scene->mRootNode, boneName) : nullptr;
            while (node)
            {
                outRequiredNames.insert(node->mName.C_Str());
                node = node->mParent;
            }
        }
    }

    void AppendRequiredBonesFromScene(
        const aiNode* node,
        const std::unordered_set<std::string>& requiredNames,
        const std::unordered_map<std::string, mathUtils::Mat4>& inverseBindByName,
        rendern::Skeleton& skeleton,
        int parentIndex)
    {
        if (!node)
        {
            return;
        }

        const std::string nodeName = node->mName.C_Str();
        int thisIndex = parentIndex;
        if (requiredNames.contains(nodeName))
        {
            rendern::SkeletonBone bone{};
            bone.name = nodeName;
            bone.parentIndex = parentIndex;
            bone.bindLocalTransform = AiToMat4(node->mTransformation);
            if (const auto it = inverseBindByName.find(nodeName); it != inverseBindByName.end())
            {
                bone.inverseBindMatrix = it->second;
            }
            else
            {
                bone.inverseBindMatrix = mathUtils::Mat4(1.0f);
            }

            thisIndex = static_cast<int>(skeleton.bones.size());
            skeleton.bones.push_back(std::move(bone));
            if (parentIndex < 0 && skeleton.bones.size() == 1)
            {
                skeleton.rootBoneIndex = 0u;
            }
        }

        for (unsigned childIndex = 0; childIndex < node->mNumChildren; ++childIndex)
        {
            AppendRequiredBonesFromScene(
                node->mChildren[childIndex],
                requiredNames,
                inverseBindByName,
                skeleton,
                thisIndex);
        }
    }

    void AppendMissingWeightedBones(
        const std::unordered_set<std::string>& weightedBoneNames,
        const std::unordered_map<std::string, mathUtils::Mat4>& inverseBindByName,
        rendern::Skeleton& skeleton)
    {
        rendern::RebuildBoneNameLookup(skeleton);
        for (const std::string& boneName : weightedBoneNames)
        {
            if (rendern::FindBoneIndex(skeleton, boneName).has_value())
            {
                continue;
            }

            rendern::SkeletonBone bone{};
            bone.name = boneName;
            bone.parentIndex = -1;
            bone.bindLocalTransform = mathUtils::Mat4(1.0f);
            if (const auto it = inverseBindByName.find(boneName); it != inverseBindByName.end())
            {
                bone.inverseBindMatrix = it->second;
            }

            if (skeleton.bones.empty())
            {
                skeleton.rootBoneIndex = 0u;
            }
            skeleton.bones.push_back(std::move(bone));
        }
        rendern::RebuildBoneNameLookup(skeleton);
    }

    void FinalizeVertexInfluences(
        rendern::SkinnedMeshCPU& out,
        const std::vector<std::vector<RawVertexInfluence>>& pendingInfluences)
    {
        const std::uint16_t rigidBoneIndex = out.skeleton.bones.empty()
            ? 0u
            : static_cast<std::uint16_t>(out.skeleton.rootBoneIndex);

        for (std::size_t vertexIndex = 0; vertexIndex < out.vertices.size(); ++vertexIndex)
        {
            auto& dstVertex = out.vertices[vertexIndex];
            auto influences = pendingInfluences[vertexIndex];
            std::sort(
                influences.begin(),
                influences.end(),
                [](const RawVertexInfluence& a, const RawVertexInfluence& b)
                {
                    return a.weight > b.weight;
                });

            if (!influences.empty())
            {
                auto tryAssign = [&](std::size_t slot, const RawVertexInfluence& influence)
                    {
                        const auto boneIndex = rendern::FindBoneIndex(out.skeleton, influence.boneName);
                        if (!boneIndex)
                        {
                            return;
                        }

                        switch (slot)
                        {
                        case 0:
                            dstVertex.boneIndex0 = static_cast<std::uint16_t>(*boneIndex);
                            dstVertex.boneWeight0 = influence.weight;
                            break;
                        case 1:
                            dstVertex.boneIndex1 = static_cast<std::uint16_t>(*boneIndex);
                            dstVertex.boneWeight1 = influence.weight;
                            break;
                        case 2:
                            dstVertex.boneIndex2 = static_cast<std::uint16_t>(*boneIndex);
                            dstVertex.boneWeight2 = influence.weight;
                            break;
                        case 3:
                            dstVertex.boneIndex3 = static_cast<std::uint16_t>(*boneIndex);
                            dstVertex.boneWeight3 = influence.weight;
                            break;
                        default:
                            break;
                        }
                    };

                const std::size_t count = std::min<std::size_t>(influences.size(), rendern::kMaxSkinWeightsPerVertex);
                for (std::size_t i = 0; i < count; ++i)
                {
                    tryAssign(i, influences[i]);
                }
            }
            else
            {
                dstVertex.boneIndex0 = rigidBoneIndex;
                dstVertex.boneIndex1 = rigidBoneIndex;
                dstVertex.boneIndex2 = rigidBoneIndex;
                dstVertex.boneIndex3 = rigidBoneIndex;
                dstVertex.boneWeight0 = 1.0f;
                dstVertex.boneWeight1 = 0.0f;
                dstVertex.boneWeight2 = 0.0f;
                dstVertex.boneWeight3 = 0.0f;
            }

            rendern::NormalizeBoneWeights(dstVertex);
        }
    }
}

export namespace rendern
{
    inline MeshCPU LoadAssimp(
        const std::filesystem::path& pathIn,
        bool flipUVs = true,
        std::optional<std::uint32_t> submeshIndex = std::nullopt,
        bool bakeNodeTransforms = true)
    {
        std::filesystem::path path = pathIn;
        if (!path.is_absolute())
        {
            path = corefs::ResolveAsset(path);
        }

        unsigned flags =
            aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace |
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
                    v.tx = 1.0f; v.ty = 0.0f; v.tz = 0.0f; v.tw = 1.0f;
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

        ComputeTangents(out);
        return out;
    }

    inline SkinnedMeshCPU LoadAssimpSkinned(
        const std::filesystem::path& pathIn,
        bool flipUVs = true,
        std::optional<std::uint32_t> submeshIndex = std::nullopt)
    {
        std::filesystem::path path = pathIn;
        if (!path.is_absolute())
        {
            path = corefs::ResolveAsset(path);
        }

        unsigned flags =
            aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace |
            aiProcess_ImproveCacheLocality;

        if (flipUVs)
        {
            flags |= aiProcess_FlipUVs;
        }

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path.string(), flags);
        if (!scene || !scene->HasMeshes())
        {
            const char* err = importer.GetErrorString();
            std::string msg = "Assimp failed to load skinned mesh: " + path.string();
            if (err && *err)
            {
                msg += " | ";
                msg += err;
            }
            throw std::runtime_error(msg);
        }

        std::vector<unsigned> selectedMeshIndices;
        if (submeshIndex)
        {
            const std::size_t idx = static_cast<std::size_t>(*submeshIndex);
            if (idx >= scene->mNumMeshes)
            {
                throw std::runtime_error("Assimp skinned submesh index out of range: " + std::to_string(*submeshIndex) + " in " + path.string());
            }
            selectedMeshIndices.push_back(static_cast<unsigned>(*submeshIndex));
        }
        else
        {
            selectedMeshIndices.reserve(scene->mNumMeshes);
            for (unsigned meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
            {
                selectedMeshIndices.push_back(meshIndex);
            }
        }

        SkinnedMeshCPU out{};
        std::vector<std::vector<RawVertexInfluence>> pendingInfluences;
        std::unordered_set<std::string> weightedBoneNames;
        std::unordered_map<std::string, mathUtils::Mat4> inverseBindByName;

        for (unsigned meshIndex : selectedMeshIndices)
        {
            const aiMesh* m = scene->mMeshes[meshIndex];
            if (!m || m->mNumVertices == 0)
            {
                continue;
            }
            if (!m->HasBones() || m->mNumBones == 0)
            {
                throw std::runtime_error("Assimp skinned import requires meshes with bones: " + path.string());
            }

            const std::uint32_t baseVertex = static_cast<std::uint32_t>(out.vertices.size());
            const std::uint32_t firstIndex = static_cast<std::uint32_t>(out.indices.size());
            out.vertices.reserve(out.vertices.size() + m->mNumVertices);
            pendingInfluences.resize(out.vertices.size() + m->mNumVertices);

            const bool hasNormals = m->HasNormals();
            const bool hasUV0 = m->HasTextureCoords(0);
            const bool hasTangents = m->HasTangentsAndBitangents();

            for (unsigned vertexIndex = 0; vertexIndex < m->mNumVertices; ++vertexIndex)
            {
                const aiVector3D p = m->mVertices[vertexIndex];
                const aiVector3D n = hasNormals ? m->mNormals[vertexIndex] : aiVector3D(0.0f, 0.0f, 1.0f);
                const aiVector3D uv = hasUV0 ? m->mTextureCoords[0][vertexIndex] : aiVector3D(0.0f, 0.0f, 0.0f);
                const aiVector3D t = hasTangents ? m->mTangents[vertexIndex] : aiVector3D(1.0f, 0.0f, 0.0f);

                SkinnedVertexDesc v{};
                v.px = p.x; v.py = p.y; v.pz = p.z;
                v.nx = n.x; v.ny = n.y; v.nz = n.z;
                v.u = uv.x; v.v = uv.y;
                v.tx = t.x; v.ty = t.y; v.tz = t.z; v.tw = 1.0f;
                out.vertices.push_back(v);
            }

            for (unsigned faceIndex = 0; faceIndex < m->mNumFaces; ++faceIndex)
            {
                const aiFace& face = m->mFaces[faceIndex];
                if (face.mNumIndices != 3)
                {
                    continue;
                }

                out.indices.push_back(baseVertex + static_cast<std::uint32_t>(face.mIndices[0]));
                out.indices.push_back(baseVertex + static_cast<std::uint32_t>(face.mIndices[1]));
                out.indices.push_back(baseVertex + static_cast<std::uint32_t>(face.mIndices[2]));
            }

            SkinnedSubmesh submesh{};
            submesh.name = (m->mName.length > 0) ? std::string(m->mName.C_Str()) : ("SkinnedSubmesh_" + std::to_string(meshIndex));
            submesh.firstIndex = firstIndex;
            submesh.indexCount = static_cast<std::uint32_t>(out.indices.size()) - firstIndex;
            submesh.materialIndex = m->mMaterialIndex;
            out.submeshes.push_back(std::move(submesh));

            for (unsigned boneArrayIndex = 0; boneArrayIndex < m->mNumBones; ++boneArrayIndex)
            {
                const aiBone* bone = m->mBones[boneArrayIndex];
                if (!bone)
                {
                    continue;
                }

                const std::string boneName = bone->mName.C_Str();
                weightedBoneNames.insert(boneName);
                inverseBindByName.try_emplace(boneName, AiToMat4(bone->mOffsetMatrix));

                for (unsigned weightIndex = 0; weightIndex < bone->mNumWeights; ++weightIndex)
                {
                    const aiVertexWeight vw = bone->mWeights[weightIndex];
                    const std::size_t dstVertexIndex = static_cast<std::size_t>(baseVertex) + static_cast<std::size_t>(vw.mVertexId);
                    if (dstVertexIndex >= pendingInfluences.size())
                    {
                        continue;
                    }
                    pendingInfluences[dstVertexIndex].push_back(RawVertexInfluence{
                        .boneName = boneName,
                        .weight = vw.mWeight
                        });
                }
            }
        }

        if (out.vertices.empty() || out.indices.empty())
        {
            throw std::runtime_error("Assimp skinned mesh is empty after import: " + path.string());
        }
        if (weightedBoneNames.empty())
        {
            throw std::runtime_error("Assimp skinned mesh has no weighted bones: " + path.string());
        }

        std::unordered_set<std::string> requiredBoneNames;
        CollectRequiredBoneNames(scene, weightedBoneNames, requiredBoneNames);
        AppendRequiredBonesFromScene(scene->mRootNode, requiredBoneNames, inverseBindByName, out.skeleton, -1);
        AppendMissingWeightedBones(weightedBoneNames, inverseBindByName, out.skeleton);

        if (!IsValidSkeleton(out.skeleton))
        {
            throw std::runtime_error("Assimp skinned mesh produced invalid skeleton: " + path.string());
        }

        FinalizeVertexInfluences(out, pendingInfluences);
        ComputeTangents(out);
        RefreshBindPoseBounds(out);
        out.bounds.maxAnimatedBounds = out.bounds.bindPoseBounds;
        return out;
    }
}