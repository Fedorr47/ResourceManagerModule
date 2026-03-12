module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <optional>
#include <limits>
#include <stdexcept>
#include <string>
#include <sstream>
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
import :animation_clip;

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

    [[nodiscard]] mathUtils::Vec3 AiToVec3(const aiVector3D& v) noexcept
    {
        return mathUtils::Vec3(v.x, v.y, v.z);
    }

    [[nodiscard]] mathUtils::Vec4 AiToQuatVec4(const aiQuaternion& q) noexcept
    {
        return mathUtils::Vec4(q.x, q.y, q.z, q.w);
    }

    struct RawVertexInfluence
    {
        std::string boneName;
        float weight{ 0.0f };
    };

    struct BindTRS
    {
        mathUtils::Vec3 translation{ 0.0f, 0.0f, 0.0f };
        mathUtils::Vec4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
        mathUtils::Vec3 scale{ 1.0f, 1.0f, 1.0f };
    };

    struct BoundsAccumulator
    {
        bool initialized{ false };
        mathUtils::Vec3 min{ 0.0f, 0.0f, 0.0f };
        mathUtils::Vec3 max{ 0.0f, 0.0f, 0.0f };
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

    [[nodiscard]] mathUtils::Mat4 ComputeNodeGlobalTransform(const aiNode* node)
    {
        mathUtils::Mat4 global(1.0f);
        std::vector<const aiNode*> chain;
        while (node)
        {
            chain.push_back(node);
            node = node->mParent;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            global = global * AiToMat4((*it)->mTransformation);
        }
        return global;
    }

    void BuildMeshOwnerGlobalTransformsRecursive(
        const aiNode* node,
        const mathUtils::Mat4& parentGlobal,
        std::unordered_map<unsigned, mathUtils::Mat4>& outMeshOwnerGlobals)
    {
        if (!node)
        {
            return;
        }

        const mathUtils::Mat4 nodeGlobal = parentGlobal * AiToMat4(node->mTransformation);
        for (unsigned meshSlot = 0; meshSlot < node->mNumMeshes; ++meshSlot)
        {
            outMeshOwnerGlobals.try_emplace(node->mMeshes[meshSlot], nodeGlobal);
        }

        for (unsigned childIndex = 0; childIndex < node->mNumChildren; ++childIndex)
        {
            BuildMeshOwnerGlobalTransformsRecursive(node->mChildren[childIndex], nodeGlobal, outMeshOwnerGlobals);
        }
    }

    [[nodiscard]] bool MatricesNearlyEqual(const mathUtils::Mat4& a, const mathUtils::Mat4& b, float eps = 1e-4f) noexcept
    {
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                if (std::abs(a(row, col) - b(row, col)) > eps)
                {
                    return false;
                }
            }
        }
        return true;
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

    [[nodiscard]] std::vector<BindTRS> BuildBindTRS(const rendern::Skeleton& skeleton)
    {
        std::vector<BindTRS> bind;
        bind.resize(skeleton.bones.size());
        for (std::size_t boneIndex = 0; boneIndex < skeleton.bones.size(); ++boneIndex)
        {
            auto& trs = bind[boneIndex];
            rendern::DecomposeTRS(
                skeleton.bones[boneIndex].bindLocalTransform,
                trs.translation,
                trs.rotation,
                trs.scale);
        }
        return bind;
    }

    [[nodiscard]] float SanitizeTicksPerSecond(double ticksPerSecond) noexcept
    {
        return (ticksPerSecond > 0.0) ? static_cast<float>(ticksPerSecond) : 25.0f;
    }

    [[nodiscard]] rendern::AnimationClip BuildAnimationClip(
        const aiAnimation& anim,
        const rendern::Skeleton& skeleton,
        std::uint32_t clipIndex)
    {
        rendern::AnimationClip clip{};
        clip.name = (anim.mName.length > 0)
            ? std::string(anim.mName.C_Str())
            : ("Anim_" + std::to_string(clipIndex));
        clip.durationTicks = std::max(0.0f, static_cast<float>(anim.mDuration));
        clip.ticksPerSecond = SanitizeTicksPerSecond(anim.mTicksPerSecond);
        clip.looping = true;
        clip.channels.reserve(anim.mNumChannels);

        for (unsigned channelIndex = 0; channelIndex < anim.mNumChannels; ++channelIndex)
        {
            const aiNodeAnim* src = anim.mChannels[channelIndex];
            if (!src)
            {
                continue;
            }

            rendern::BoneAnimationChannel dst{};
            dst.boneName = src->mNodeName.C_Str();
            if (const auto boneIndex = rendern::FindBoneIndex(skeleton, dst.boneName))
            {
                dst.boneIndex = static_cast<int>(*boneIndex);
            }

            dst.translationKeys.reserve(src->mNumPositionKeys);
            for (unsigned keyIndex = 0; keyIndex < src->mNumPositionKeys; ++keyIndex)
            {
                const aiVectorKey& key = src->mPositionKeys[keyIndex];
                dst.translationKeys.push_back(rendern::TranslationKey{
                    .timeTicks = static_cast<float>(key.mTime),
                    .value = AiToVec3(key.mValue)
                    });
            }

            dst.rotationKeys.reserve(src->mNumRotationKeys);
            for (unsigned keyIndex = 0; keyIndex < src->mNumRotationKeys; ++keyIndex)
            {
                const aiQuatKey& key = src->mRotationKeys[keyIndex];
                dst.rotationKeys.push_back(rendern::RotationKey{
                    .timeTicks = static_cast<float>(key.mTime),
                    .value = rendern::NormalizeQuat(AiToQuatVec4(key.mValue))
                    });
            }

            dst.scaleKeys.reserve(src->mNumScalingKeys);
            for (unsigned keyIndex = 0; keyIndex < src->mNumScalingKeys; ++keyIndex)
            {
                const aiVectorKey& key = src->mScalingKeys[keyIndex];
                dst.scaleKeys.push_back(rendern::ScaleKey{
                    .timeTicks = static_cast<float>(key.mTime),
                    .value = AiToVec3(key.mValue)
                    });
            }

            clip.channels.push_back(std::move(dst));
        }

        return clip;
    }

    [[nodiscard]] std::vector<rendern::AnimationClip> BuildAnimationClipsFromScene(
        const aiScene* scene,
        const rendern::Skeleton& skeleton)
    {
        std::vector<rendern::AnimationClip> clips;
        if (!scene || scene->mNumAnimations == 0)
        {
            return clips;
        }

        clips.reserve(scene->mNumAnimations);
        for (unsigned animationIndex = 0; animationIndex < scene->mNumAnimations; ++animationIndex)
        {
            const aiAnimation* anim = scene->mAnimations[animationIndex];
            if (!anim)
            {
                continue;
            }
            clips.push_back(BuildAnimationClip(*anim, skeleton, animationIndex));
        }
        return clips;
    }

    void BuildSkinMatricesForSample(
        const rendern::Skeleton& skeleton,
        const rendern::AnimationClip& clip,
        const std::vector<BindTRS>& bindTrs,
        float timeTicks,
        const mathUtils::Mat4& skeletonToMeshSpace,
        std::vector<mathUtils::Mat4>& outSkinMatrices)
    {
        std::vector<mathUtils::Mat4> localPose;
        std::vector<mathUtils::Mat4> globalPose;
        localPose.resize(skeleton.bones.size());
        globalPose.resize(skeleton.bones.size());
        outSkinMatrices.resize(skeleton.bones.size());

        for (std::size_t boneIndex = 0; boneIndex < skeleton.bones.size(); ++boneIndex)
        {
            localPose[boneIndex] = skeleton.bones[boneIndex].bindLocalTransform;
        }

        for (const auto& channel : clip.channels)
        {
            if (channel.boneIndex < 0 || channel.boneIndex >= static_cast<int>(skeleton.bones.size()))
            {
                continue;
            }

            const std::size_t boneIndex = static_cast<std::size_t>(channel.boneIndex);
            BindTRS sampled = bindTrs[boneIndex];
            sampled.translation = rendern::SampleTranslationKeys(channel.translationKeys, timeTicks, sampled.translation);
            sampled.rotation = rendern::SampleRotationKeys(channel.rotationKeys, timeTicks, sampled.rotation);
            sampled.scale = rendern::SampleScaleKeys(channel.scaleKeys, timeTicks, sampled.scale);
            localPose[boneIndex] = rendern::ComposeTRS(sampled.translation, sampled.rotation, sampled.scale);
        }

        for (std::size_t boneIndex = 0; boneIndex < skeleton.bones.size(); ++boneIndex)
        {
            const int parentIndex = skeleton.bones[boneIndex].parentIndex;
            if (parentIndex >= 0)
            {
                globalPose[boneIndex] = globalPose[static_cast<std::size_t>(parentIndex)] * localPose[boneIndex];
            }
            else
            {
                globalPose[boneIndex] = localPose[boneIndex];
            }
            outSkinMatrices[boneIndex] =
                skeletonToMeshSpace *
                globalPose[boneIndex] *
                skeleton.bones[boneIndex].inverseBindMatrix;
        }
    }

    [[nodiscard]] mathUtils::Vec3 SkinVertexPosition(
        const rendern::SkinnedVertexDesc& vertex,
        const std::vector<mathUtils::Mat4>& skinMatrices) noexcept
    {
        const mathUtils::Vec3 bindPos(vertex.px, vertex.py, vertex.pz);
        mathUtils::Vec3 skinned(0.0f, 0.0f, 0.0f);
        float accumWeight = 0.0f;

        auto apply = [&](std::uint16_t boneIndex, float weight)
            {
                if (weight <= 0.0f || boneIndex >= skinMatrices.size())
                {
                    return;
                }
                mathUtils::Vec3 tp = TransformPoint(skinMatrices[boneIndex], bindPos);
                skinned = skinned + tp * weight;
                accumWeight += weight;
            };

        apply(vertex.boneIndex0, vertex.boneWeight0);
        apply(vertex.boneIndex1, vertex.boneWeight1);
        apply(vertex.boneIndex2, vertex.boneWeight2);
        apply(vertex.boneIndex3, vertex.boneWeight3);

        if (accumWeight <= 1e-8f)
        {
            return bindPos;
        }
        return skinned;
    }

    void ExpandBounds(BoundsAccumulator& acc, const mathUtils::Vec3& p) noexcept
    {
        if (!acc.initialized)
        {
            acc.initialized = true;
            acc.min = p;
            acc.max = p;
            return;
        }
        acc.min.x = std::min(acc.min.x, p.x);
        acc.min.y = std::min(acc.min.y, p.y);
        acc.min.z = std::min(acc.min.z, p.z);
        acc.max.x = std::max(acc.max.x, p.x);
        acc.max.y = std::max(acc.max.y, p.y);
        acc.max.z = std::max(acc.max.z, p.z);
    }

    [[nodiscard]] rendern::SkinnedBounds MakeBounds(const BoundsAccumulator& acc) noexcept
    {
        rendern::SkinnedBounds b{};
        if (!acc.initialized)
        {
            return b;
        }
        b.aabbMin = acc.min;
        b.aabbMax = acc.max;
        b.sphereCenter = (acc.min + acc.max) * 0.5f;
        const mathUtils::Vec3 ext = acc.max - b.sphereCenter;
        b.sphereRadius = mathUtils::Length(ext);
        return b;
    }

    [[nodiscard]] rendern::SkinnedBounds MergeBounds(
        const rendern::SkinnedBounds& a,
        const rendern::SkinnedBounds& b) noexcept
    {
        if (a.sphereRadius <= 0.0f)
        {
            return b;
        }
        if (b.sphereRadius <= 0.0f)
        {
            return a;
        }

        BoundsAccumulator acc{};
        acc.initialized = true;
        acc.min = mathUtils::Vec3(
            std::min(a.aabbMin.x, b.aabbMin.x),
            std::min(a.aabbMin.y, b.aabbMin.y),
            std::min(a.aabbMin.z, b.aabbMin.z));
        acc.max = mathUtils::Vec3(
            std::max(a.aabbMax.x, b.aabbMax.x),
            std::max(a.aabbMax.y, b.aabbMax.y),
            std::max(a.aabbMax.z, b.aabbMax.z));
        return MakeBounds(acc);
    }

    [[nodiscard]] std::uint32_t DetermineClipSampleCount(const rendern::AnimationClip& clip) noexcept
    {
        if (clip.durationTicks <= 0.0f || clip.ticksPerSecond <= 0.0f)
        {
            return 1u;
        }

        const float durationSeconds = clip.durationTicks / clip.ticksPerSecond;
        const float targetFps = 60.0f;
        const std::uint32_t sampleCount = static_cast<std::uint32_t>(std::ceil(durationSeconds * targetFps)) + 1u;
        return std::clamp(sampleCount, 2u, 600u);
    }

    [[nodiscard]] rendern::SkinnedBounds ComputeClipBounds(
        const rendern::SkinnedMeshCPU& mesh,
        const rendern::AnimationClip& clip)
    {
        BoundsAccumulator acc{};
        for (const auto& v : mesh.vertices)
        {
            ExpandBounds(acc, mathUtils::Vec3(v.px, v.py, v.pz));
        }

        if (!rendern::IsValidAnimationClip(clip) || mesh.skeleton.bones.empty())
        {
            return MakeBounds(acc);
        }

        const std::vector<BindTRS> bindTrs = BuildBindTRS(mesh.skeleton);
        std::vector<mathUtils::Mat4> skinMatrices;

        const std::uint32_t sampleCount = DetermineClipSampleCount(clip);
        for (std::uint32_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
        {
            const float sampleT = (sampleCount <= 1)
                ? 0.0f
                : (clip.durationTicks * static_cast<float>(sampleIndex) / static_cast<float>(sampleCount - 1u));
            BuildSkinMatricesForSample(mesh.skeleton, clip, bindTrs, sampleT, mesh.skinningSkeletonToMeshSpace, skinMatrices);
            for (const auto& v : mesh.vertices)
            {
                ExpandBounds(acc, SkinVertexPosition(v, skinMatrices));
            }
        }

        return MakeBounds(acc);
    }
}

export namespace rendern
{
    struct AssimpSkinnedImportResult
    {
        SkinnedMeshCPU mesh;
        std::vector<AnimationClip> clips;
    };

    struct AssimpAnimationImportResult
    {
        std::vector<AnimationClip> clips;
        std::size_t sourceChannelCount{ 0 };
        std::size_t matchedChannelCount{ 0 };
        std::size_t ignoredChannelCount{ 0 };
        std::string diagnosticMessage{};
    };

    MeshCPU LoadAssimp(
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

    AssimpSkinnedImportResult LoadAssimpSkinnedAsset(
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

        std::unordered_map<unsigned, mathUtils::Mat4> meshOwnerGlobalByIndex;
        if (scene->mRootNode)
        {
            BuildMeshOwnerGlobalTransformsRecursive(scene->mRootNode, mathUtils::Mat4(1.0f), meshOwnerGlobalByIndex);
        }

        mathUtils::Mat4 commonMeshGlobal = scene->mRootNode
            ? ComputeNodeGlobalTransform(scene->mRootNode)
            : mathUtils::Mat4(1.0f);
        for (unsigned meshIndex : selectedMeshIndices)
        {
            if (const auto it = meshOwnerGlobalByIndex.find(meshIndex); it != meshOwnerGlobalByIndex.end())
            {
                commonMeshGlobal = it->second;
                break;
            }
        }

        AssimpSkinnedImportResult result{};
        SkinnedMeshCPU& out = result.mesh;
        out.skinningSkeletonToMeshSpace = mathUtils::Inverse(commonMeshGlobal);
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

            const mathUtils::Mat4 meshOwnerGlobal = [&]()
                {
                    if (const auto it = meshOwnerGlobalByIndex.find(meshIndex); it != meshOwnerGlobalByIndex.end())
                    {
                        return it->second;
                    }
                    return commonMeshGlobal;
                }();
            const mathUtils::Mat4 meshToCommon = out.skinningSkeletonToMeshSpace * meshOwnerGlobal;

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

                const mathUtils::Vec3 pCommon = mathUtils::TransformPoint(meshToCommon, mathUtils::Vec3(p.x, p.y, p.z));
                mathUtils::Vec3 nCommon = mathUtils::TransformVector(meshToCommon, mathUtils::Vec3(n.x, n.y, n.z));
                mathUtils::Vec3 tCommon = mathUtils::TransformVector(meshToCommon, mathUtils::Vec3(t.x, t.y, t.z));
                if (mathUtils::Length(nCommon) > 1e-6f)
                {
                    nCommon = mathUtils::Normalize(nCommon);
                }
                if (mathUtils::Length(tCommon) > 1e-6f)
                {
                    tCommon = mathUtils::Normalize(tCommon);
                }

                SkinnedVertexDesc v{};
                v.px = pCommon.x; v.py = pCommon.y; v.pz = pCommon.z;
                v.nx = nCommon.x; v.ny = nCommon.y; v.nz = nCommon.z;
                v.u = uv.x; v.v = uv.y;
                v.tx = tCommon.x; v.ty = tCommon.y; v.tz = tCommon.z; v.tw = 1.0f;
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
                
                const mathUtils::Mat4 adjustedInverseBind =
                    AiToMat4(bone->mOffsetMatrix) *
                    mathUtils::Inverse(meshOwnerGlobal) *
                    commonMeshGlobal;
                if (const auto [it, inserted] = inverseBindByName.try_emplace(boneName, adjustedInverseBind); !inserted)
                {
                    if (!MatricesNearlyEqual(it->second, adjustedInverseBind))
                    {
                        throw std::runtime_error(
                            "Assimp skinned import found incompatible mesh-owner bind spaces for bone '" +
                            boneName +
                            "' in " +
                            path.string() +
                            ". Import a single skinned submesh or normalize the source rig hierarchy.");
                    }
                }

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

        result.clips = BuildAnimationClipsFromScene(scene, out.skeleton);
        out.bounds.perClipBounds.clear();
        out.bounds.maxAnimatedBounds = out.bounds.bindPoseBounds;
        out.bounds.perClipBounds.reserve(result.clips.size());
        for (const auto& clip : result.clips)
        {
            if (!IsValidAnimationClip(clip))
            {
                continue;
            }

            PerClipBounds perClip{};
            perClip.clipName = clip.name;
            perClip.bounds = ComputeClipBounds(out, clip);
            out.bounds.maxAnimatedBounds = MergeBounds(out.bounds.maxAnimatedBounds, perClip.bounds);
            out.bounds.perClipBounds.push_back(std::move(perClip));
        }

        return result;
    }

    AssimpAnimationImportResult LoadAssimpAnimationClips(
        const std::filesystem::path& pathIn,
        const Skeleton& targetSkeleton,
        bool flipUVs = true)
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
        if (!scene || scene->mNumAnimations == 0)
        {
            const char* err = importer.GetErrorString();
            std::string msg = "Assimp failed to load animation clips: " + path.string();
            if (err && *err)
            {
                msg += " | ";
                msg += err;
            }
            throw std::runtime_error(msg);
        }

        AssimpAnimationImportResult result{};
        result.clips.reserve(scene->mNumAnimations);
        for (unsigned animationIndex = 0; animationIndex < scene->mNumAnimations; ++animationIndex)
        {
            const aiAnimation* anim = scene->mAnimations[animationIndex];
            if (!anim)
            {
                continue;
            }

            AnimationClip clip = BuildAnimationClip(*anim, targetSkeleton, animationIndex);
            result.sourceChannelCount += clip.channels.size();

            std::vector<BoneAnimationChannel> filteredChannels;
            filteredChannels.reserve(clip.channels.size());
            for (auto& channel : clip.channels)
            {
                if (channel.boneIndex < 0)
                {
                    ++result.ignoredChannelCount;
                    continue;
                }

                ++result.matchedChannelCount;
                filteredChannels.push_back(std::move(channel));
            }

            clip.channels = std::move(filteredChannels);
            if (!clip.channels.empty())
            {
                result.clips.push_back(std::move(clip));
            }
        }

        if (result.matchedChannelCount == 0 || result.clips.empty())
        {
            throw std::runtime_error(
                "Assimp animation import found no compatible bone channels for target skeleton: " +
                path.string());
        }

        return result;
    }

    SkinnedMeshCPU LoadAssimpSkinned(
        const std::filesystem::path& pathIn,
        bool flipUVs = true,
        std::optional<std::uint32_t> submeshIndex = std::nullopt)
    {
        return LoadAssimpSkinnedAsset(pathIn, flipUVs, submeshIndex).mesh;
    }
}