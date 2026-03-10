module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>

export module core:skeleton;

import :math_utils;

export namespace rendern
{
	struct SkeletonBone
	{
		std::string name;
		int parentIndex{ -1 };
		mathUtils::Mat4 inverseBindMatrix{ 1.0f };
		mathUtils::Mat4 bindLocalTransform{ 1.0f };
	};

	struct Skeleton
	{
		std::vector<SkeletonBone> bones;
		std::unordered_map<std::string, std::uint32_t> boneIndexByName;
		std::uint32_t rootBoneIndex{ 0 };
	};

	inline void RebuildBoneNameLookup(Skeleton& skeleton)
	{
		skeleton.boneIndexByName.clear();
		skeleton.boneIndexByName.reserve(skeleton.bones.size());
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(skeleton.bones.size()); ++i)
		{
			skeleton.boneIndexByName.emplace(skeleton.bones[i].name, i);
		}
	}

	[[nodiscard]] inline std::optional<std::uint32_t> FindBoneIndex(const Skeleton& skeleton, std::string_view boneName)
	{
		if (const auto it = skeleton.boneIndexByName.find(std::string(boneName)); it != skeleton.boneIndexByName.end())
		{
			return it->second;
		}
		return std::nullopt;
	}

	[[nodiscard]] inline bool IsValidSkeleton(const Skeleton& skeleton) noexcept
	{
		if (skeleton.bones.empty())
		{
			return false;
		}
		if (skeleton.rootBoneIndex >= skeleton.bones.size())
		{
			return false;
		}
		for (std::size_t i = 0; i < skeleton.bones.size(); ++i)
		{
			const int parentIndex = skeleton.bones[i].parentIndex;
			if (parentIndex >= static_cast<int>(skeleton.bones.size()))
			{
				return false;
			}
			if (parentIndex == static_cast<int>(i))
			{
				return false;
			}
		}
		return true;
	}
}