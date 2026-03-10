module;

#include <cstdint>
#include <string>
#include <vector>

export module core:animation_clip;

import :math_utils;

export namespace rendern
{
	struct TranslationKey
	{
		float timeTicks{ 0.0f };
		mathUtils::Vec3 value{ 0.0f, 0.0f, 0.0f };
	};

	struct RotationKey
	{
		float timeTicks{ 0.0f };
		// Normalized quaternion stored as (x, y, z, w).
		mathUtils::Vec4 value{ 0.0f, 0.0f, 0.0f, 1.0f };
	};

	struct ScaleKey
	{
		float timeTicks{ 0.0f };
		mathUtils::Vec3 value{ 1.0f, 1.0f, 1.0f };
	};

	struct BoneAnimationChannel
	{
		std::string boneName;
		int boneIndex{ -1 };
		std::vector<TranslationKey> translationKeys;
		std::vector<RotationKey> rotationKeys;
		std::vector<ScaleKey> scaleKeys;
	};

	struct AnimationClip
	{
		std::string name;
		float durationTicks{ 0.0f };
		float ticksPerSecond{ 25.0f };
		bool looping{ true };
		std::vector<BoneAnimationChannel> channels;
	};

	[[nodiscard]] inline bool IsValidAnimationClip(const AnimationClip& clip) noexcept
	{
		return !clip.name.empty() && clip.durationTicks >= 0.0f && clip.ticksPerSecond > 0.0f;
	}
}