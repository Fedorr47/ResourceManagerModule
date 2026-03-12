module;

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

export module core:animator;

import :animation_clip;
import :math_utils;
import :skeleton;

export namespace rendern
{
	struct LocalBoneTransform
	{
		mathUtils::Vec3 translation{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
		mathUtils::Vec3 scale{ 1.0f, 1.0f, 1.0f };
	};

	struct AnimatorState
	{
		const Skeleton* skeleton{ nullptr };
		const AnimationClip* clip{ nullptr };

		float timeSeconds{ 0.0f };
		float playRate{ 1.0f };
		bool looping{ true };
		bool paused{ false };

		std::vector<int> channelIndexByBone;
		std::vector<LocalBoneTransform> localPose;
		std::vector<mathUtils::Mat4> localMatrices;
		std::vector<mathUtils::Mat4> globalMatrices;
		std::vector<mathUtils::Mat4> skinMatrices;
	};

	[[nodiscard]] inline LocalBoneTransform BlendLocalBoneTransform(
		const LocalBoneTransform& from,
		const LocalBoneTransform& to,
		float alpha) noexcept
	{
		const float t = std::clamp(alpha, 0.0f, 1.0f);
		LocalBoneTransform out{};
		out.translation = mathUtils::Lerp(from.translation, to.translation, t);
		out.rotation = NlerpQuat(from.rotation, to.rotation, t);
		out.scale = mathUtils::Lerp(from.scale, to.scale, t);
		return out;
	}

	inline void BlendLocalPoses(
		std::vector<LocalBoneTransform>& outPose,
		const std::vector<LocalBoneTransform>& fromPose,
		const std::vector<LocalBoneTransform>& toPose,
		float alpha)
	{
		const std::size_t boneCount = std::min(fromPose.size(), toPose.size());
		if (boneCount == 0)
		{
			outPose.clear();
			return;
		}

		outPose.resize(boneCount);
		for (std::size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
		{
			outPose[boneIndex] = BlendLocalBoneTransform(fromPose[boneIndex], toPose[boneIndex], alpha);
		}
	}

	namespace detail
	{
		[[nodiscard]] inline const AnimationClip* GetClipByIndex(
			const std::vector<AnimationClip>& clips,
			int clipIndex) noexcept
		{
			if (clipIndex < 0 || static_cast<std::size_t>(clipIndex) >= clips.size())
			{
				return nullptr;
			}
			return &clips[static_cast<std::size_t>(clipIndex)];
		}

		inline void ApplyLegacyClipSettings(
			AnimatorState& state,
			const AnimationClip* clip,
			bool loop,
			float playRate,
			bool resetTime) noexcept
		{
			state.clip = clip;
			state.playRate = playRate;
			state.looping = (clip != nullptr) ? (loop && clip->looping) : loop;

			if (resetTime)
			{
				state.timeSeconds = 0.0f;
			}
		}

		inline void SyncSkeletonPointer(AnimatorState& state, const Skeleton& skeleton)
		{
			if (state.skeleton != &skeleton)
			{
				state.skeleton = &skeleton;
			}
		}
	}

	[[nodiscard]] inline bool IsAnimatorReady(const AnimatorState& state) noexcept
	{
		return state.skeleton != nullptr && IsValidSkeleton(*state.skeleton);
	}

	[[nodiscard]] inline std::vector<LocalBoneTransform> BuildBindPoseLocalPose(const Skeleton& skeleton)
	{
		std::vector<LocalBoneTransform> bindPose;
		bindPose.resize(skeleton.bones.size());

		for (std::size_t boneIndex = 0; boneIndex < skeleton.bones.size(); ++boneIndex)
		{
			DecomposeTRS(
				skeleton.bones[boneIndex].bindLocalTransform,
				bindPose[boneIndex].translation,
				bindPose[boneIndex].rotation,
				bindPose[boneIndex].scale);
		}

		return bindPose;
	}

	inline void ResetAnimatorToBindPose(AnimatorState& state)
	{
		if (!IsAnimatorReady(state))
		{
			state.channelIndexByBone.clear();
			state.localPose.clear();
			state.localMatrices.clear();
			state.globalMatrices.clear();
			state.skinMatrices.clear();
			return;
		}

		const std::size_t boneCount = state.skeleton->bones.size();
		state.channelIndexByBone.assign(boneCount, -1);
		state.localPose = BuildBindPoseLocalPose(*state.skeleton);
		state.localMatrices.assign(boneCount, mathUtils::Mat4(1.0f));
		state.globalMatrices.assign(boneCount, mathUtils::Mat4(1.0f));
		state.skinMatrices.assign(boneCount, mathUtils::Mat4(1.0f));
	}

	inline void ResetAnimatorToBindPose(AnimatorState& state, const Skeleton& skeleton)
	{
		detail::SyncSkeletonPointer(state, skeleton);
		ResetAnimatorToBindPose(state);
	}

	inline void RebuildAnimatorClipBinding(AnimatorState& state)
	{
		if (!IsAnimatorReady(state))
		{
			state.channelIndexByBone.clear();
			return;
		}

		state.channelIndexByBone.assign(state.skeleton->bones.size(), -1);
		if (state.clip == nullptr)
		{
			return;
		}

		for (std::size_t channelIndex = 0; channelIndex < state.clip->channels.size(); ++channelIndex)
		{
			const BoneAnimationChannel& channel = state.clip->channels[channelIndex];
			int boneIndex = channel.boneIndex;

			if (boneIndex < 0)
			{
				if (const auto found = FindBoneIndex(*state.skeleton, channel.boneName))
				{
					boneIndex = static_cast<int>(*found);
				}
			}

			if (boneIndex < 0 || boneIndex >= static_cast<int>(state.channelIndexByBone.size()))
			{
				continue;
			}

			state.channelIndexByBone[static_cast<std::size_t>(boneIndex)] = static_cast<int>(channelIndex);
		}
	}

	inline void InitializeAnimator(AnimatorState& state, const Skeleton* skeleton, const AnimationClip* clip = nullptr)
	{
		state = {};
		state.skeleton = skeleton;
		state.clip = clip;
		state.timeSeconds = 0.0f;
		state.playRate = 1.0f;
		state.looping = (clip != nullptr) ? clip->looping : true;
		state.paused = false;

		ResetAnimatorToBindPose(state);
		RebuildAnimatorClipBinding(state);
	}

	inline void InitializeAnimator(
		AnimatorState& state,
		const Skeleton& skeleton,
		const std::vector<AnimationClip>& clips,
		int clipIndex = -1,
		bool loop = true,
		float playRate = 1.0f)
	{
		const AnimationClip* clip = detail::GetClipByIndex(clips, clipIndex);

		state = {};
		state.skeleton = &skeleton;
		state.paused = false;

		detail::ApplyLegacyClipSettings(state, clip, loop, playRate, true);
		ResetAnimatorToBindPose(state);
		RebuildAnimatorClipBinding(state);
	}

	inline void SetAnimatorClip(AnimatorState& state, const AnimationClip* clip, bool resetTime = true)
	{
		state.clip = clip;

		if (clip != nullptr)
		{
			state.looping = clip->looping;
		}

		if (resetTime)
		{
			state.timeSeconds = 0.0f;
		}

		ResetAnimatorToBindPose(state);
		RebuildAnimatorClipBinding(state);
	}

	inline void SetAnimatorClip(
		AnimatorState& state,
		const AnimationClip* clip,
		bool loop,
		float playRate,
		bool resetTime = true)
	{
		detail::ApplyLegacyClipSettings(state, clip, loop, playRate, resetTime);
		ResetAnimatorToBindPose(state);
		RebuildAnimatorClipBinding(state);
	}

	inline void SetAnimatorClip(
		AnimatorState& state,
		const std::vector<AnimationClip>& clips,
		int clipIndex,
		bool loop = true,
		float playRate = 1.0f,
		bool resetTime = true)
	{
		const AnimationClip* clip = detail::GetClipByIndex(clips, clipIndex);
		detail::ApplyLegacyClipSettings(state, clip, loop, playRate, resetTime);
		ResetAnimatorToBindPose(state);
		RebuildAnimatorClipBinding(state);
	}

	inline void SetAnimatorClip(
		AnimatorState& state,
		const Skeleton& skeleton,
		const std::vector<AnimationClip>& clips,
		int clipIndex,
		bool loop = true,
		float playRate = 1.0f,
		bool resetTime = true)
	{
		detail::SyncSkeletonPointer(state, skeleton);
		SetAnimatorClip(state, clips, clipIndex, loop, playRate, resetTime);
	}

	inline void EvaluateAnimatorLocalPose(AnimatorState& state)
	{
		if (!IsAnimatorReady(state))
		{
			return;
		}

		if (state.localPose.size() != state.skeleton->bones.size())
		{
			ResetAnimatorToBindPose(state);
			RebuildAnimatorClipBinding(state);
		}

		state.localPose = BuildBindPoseLocalPose(*state.skeleton);

		if (state.clip == nullptr || !IsValidAnimationClip(*state.clip))
		{
			return;
		}

		const float timeSeconds = NormalizeAnimationTimeSeconds(*state.clip, state.timeSeconds, state.looping);
		const float timeTicks = timeSeconds * state.clip->ticksPerSecond;

		for (std::size_t boneIndex = 0; boneIndex < state.localPose.size(); ++boneIndex)
		{
			const int channelIndex =
				(boneIndex < state.channelIndexByBone.size())
				? state.channelIndexByBone[boneIndex]
				: -1;

			if (channelIndex < 0 || channelIndex >= static_cast<int>(state.clip->channels.size()))
			{
				continue;
			}

			const BoneAnimationChannel& channel = state.clip->channels[static_cast<std::size_t>(channelIndex)];
			LocalBoneTransform& dst = state.localPose[boneIndex];

			dst.translation = SampleTranslationKeys(channel.translationKeys, timeTicks, dst.translation);
			dst.rotation = SampleRotationKeys(channel.rotationKeys, timeTicks, dst.rotation);
			dst.scale = SampleScaleKeys(channel.scaleKeys, timeTicks, dst.scale);
		}
	}

	inline void EvaluateAnimatorLocalPose(
		AnimatorState& state,
		const Skeleton& skeleton,
		const std::vector<AnimationClip>& clips)
	{
		detail::SyncSkeletonPointer(state, skeleton);

		if (state.clip == nullptr && !clips.empty())
		{
			RebuildAnimatorClipBinding(state);
		}

		EvaluateAnimatorLocalPose(state);
	}

	inline void BuildAnimatorMatrices(AnimatorState& state)
	{
		if (!IsAnimatorReady(state))
		{
			return;
		}

		const std::size_t boneCount = state.skeleton->bones.size();
		if (state.localPose.size() != boneCount)
		{
			ResetAnimatorToBindPose(state);
		}

		state.localMatrices.resize(boneCount, mathUtils::Mat4(1.0f));
		state.globalMatrices.resize(boneCount, mathUtils::Mat4(1.0f));
		state.skinMatrices.resize(boneCount, mathUtils::Mat4(1.0f));

		for (std::size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
		{
			const LocalBoneTransform& trs = state.localPose[boneIndex];
			state.localMatrices[boneIndex] = ComposeTRS(trs.translation, trs.rotation, trs.scale);
		}

		for (std::size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
		{
			const int parentIndex = state.skeleton->bones[boneIndex].parentIndex;

			if (parentIndex >= 0)
			{
				state.globalMatrices[boneIndex] =
					state.globalMatrices[static_cast<std::size_t>(parentIndex)] *
					state.localMatrices[boneIndex];
			}
			else
			{
				state.globalMatrices[boneIndex] = state.localMatrices[boneIndex];
			}

			state.skinMatrices[boneIndex] =
				state.globalMatrices[boneIndex] *
				state.skeleton->bones[boneIndex].inverseBindMatrix;
		}
	}

	inline void BuildAnimatorMatrices(AnimatorState& state, const Skeleton& skeleton)
	{
		detail::SyncSkeletonPointer(state, skeleton);
		BuildAnimatorMatrices(state);
	}

	inline void EvaluateAnimator(AnimatorState& state)
	{
		EvaluateAnimatorLocalPose(state);
		BuildAnimatorMatrices(state);
	}

	inline void EvaluateAnimator(
		AnimatorState& state,
		const Skeleton& skeleton,
		const std::vector<AnimationClip>& clips)
	{
		detail::SyncSkeletonPointer(state, skeleton);

		if (state.clip == nullptr && !clips.empty())
		{
			RebuildAnimatorClipBinding(state);
		}

		EvaluateAnimator(state);
	}

	inline void AdvanceAnimator(AnimatorState& state, float deltaSeconds) noexcept
	{
		if (state.paused || state.clip == nullptr || !IsValidAnimationClip(*state.clip))
		{
			return;
		}

		state.timeSeconds += deltaSeconds * state.playRate;
		state.timeSeconds = NormalizeAnimationTimeSeconds(*state.clip, state.timeSeconds, state.looping);
	}

	inline void AdvanceAnimator(
		AnimatorState& state,
		const std::vector<AnimationClip>& /*clips*/,
		float deltaSeconds) noexcept
	{
		AdvanceAnimator(state, deltaSeconds);
	}

	inline void UpdateAnimator(AnimatorState& state, float deltaSeconds)
	{
		AdvanceAnimator(state, deltaSeconds);
		EvaluateAnimator(state);
	}

	inline void UpdateAnimator(
		AnimatorState& state,
		const Skeleton& skeleton,
		const std::vector<AnimationClip>& clips,
		float deltaSeconds)
	{
		detail::SyncSkeletonPointer(state, skeleton);

		if (state.clip == nullptr && !clips.empty())
		{
			RebuildAnimatorClipBinding(state);
		}

		UpdateAnimator(state, deltaSeconds);
	}
}