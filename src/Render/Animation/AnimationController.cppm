module;

#include <algorithm>
#include <concepts>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <cstddef>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <utility>

export module core:animation_controller;

import :animation_clip;
import :animator;
import :skeleton;

export namespace rendern
{
	enum class AnimationParameterType : std::uint8_t
	{
		Bool = 0,
		Int = 1,
		Float = 2,
		Trigger = 3
	};

	struct AnimationParameterValue
	{
		AnimationParameterType type{ AnimationParameterType::Bool };
		bool boolValue{ false };
		int intValue{ 0 };
		float floatValue{ 0.0f };
		bool triggerValue{ false };
	};

	struct AnimationParameterStore
	{
		std::unordered_map<std::string, AnimationParameterValue> values;
	};

	enum class AnimationConditionOp : std::uint8_t
	{
		IfTrue = 0,
		IfFalse,
		Greater,
		GreaterEqual,
		Less,
		LessEqual,
		Equal,
		NotEqual,
		Triggered
	};

	struct AnimationParameterDesc
	{
		std::string name;
		AnimationParameterValue defaultValue{};
	};

	struct AnimationBlend1DPoint
	{
		std::string clipName;
		float value{ 0.0f };
	};

	struct AnimationNotifyDesc
	{
		std::string id;
		float timeNormalized{ 0.0f };
		bool fireOnEnter{ false };
	};

	struct AnimationStateDesc
	{
		std::string name;
		std::string clipName;
		std::string clipSourceAssetId;
		std::string blendParameter;
		std::vector<AnimationBlend1DPoint> blend1D;
		std::vector<AnimationNotifyDesc> notifies;
		bool looping{ true };
		float playRate{ 1.0f };
	};

	struct AnimationConditionDesc
	{
		std::string parameter;
		AnimationConditionOp op{ AnimationConditionOp::IfTrue };
		AnimationParameterValue value{};
	};

	struct AnimationTransitionDesc
	{
		std::string fromState;
		std::string toState;
		bool hasExitTime{ false };
		float exitTimeNormalized{ 1.0f };
		float blendDurationSeconds{ 0.15f };
		std::vector<AnimationConditionDesc> conditions;
	};

	struct AnimationControllerAsset
	{
		std::string id;
		std::string defaultState;
		std::vector<AnimationParameterDesc> parameters;
		std::vector<AnimationStateDesc> states;
		std::vector<AnimationTransitionDesc> transitions;
	};

	enum class AnimationRootMotionMode : std::uint8_t
	{
		InPlace = 0,
		Allow = 1
	};

	struct AnimationNotifyEvent
	{
		std::uint64_t sequence{ 0 };
		std::string id;
		std::string stateName;
		std::string clipName;
		float normalizedTime{ 0.0f };
	};

	enum class AnimationControllerMode : std::uint8_t
	{
		LegacyClip = 0,
		StateMachine = 1
	};

	struct AnimationControllerRuntime
	{
		AnimationControllerMode mode{ AnimationControllerMode::LegacyClip };
		AnimationRootMotionMode rootMotionMode{ AnimationRootMotionMode::InPlace };
		std::string rootMotionBoneName;
		const Skeleton* skeleton{ nullptr };
		const std::vector<AnimationClip>* clips{ nullptr };
		const std::vector<std::string>* clipSourceAssetIds{ nullptr };

		std::string controllerAssetId;
		std::string currentStateName;
		std::string requestedStateName;
		const AnimationControllerAsset* stateMachineAsset{ nullptr };
		int currentStateIndex{ -1 };
		std::vector<int> resolvedStateClipIndices;
		std::vector<std::vector<int>> resolvedStateBlendClipIndices;

		bool currentStateUsesBlend1D{ false };
		std::string currentBlendParameterName;
		float currentBlendParameterValue{ 0.0f };
		std::string currentBlendPrimaryClipName;
		std::string currentBlendSecondaryClipName;
		AnimatorState blendSecondaryAnimator{};
		int blendSecondaryClipIndex{ -1 };
		float blendSecondaryAlpha{ 0.0f };

		bool transitionActive{ false };
		int transitionSourceStateIndex{ -1 };
		std::string transitionSourceStateName;
		float transitionElapsedSeconds{ 0.0f };
		float transitionDurationSeconds{ 0.0f };
		AnimatorState transitionSourceAnimator{};
		AnimatorState transitionSourceBlendSecondaryAnimator{};
		int transitionSourceSecondaryClipIndex{ -1 };
		float transitionSourceSecondaryAlpha{ 0.0f };

		int legacyClipIndex{ -1 };
		bool autoplay{ true };
		bool looping{ true };
		float playRate{ 1.0f };
		bool paused{ false };
		bool forceBindPose{ false };
		mathUtils::Vec3 lastAppliedRootMotionDelta{ 0.0f, 0.0f, 0.0f };
		float previousStateNormalizedTime{ 0.0f };
		bool stateEnteredThisFrame{ true };
		std::uint64_t nextNotifySequence{ 0 };
		std::vector<AnimationNotifyEvent> pendingNotifyEvents;

		AnimationParameterStore parameters{};
	};

	[[nodiscard]] inline const AnimationClip* ResolveLegacyAnimationClip(const AnimationControllerRuntime& runtime) noexcept;

	namespace detail
	{

		template<typename T>
		concept AnimationReadableType =
			std::same_as<std::remove_cvref_t<T>, bool> ||
			std::same_as<std::remove_cvref_t<T>, int> ||
			std::same_as<std::remove_cvref_t<T>, float>;

		template<AnimationReadableType T>
		[[nodiscard]] inline T GetParameterAs(const AnimationParameterValue& value) noexcept
		{
			using ValueT = std::remove_cvref_t<T>;

			switch (value.type)
			{
			case AnimationParameterType::Bool:
				if constexpr (std::same_as<ValueT, bool>)
					return value.boolValue;
				else if constexpr (std::same_as<ValueT, int>)
					return value.boolValue ? 1 : 0;
				else
					return value.boolValue ? 1.0f : 0.0f;

			case AnimationParameterType::Int:
				if constexpr (std::same_as<ValueT, bool>)
					return value.intValue != 0;
				else if constexpr (std::same_as<ValueT, int>)
					return value.intValue;
				else
					return static_cast<float>(value.intValue);

			case AnimationParameterType::Float:
				if constexpr (std::same_as<ValueT, bool>)
					return std::fabs(value.floatValue) > 1e-6f;
				else if constexpr (std::same_as<ValueT, int>)
					return static_cast<int>(value.floatValue);
				else
					return value.floatValue;

			case AnimationParameterType::Trigger:
				if constexpr (std::same_as<ValueT, bool>)
					return value.triggerValue;
				else if constexpr (std::same_as<ValueT, int>)
					return value.triggerValue ? 1 : 0;
				else
					return value.triggerValue ? 1.0f : 0.0f;

			default:
				if constexpr (std::same_as<ValueT, bool>)
					return false;
				else if constexpr (std::same_as<ValueT, int>)
					return 0;
				else
					return 0.0f;
			}
		}

		[[nodiscard]] inline bool GetParameterAsBool(const AnimationParameterValue& value) noexcept
		{
			return GetParameterAs<bool>(value);
		}

		[[nodiscard]] inline int GetParameterAsInt(const AnimationParameterValue& value) noexcept
		{
			return GetParameterAs<int>(value);
		}

		[[nodiscard]] inline float GetParameterAsFloat(const AnimationParameterValue& value) noexcept
		{
			return GetParameterAs<float>(value);
		}

		[[nodiscard]] inline int ResolveClipIndexByName(
			const std::vector<AnimationClip>& clips,
			std::string_view clipName) noexcept
		{
			if (clipName.empty())
			{
				return -1;
			}
			for (std::size_t i = 0; i < clips.size(); ++i)
			{
				if (clips[i].name == clipName)
				{
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		[[nodiscard]] inline const AnimationClip* ResolveClipByIndex(
			const std::vector<AnimationClip>* clips,
			int clipIndex) noexcept
		{
			if (clips == nullptr || clipIndex < 0 || static_cast<std::size_t>(clipIndex) >= clips->size())
			{
				return nullptr;
			}
			return &(*clips)[static_cast<std::size_t>(clipIndex)];
		}

		[[nodiscard]] inline float ClipDurationSeconds(const AnimationClip* clip) noexcept
		{
			if (clip == nullptr || !IsValidAnimationClip(*clip) || clip->ticksPerSecond <= 0.0f)
			{
				return 0.0f;
			}
			return clip->durationTicks / clip->ticksPerSecond;
		}


		[[nodiscard]] inline int ResolveClipIndexForState(
			const AnimationControllerRuntime& runtime,
			const AnimationStateDesc& state) noexcept
		{
			if (runtime.clips == nullptr)
			{
				return -1;
			}

			const std::vector<AnimationClip>& clips = *runtime.clips;

			const bool hasSourceIds =
				runtime.clipSourceAssetIds != nullptr &&
				runtime.clipSourceAssetIds->size() == clips.size();

			const bool useSourceFilter =
				!state.clipSourceAssetId.empty() && hasSourceIds;

			if (useSourceFilter)
			{
				if (!state.clipName.empty())
				{
					for (std::size_t i = 0; i < clips.size(); ++i)
					{
						if ((*runtime.clipSourceAssetIds)[i] == state.clipSourceAssetId &&
							clips[i].name == state.clipName)
						{
							return static_cast<int>(i);
						}
					}
				}

				for (std::size_t i = 0; i < clips.size(); ++i)
				{
					if ((*runtime.clipSourceAssetIds)[i] == state.clipSourceAssetId)
					{
						return static_cast<int>(i);
					}
				}
			}

			return ResolveClipIndexByName(clips, state.clipName);
		}

		inline void SetAnimatorNormalizedTime(AnimatorState& animator, float normalizedTime) noexcept
		{
			const float durationSeconds = ClipDurationSeconds(animator.clip);
			if (durationSeconds <= 0.0f)
			{
				animator.timeSeconds = 0.0f;
				return;
			}
			animator.timeSeconds = std::clamp(normalizedTime, 0.0f, 1.0f) * durationSeconds;
		}

		struct StateSampleConfig
		{
			const AnimationStateDesc* state{ nullptr };
			int primaryClipIndex{ -1 };
			int secondaryClipIndex{ -1 };
			float secondaryAlpha{ 0.0f };
			bool usesBlend1D{ false };
			std::string parameterName;
			float parameterValue{ 0.0f };
		};

		[[nodiscard]] inline StateSampleConfig BuildStateSampleConfig(
			const AnimationControllerRuntime& runtime,
			int stateIndex) noexcept
		{
			StateSampleConfig sample{};
			if (runtime.stateMachineAsset == nullptr ||
				stateIndex < 0 ||
				static_cast<std::size_t>(stateIndex) >= runtime.stateMachineAsset->states.size())
			{
				return sample;
			}

			const AnimationStateDesc& state = runtime.stateMachineAsset->states[static_cast<std::size_t>(stateIndex)];
			sample.state = &state;
			sample.primaryClipIndex =
				(static_cast<std::size_t>(stateIndex) < runtime.resolvedStateClipIndices.size())
				? runtime.resolvedStateClipIndices[static_cast<std::size_t>(stateIndex)]
				: -1;

			if (state.blendParameter.empty() || state.blend1D.empty())
			{
				return sample;
			}

			sample.usesBlend1D = true;
			sample.parameterName = state.blendParameter;
			if (auto it = runtime.parameters.values.find(state.blendParameter); it != runtime.parameters.values.end())
			{
				sample.parameterValue = GetParameterAsFloat(it->second);
			}

			const std::vector<int>* resolvedIndices =
				(static_cast<std::size_t>(stateIndex) < runtime.resolvedStateBlendClipIndices.size())
				? &runtime.resolvedStateBlendClipIndices[static_cast<std::size_t>(stateIndex)]
				: nullptr;
			if (resolvedIndices == nullptr || resolvedIndices->empty())
			{
				return sample;
			}

			if (resolvedIndices->size() == 1 || state.blend1D.size() == 1)
			{
				sample.primaryClipIndex = (*resolvedIndices)[0];
				return sample;
			}

			if (sample.parameterValue <= state.blend1D.front().value)
			{
				sample.primaryClipIndex = (*resolvedIndices)[0];
				return sample;
			}
			if (sample.parameterValue >= state.blend1D.back().value)
			{
				sample.primaryClipIndex = (*resolvedIndices)[resolvedIndices->size() - 1];
				return sample;
			}

			for (std::size_t pointIndex = 0; pointIndex + 1 < state.blend1D.size(); ++pointIndex)
			{
				const AnimationBlend1DPoint& a = state.blend1D[pointIndex];
				const AnimationBlend1DPoint& b = state.blend1D[pointIndex + 1];
				if (sample.parameterValue > b.value)
				{
					continue;
				}
				sample.primaryClipIndex = (*resolvedIndices)[pointIndex];
				sample.secondaryClipIndex = (*resolvedIndices)[pointIndex + 1];
				const float span = b.value - a.value;
				sample.secondaryAlpha = (std::fabs(span) > 1e-6f)
					? std::clamp((sample.parameterValue - a.value) / span, 0.0f, 1.0f)
					: 1.0f;
				break;
			}

			if (sample.primaryClipIndex < 0 && sample.secondaryClipIndex >= 0)
			{
				sample.primaryClipIndex = sample.secondaryClipIndex;
				sample.secondaryClipIndex = -1;
				sample.secondaryAlpha = 0.0f;
			}
			if (sample.primaryClipIndex == sample.secondaryClipIndex)
			{
				sample.secondaryClipIndex = -1;
				sample.secondaryAlpha = 0.0f;
			}
			if (sample.secondaryAlpha <= 1e-6f)
			{
				sample.secondaryClipIndex = -1;
				sample.secondaryAlpha = 0.0f;
			}
			return sample;
		}

		inline void ResolveStateClipIndices(AnimationControllerRuntime& runtime)
		{
			runtime.resolvedStateClipIndices.clear();
			runtime.resolvedStateBlendClipIndices.clear();
			if (runtime.stateMachineAsset == nullptr || runtime.clips == nullptr)
			{
				return;
			}
			runtime.resolvedStateClipIndices.resize(runtime.stateMachineAsset->states.size(), -1);
			runtime.resolvedStateBlendClipIndices.resize(runtime.stateMachineAsset->states.size());
			for (std::size_t i = 0; i < runtime.stateMachineAsset->states.size(); ++i)
			{
				const AnimationStateDesc& state = runtime.stateMachineAsset->states[i];
				if (!state.blend1D.empty())
				{
					auto& resolvedBlend = runtime.resolvedStateBlendClipIndices[i];
					resolvedBlend.reserve(state.blend1D.size());
					for (const AnimationBlend1DPoint& point : state.blend1D)
					{
						resolvedBlend.push_back(ResolveClipIndexByName(*runtime.clips, point.clipName));
					}
					runtime.resolvedStateClipIndices[i] = resolvedBlend.empty() ? -1 : resolvedBlend.front();
				}
				else
				{
					runtime.resolvedStateClipIndices[i] =
						ResolveClipIndexForState(runtime, runtime.stateMachineAsset->states[i]);
				}
			}
		}

		inline void ApplyParameterDefaults(AnimationParameterStore& store, const AnimationControllerAsset& asset)
		{
			store.values.clear();
			for (const AnimationParameterDesc& param : asset.parameters)
			{
				store.values[param.name] = param.defaultValue;
			}
		}

		[[nodiscard]] inline float GetAnimatorNormalizedTime(const AnimatorState& animator) noexcept
		{
			if (animator.clip == nullptr || !IsValidAnimationClip(*animator.clip) || animator.clip->ticksPerSecond <= 0.0f)
			{
				return 0.0f;
			}
			const float durationSeconds = animator.clip->durationTicks / animator.clip->ticksPerSecond;
			if (durationSeconds <= 0.0f)
			{
				return 0.0f;
			}
			const float normalized =
				NormalizeAnimationTimeSeconds(*animator.clip, animator.timeSeconds, animator.looping) /
				durationSeconds;
			return std::clamp(normalized, 0.0f, 1.0f);
		}

		[[nodiscard]] inline bool EvaluateCondition(
			const AnimationConditionDesc& condition,
			const AnimationParameterStore& store) noexcept
		{
			auto it = store.values.find(condition.parameter);
			if (it == store.values.end())
			{
				return false;
			}
			const AnimationParameterValue& param = it->second;

			switch (condition.op)
			{
			case AnimationConditionOp::IfTrue: return GetParameterAsBool(param);
			case AnimationConditionOp::IfFalse: return !GetParameterAsBool(param);
			case AnimationConditionOp::Greater: return GetParameterAsFloat(param) > GetParameterAsFloat(condition.value);
			case AnimationConditionOp::GreaterEqual: return GetParameterAsFloat(param) >= GetParameterAsFloat(condition.value);
			case AnimationConditionOp::Less: return GetParameterAsFloat(param) < GetParameterAsFloat(condition.value);
			case AnimationConditionOp::LessEqual: return GetParameterAsFloat(param) <= GetParameterAsFloat(condition.value);
			case AnimationConditionOp::Equal:
				if (condition.value.type == AnimationParameterType::Bool || param.type == AnimationParameterType::Bool)
				{
					return GetParameterAsBool(param) == GetParameterAsBool(condition.value);
				}
				if (condition.value.type == AnimationParameterType::Int || param.type == AnimationParameterType::Int)
				{
					return GetParameterAsInt(param) == GetParameterAsInt(condition.value);
				}
				return std::fabs(GetParameterAsFloat(param) - GetParameterAsFloat(condition.value)) <= 1e-6f;
			case AnimationConditionOp::NotEqual:
				if (condition.value.type == AnimationParameterType::Bool || param.type == AnimationParameterType::Bool)
				{
					return GetParameterAsBool(param) != GetParameterAsBool(condition.value);
				}
				if (condition.value.type == AnimationParameterType::Int || param.type == AnimationParameterType::Int)
				{
					return GetParameterAsInt(param) != GetParameterAsInt(condition.value);
				}
				return std::fabs(GetParameterAsFloat(param) - GetParameterAsFloat(condition.value)) > 1e-6f;
			case AnimationConditionOp::Triggered:
				return param.type == AnimationParameterType::Trigger && param.triggerValue;
			default:
				return false;
			}
		}

		inline void ConsumeTransitionTriggers(AnimationParameterStore& store, const AnimationTransitionDesc& transition)
		{
			for (const AnimationConditionDesc& condition : transition.conditions)
			{
				if (condition.op == AnimationConditionOp::Triggered)
				{
					auto it = store.values.find(condition.parameter);
					if (it != store.values.end())
					{
						it->second.triggerValue = false;
					}
				}
			}
		}

		[[nodiscard]] inline int FindStateIndexByName(
			const AnimationControllerAsset& asset,
			std::string_view stateName) noexcept
		{
			for (std::size_t i = 0; i < asset.states.size(); ++i)
			{
				if (asset.states[i].name == stateName)
				{
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		inline void SyncAnimatorClip(
			AnimatorState& animator,
			const Skeleton* skeleton,
			const AnimationClip* clip,
			bool looping,
			float playRate,
			bool paused,
			bool resetTime,
			float normalizedTime,
			bool syncNormalizedWhenUnchanged)
		{
			const bool needsInit = !IsAnimatorReady(animator) || animator.skeleton != skeleton;
			const bool clipChanged = needsInit || animator.clip != clip;
			if (needsInit)
			{
				InitializeAnimator(animator, skeleton, clip);
			}
			else if (clipChanged)
			{
				SetAnimatorClip(animator, clip, looping, playRate, true);
			}
			animator.looping = (clip != nullptr) ? (looping && clip->looping) : looping;
			animator.playRate = playRate;
			animator.paused = paused;
			if (resetTime)
			{
				animator.timeSeconds = 0.0f;
			}
			else if (clipChanged || syncNormalizedWhenUnchanged)
			{
				SetAnimatorNormalizedTime(animator, normalizedTime);
			}
		}

		inline void EvaluateAnimatorPairToLocalPose(
			AnimatorState& primaryAnimator,
			AnimatorState* secondaryAnimator,
			float secondaryAlpha)
		{
			EvaluateAnimatorLocalPose(primaryAnimator);
			if (secondaryAnimator != nullptr && IsAnimatorReady(*secondaryAnimator) && secondaryAnimator->clip != nullptr && secondaryAlpha > 1e-6f)
			{
				EvaluateAnimatorLocalPose(*secondaryAnimator);
				const std::vector<LocalBoneTransform> primaryPose = primaryAnimator.localPose;
				BlendLocalPoses(primaryAnimator.localPose, primaryPose, secondaryAnimator->localPose, secondaryAlpha);
			}
		}

		[[nodiscard]] inline char ToLowerAscii(char c) noexcept
		{
			return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}

		[[nodiscard]] inline bool ContainsInsensitive(std::string_view text, std::string_view needle) noexcept
		{
			if (needle.empty() || needle.size() > text.size())
			{
				return false;
			}
			for (std::size_t i = 0; i + needle.size() <= text.size(); ++i)
			{
				bool match = true;
				for (std::size_t j = 0; j < needle.size(); ++j)
				{
					if (ToLowerAscii(text[i + j]) != ToLowerAscii(needle[j]))
					{
						match = false;
						break;
					}
				}
				if (match)
				{
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] inline int GetBoneDepth(const Skeleton& skeleton, std::size_t boneIndex) noexcept
		{
			int depth = 0;
			int current = static_cast<int>(boneIndex);
			while (current >= 0 && static_cast<std::size_t>(current) < skeleton.bones.size())
			{
				current = skeleton.bones[static_cast<std::size_t>(current)].parentIndex;
				if (current >= 0)
				{
					++depth;
				}
			}
			return depth;
		}

		[[nodiscard]] inline bool ChannelHasMeaningfulTranslation(
			const Skeleton& skeleton,
			const BoneAnimationChannel& channel) noexcept
		{
			if (channel.boneIndex < 0 || static_cast<std::size_t>(channel.boneIndex) >= skeleton.bones.size())
			{
				return false;
			}

			mathUtils::Vec3 bindTranslation{ 0.0f, 0.0f, 0.0f };
			mathUtils::Vec4 bindRotation{ 0.0f, 0.0f, 0.0f, 1.0f };
			mathUtils::Vec3 bindScale{ 1.0f, 1.0f, 1.0f };
			DecomposeTRS(
				skeleton.bones[static_cast<std::size_t>(channel.boneIndex)].bindLocalTransform,
				bindTranslation,
				bindRotation,
				bindScale);

			for (const TranslationKey& key : channel.translationKeys)
			{
				const mathUtils::Vec3 delta = key.value - bindTranslation;
				if (std::fabs(delta.x) > 1e-4f ||
					std::fabs(delta.y) > 1e-4f ||
					std::fabs(delta.z) > 1e-4f)
				{
					return true;
				}
			}

			return false;
		}

		[[nodiscard]] inline std::size_t ResolveInPlaceMotionBoneIndex(
			const AnimationControllerRuntime& runtime,
			const AnimatorState& animator) noexcept
		{
			const Skeleton& skeleton = *animator.skeleton;
			const std::size_t rootIndex = static_cast<std::size_t>(skeleton.rootBoneIndex);
			if (rootIndex >= skeleton.bones.size())
			{
				return skeleton.bones.empty() ? 0u : skeleton.bones.size() - 1u;
			}

			if (!runtime.rootMotionBoneName.empty())
			{
				if (const auto explicitBone = FindBoneIndex(skeleton, runtime.rootMotionBoneName))
				{
					return static_cast<std::size_t>(*explicitBone);
				}
			}

			if (animator.clip == nullptr)
			{
				return rootIndex;
			}

			const auto scoreChannel = [&](const BoneAnimationChannel& channel) noexcept -> int
				{
					if (!ChannelHasMeaningfulTranslation(skeleton, channel))
					{
						return -1;
					}

					int score = 0;
					const std::string_view boneName = channel.boneName;
					if (ContainsInsensitive(boneName, "hips")) score += 200;
					if (ContainsInsensitive(boneName, "pelvis")) score += 180;
					if (ContainsInsensitive(boneName, "root")) score += 120;
					if (ContainsInsensitive(boneName, "master")) score += 80;
					if (ContainsInsensitive(boneName, "ctrl")) score -= 10;
					const std::size_t boneIndex = static_cast<std::size_t>(channel.boneIndex);
					score -= GetBoneDepth(skeleton, boneIndex) * 4;
					return score;
				};

			int bestScore = -1;
			std::size_t bestIndex = rootIndex;
			for (const BoneAnimationChannel& channel : animator.clip->channels)
			{
				if (channel.boneIndex < 0 || static_cast<std::size_t>(channel.boneIndex) >= skeleton.bones.size())
				{
					continue;
				}

				const int score = scoreChannel(channel);
				if (score > bestScore)
				{
					bestScore = score;
					bestIndex = static_cast<std::size_t>(channel.boneIndex);
				}
			}

			return bestIndex;
		}

		inline void ApplyRootMotionModeToAnimatorPose(AnimationControllerRuntime& runtime, AnimatorState& animator)
		{
			runtime.lastAppliedRootMotionDelta = mathUtils::Vec3(0.0f, 0.0f, 0.0f);

			if (runtime.rootMotionMode != AnimationRootMotionMode::InPlace ||
				!IsAnimatorReady(animator) ||
				animator.localPose.empty())
			{
				return;
			}

			const std::size_t motionBoneIndex = ResolveInPlaceMotionBoneIndex(runtime, animator);
			if (motionBoneIndex >= animator.localPose.size() || motionBoneIndex >= animator.skeleton->bones.size())
			{
				return;
			}

			mathUtils::Vec3 bindTranslation{ 0.0f, 0.0f, 0.0f };
			mathUtils::Vec4 bindRotation{ 0.0f, 0.0f, 0.0f, 1.0f };
			mathUtils::Vec3 bindScale{ 1.0f, 1.0f, 1.0f };
			DecomposeTRS(
				animator.skeleton->bones[motionBoneIndex].bindLocalTransform,
				bindTranslation,
				bindRotation,
				bindScale);

			LocalBoneTransform& motionTransform = animator.localPose[motionBoneIndex];
			runtime.lastAppliedRootMotionDelta = mathUtils::Vec3(
				motionTransform.translation.x - bindTranslation.x,
				0.0f,
				motionTransform.translation.z - bindTranslation.z);
			motionTransform.translation.x = bindTranslation.x;
			motionTransform.translation.z = bindTranslation.z;
		}

		inline void PushNotifyEvent(
			AnimationControllerRuntime& runtime,
			const AnimationStateDesc& state,
			const AnimationNotifyDesc& notify,
			const AnimationClip* clip)
		{
			AnimationNotifyEvent event{};
			event.sequence = ++runtime.nextNotifySequence;
			event.id = notify.id;
			event.stateName = state.name;
			event.clipName = (clip != nullptr) ? clip->name : std::string{};
			event.normalizedTime = std::clamp(notify.timeNormalized, 0.0f, 1.0f);
			runtime.pendingNotifyEvents.push_back(std::move(event));

			constexpr std::size_t kMaxRetainedNotifyEvents = 64;
			if (runtime.pendingNotifyEvents.size() > kMaxRetainedNotifyEvents)
			{
				runtime.pendingNotifyEvents.erase(
					runtime.pendingNotifyEvents.begin(),
					runtime.pendingNotifyEvents.begin() + static_cast<std::ptrdiff_t>(runtime.pendingNotifyEvents.size() - kMaxRetainedNotifyEvents));
			}
		}

		[[nodiscard]] inline bool DidNormalizedTimePass(
			float previousNormalizedTime,
			float currentNormalizedTime,
			float notifyTime,
			bool looping) noexcept
		{
			const float t = std::clamp(notifyTime, 0.0f, 1.0f);
			constexpr float kEpsilon = 1e-6f;

			if (!looping || currentNormalizedTime >= previousNormalizedTime)
			{
				return t > previousNormalizedTime + kEpsilon && t <= currentNormalizedTime + kEpsilon;
			}

			return t > previousNormalizedTime + kEpsilon || t <= currentNormalizedTime + kEpsilon;
		}

		inline void QueueCurrentStateNotifies(AnimationControllerRuntime& runtime, const AnimatorState& animator)
		{
			if (runtime.stateMachineAsset == nullptr ||
				runtime.currentStateIndex < 0 ||
				static_cast<std::size_t>(runtime.currentStateIndex) >= runtime.stateMachineAsset->states.size())
			{
				runtime.previousStateNormalizedTime = 0.0f;
				runtime.stateEnteredThisFrame = false;
				return;
			}

			const AnimationStateDesc& state = runtime.stateMachineAsset->states[static_cast<std::size_t>(runtime.currentStateIndex)];
			const float currentNormalizedTime = GetAnimatorNormalizedTime(animator);

			if (!state.notifies.empty())
			{
				const bool looping = animator.clip != nullptr && animator.looping && animator.clip->looping;
				for (const AnimationNotifyDesc& notify : state.notifies)
				{
					if (notify.id.empty())
					{
						continue;
					}

					const float notifyTime = std::clamp(notify.timeNormalized, 0.0f, 1.0f);
					if (runtime.stateEnteredThisFrame && (notify.fireOnEnter || notifyTime <= 1e-6f))
					{
						PushNotifyEvent(runtime, state, notify, animator.clip);
						continue;
					}

					if (DidNormalizedTimePass(runtime.previousStateNormalizedTime, currentNormalizedTime, notifyTime, looping))
					{
						PushNotifyEvent(runtime, state, notify, animator.clip);
					}
				}
			}

			runtime.previousStateNormalizedTime = currentNormalizedTime;
			runtime.stateEnteredThisFrame = false;
		}

		inline void SyncRuntimeBlendMetadata(AnimationControllerRuntime& runtime, const StateSampleConfig& sample)
		{
			runtime.currentStateUsesBlend1D = sample.usesBlend1D;
			runtime.currentBlendParameterName = sample.parameterName;
			runtime.currentBlendParameterValue = sample.parameterValue;
			runtime.currentBlendPrimaryClipName.clear();
			runtime.currentBlendSecondaryClipName.clear();
			if (const AnimationClip* primaryClip = ResolveClipByIndex(runtime.clips, sample.primaryClipIndex))
			{
				runtime.currentBlendPrimaryClipName = primaryClip->name;
			}
			if (const AnimationClip* secondaryClip = ResolveClipByIndex(runtime.clips, sample.secondaryClipIndex))
			{
				runtime.currentBlendSecondaryClipName = secondaryClip->name;
			}
			runtime.blendSecondaryClipIndex = sample.secondaryClipIndex;
			runtime.blendSecondaryAlpha = sample.secondaryAlpha;
		}

		inline void SyncActiveStateAnimators(
			AnimationControllerRuntime& runtime,
			AnimatorState& primaryAnimator,
			const StateSampleConfig& sample,
			bool resetTime)
		{
			const float normalizedTime = resetTime ? 0.0f : GetAnimatorNormalizedTime(primaryAnimator);
			const AnimationClip* primaryClip = ResolveClipByIndex(runtime.clips, sample.primaryClipIndex);
			SyncAnimatorClip(
				primaryAnimator,
				runtime.skeleton,
				primaryClip,
				sample.state != nullptr ? sample.state->looping : runtime.looping,
				sample.state != nullptr ? sample.state->playRate : runtime.playRate,
				runtime.paused,
				resetTime,
				normalizedTime,
				false);

			const bool useSecondary = sample.secondaryClipIndex >= 0 && sample.secondaryAlpha > 1e-6f;
			if (useSecondary)
			{
				const AnimationClip* secondaryClip = ResolveClipByIndex(runtime.clips, sample.secondaryClipIndex);
				SyncAnimatorClip(
					runtime.blendSecondaryAnimator,
					runtime.skeleton,
					secondaryClip,
					sample.state != nullptr ? sample.state->looping : runtime.looping,
					sample.state != nullptr ? sample.state->playRate : runtime.playRate,
					runtime.paused,
					resetTime,
					normalizedTime,
					true);
			}
			else
			{
				runtime.blendSecondaryAnimator = {};
			}
			SyncRuntimeBlendMetadata(runtime, sample);
		}

		inline void ClearActiveBlendMetadata(AnimationControllerRuntime& runtime)
		{
			runtime.currentStateUsesBlend1D = false;
			runtime.currentBlendParameterName.clear();
			runtime.currentBlendParameterValue = 0.0f;
			runtime.currentBlendPrimaryClipName.clear();
			runtime.currentBlendSecondaryClipName.clear();
			runtime.blendSecondaryAnimator = {};
			runtime.blendSecondaryClipIndex = -1;
			runtime.blendSecondaryAlpha = 0.0f;
		}

		inline void ApplyRuntimeState(AnimationControllerRuntime& runtime, int stateIndex, bool resetStateTracking = true)
		{
			if (runtime.stateMachineAsset == nullptr ||
				stateIndex < 0 ||
				static_cast<std::size_t>(stateIndex) >= runtime.stateMachineAsset->states.size())
			{
				runtime.currentStateIndex = -1;
				runtime.currentStateName.clear();
				runtime.legacyClipIndex = -1;
				ClearActiveBlendMetadata(runtime);
				if (resetStateTracking)
				{
					runtime.previousStateNormalizedTime = 0.0f;
					runtime.stateEnteredThisFrame = true;
				}
				return;
			}
			runtime.currentStateIndex = stateIndex;
			const AnimationStateDesc& state = runtime.stateMachineAsset->states[static_cast<std::size_t>(stateIndex)];
			runtime.currentStateName = state.name;
			runtime.looping = state.looping;
			runtime.playRate = state.playRate;
			runtime.legacyClipIndex =
				(static_cast<std::size_t>(stateIndex) < runtime.resolvedStateClipIndices.size())
				? runtime.resolvedStateClipIndices[static_cast<std::size_t>(stateIndex)]
				: -1;
			runtime.currentStateUsesBlend1D = !state.blendParameter.empty() && !state.blend1D.empty();
			runtime.currentBlendParameterName = state.blendParameter;
			runtime.currentBlendParameterValue = 0.0f;
			runtime.currentBlendPrimaryClipName.clear();
			runtime.currentBlendSecondaryClipName.clear();
			runtime.blendSecondaryClipIndex = -1;
			runtime.blendSecondaryAlpha = 0.0f;
			if (resetStateTracking)
			{
				runtime.previousStateNormalizedTime = 0.0f;
				runtime.stateEnteredThisFrame = true;
			}
		}

		[[nodiscard]] inline bool TransitionMatchesState(const AnimationTransitionDesc& transition, std::string_view currentState) noexcept
		{
			return transition.fromState.empty() || transition.fromState == "*" || transition.fromState == currentState;
		}

		inline void ResetBlendState(AnimationControllerRuntime& runtime)
		{
			runtime.transitionActive = false;
			runtime.transitionSourceStateIndex = -1;
			runtime.transitionSourceStateName.clear();
			runtime.transitionElapsedSeconds = 0.0f;
			runtime.transitionDurationSeconds = 0.0f;
			runtime.transitionSourceAnimator = {};
			runtime.transitionSourceBlendSecondaryAnimator = {};
			runtime.transitionSourceSecondaryClipIndex = -1;
			runtime.transitionSourceSecondaryAlpha = 0.0f;
		}
	}

	inline void ResetAnimationParameters(AnimationParameterStore& store)
	{
		store.values.clear();
	}

	[[nodiscard]] inline AnimationParameterValue* FindAnimationParameter(AnimationParameterStore& store, std::string_view name)
	{
		auto it = store.values.find(std::string(name));
		return (it == store.values.end()) ? nullptr : &it->second;
	}

	[[nodiscard]] inline const AnimationParameterValue* FindAnimationParameter(const AnimationParameterStore& store, std::string_view name)
	{
		auto it = store.values.find(std::string(name));
		return (it == store.values.end()) ? nullptr : &it->second;
	}

	[[nodiscard]] inline const AnimationParameterDesc* FindAnimationParameterDesc(const AnimationControllerAsset& asset, std::string_view name) noexcept
	{
		for (const AnimationParameterDesc& param : asset.parameters)
		{
			if (param.name == name)
			{
				return &param;
			}
		}
		return nullptr;
	}

	template <typename T>
	concept AnimationParameterTypeC =
		std::same_as<std::remove_cvref_t<T>, bool> ||
		std::same_as<std::remove_cvref_t<T>, int> ||
		std::same_as<std::remove_cvref_t<T>, float>;

	template<AnimationParameterTypeC T>
	inline void SetAnimationParameter(AnimationParameterStore& store, std::string_view name, T value)
	{
		using ValueT = std::remove_cvref_t<T>;

		AnimationParameterValue& param = store.values[std::string(name)];

		if constexpr (std::same_as<ValueT, bool>)
		{
			param.type = AnimationParameterType::Bool;
			param.boolValue = value;
		}
		else if constexpr (std::same_as<ValueT, int>)
		{
			param.type = AnimationParameterType::Int;
			param.intValue = value;
		}
		else if constexpr (std::same_as<ValueT, float>)
		{
			param.type = AnimationParameterType::Float;
			param.floatValue = value;
		}
	}

	inline void FireAnimationTrigger(AnimationParameterStore& store, std::string_view name)
	{
		AnimationParameterValue& param = store.values[std::string(name)];
		param.type = AnimationParameterType::Trigger;
		param.triggerValue = true;
	}

	inline void ResetAnimationTrigger(AnimationParameterStore& store, std::string_view name)
	{
		if (AnimationParameterValue* param = FindAnimationParameter(store, name))
		{
			if (param->type == AnimationParameterType::Trigger)
			{
				param->triggerValue = false;
			}
		}
	}

	[[nodiscard]] inline bool ConsumeAnimationTrigger(AnimationParameterStore& store, std::string_view name)
	{
		AnimationParameterValue* param = FindAnimationParameter(store, name);
		if (param == nullptr || param->type != AnimationParameterType::Trigger || !param->triggerValue)
		{
			return false;
		}
		param->triggerValue = false;
		return true;
	}

	[[nodiscard]] inline const std::vector<AnimationNotifyEvent>& PeekAnimationControllerNotifyEvents(const AnimationControllerRuntime& runtime) noexcept
	{
		return runtime.pendingNotifyEvents;
	}

	inline void ClearAnimationControllerNotifyEvents(AnimationControllerRuntime& runtime) noexcept
	{
		runtime.pendingNotifyEvents.clear();
	}

	[[nodiscard]] inline std::vector<AnimationNotifyEvent> ConsumeAnimationControllerNotifyEvents(AnimationControllerRuntime& runtime)
	{
		std::vector<AnimationNotifyEvent> out = std::move(runtime.pendingNotifyEvents);
		runtime.pendingNotifyEvents.clear();
		return out;
	}

	[[nodiscard]] inline bool IsAnimationControllerUsingLegacyClipMode(const AnimationControllerRuntime& runtime) noexcept
	{
		return runtime.mode == AnimationControllerMode::LegacyClip;
	}

	[[nodiscard]] inline const AnimationClip* ResolveLegacyAnimationClip(const AnimationControllerRuntime& runtime) noexcept
	{
		if (runtime.clips == nullptr ||
			runtime.legacyClipIndex < 0 ||
			static_cast<std::size_t>(runtime.legacyClipIndex) >= runtime.clips->size())
		{
			return nullptr;
		}

		return &(*runtime.clips)[static_cast<std::size_t>(runtime.legacyClipIndex)];
	}

	[[nodiscard]] inline int FindAnimationControllerStateIndex(const AnimationControllerAsset& asset, std::string_view stateName) noexcept
	{
		return detail::FindStateIndexByName(asset, stateName);
	}

	inline void ResetAnimationControllerRuntime(AnimationControllerRuntime& runtime)
	{
		runtime = {};
	}

	inline void SyncAnimationControllerLegacyClip(
		AnimationControllerRuntime& runtime,
		const Skeleton& skeleton,
		const std::vector<AnimationClip>& clips,
		int clipIndex,
		bool autoplay,
		bool looping,
		float playRate,
		bool paused,
		bool forceBindPose)
	{
		runtime.mode = AnimationControllerMode::LegacyClip;
		runtime.skeleton = &skeleton;
		runtime.clips = &clips;
		runtime.clipSourceAssetIds = nullptr;
		runtime.stateMachineAsset = nullptr;
		runtime.currentStateIndex = -1;
		runtime.resolvedStateClipIndices.clear();
		runtime.resolvedStateBlendClipIndices.clear();
		detail::ResetBlendState(runtime);
		detail::ClearActiveBlendMetadata(runtime);
		runtime.legacyClipIndex = clipIndex;
		runtime.autoplay = autoplay;
		runtime.looping = looping;
		runtime.playRate = playRate;
		runtime.paused = paused;
		runtime.forceBindPose = forceBindPose;
		runtime.currentStateName = (ResolveLegacyAnimationClip(runtime) != nullptr)
			? ResolveLegacyAnimationClip(runtime)->name
			: std::string("BindPose");
	}

	inline void BindAnimationControllerStateMachine(
		AnimationControllerRuntime& runtime,
		const Skeleton& skeleton,
		const std::vector<AnimationClip>& clips,
		const std::vector<std::string>& clipSourceAssetIds,
		const AnimationControllerAsset& asset,
		bool autoplay,
		bool paused,
		bool forceBindPose)
	{
		const bool sameAsset = runtime.mode == AnimationControllerMode::StateMachine && runtime.stateMachineAsset == &asset;
		runtime.mode = AnimationControllerMode::StateMachine;
		runtime.skeleton = &skeleton;
		runtime.clips = &clips;
		runtime.clipSourceAssetIds = &clipSourceAssetIds;
		runtime.stateMachineAsset = &asset;
		runtime.controllerAssetId = asset.id;
		runtime.autoplay = autoplay;
		runtime.paused = paused;
		runtime.forceBindPose = forceBindPose;
		detail::ResolveStateClipIndices(runtime);
		detail::ResetBlendState(runtime);
		detail::ClearActiveBlendMetadata(runtime);

		if (!sameAsset)
		{
			detail::ApplyParameterDefaults(runtime.parameters, asset);
			runtime.requestedStateName.clear();
			const int defaultIndex =
				!asset.defaultState.empty()
				? detail::FindStateIndexByName(asset, asset.defaultState)
				: (asset.states.empty() ? -1 : 0);
			detail::ApplyRuntimeState(runtime, defaultIndex, true);
		}
		else if (runtime.currentStateIndex >= 0)
		{
			detail::ApplyRuntimeState(runtime, runtime.currentStateIndex, false);
		}
	}

	inline void RefreshAnimationControllerRuntimeBindings(
		AnimationControllerRuntime& runtime,
		const Skeleton& skeleton,
		const std::vector<AnimationClip>& clips,
		const std::vector<std::string>& clipSourceAssetIds,
		bool autoplay,
		bool paused,
		bool forceBindPose)
	{
		runtime.skeleton = &skeleton;
		runtime.clips = &clips;
		runtime.clipSourceAssetIds = &clipSourceAssetIds;
		runtime.autoplay = autoplay;
		runtime.paused = paused;
		runtime.forceBindPose = forceBindPose;
		if (runtime.mode == AnimationControllerMode::StateMachine && runtime.stateMachineAsset != nullptr)
		{
			detail::ResolveStateClipIndices(runtime);
			if (runtime.currentStateIndex >= 0)
			{
				detail::ApplyRuntimeState(runtime, runtime.currentStateIndex, false);
			}
			else
			{
				detail::ClearActiveBlendMetadata(runtime);
			}
			if (runtime.transitionActive)
			{
				detail::ResetBlendState(runtime);
			}
		}
	}

	inline void RequestAnimationControllerState(AnimationControllerRuntime& runtime, std::string_view stateName)
	{
		runtime.requestedStateName = std::string(stateName);
	}

	inline void UpdateAnimationControllerRuntime(AnimationControllerRuntime& runtime, AnimatorState& animator, float deltaSeconds)
	{
		if (runtime.skeleton == nullptr)
		{
			return;
		}

		if (runtime.mode == AnimationControllerMode::StateMachine && runtime.stateMachineAsset != nullptr)
		{
			if (runtime.currentStateIndex < 0)
			{
				const int defaultIndex =
					!runtime.stateMachineAsset->defaultState.empty()
					? detail::FindStateIndexByName(*runtime.stateMachineAsset, runtime.stateMachineAsset->defaultState)
					: (runtime.stateMachineAsset->states.empty() ? -1 : 0);
				detail::ApplyRuntimeState(runtime, defaultIndex, true);
			}

			if (runtime.forceBindPose)
			{
				detail::ResetBlendState(runtime);
				detail::ClearActiveBlendMetadata(runtime);
				runtime.lastAppliedRootMotionDelta = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
				ResetAnimatorToBindPose(animator, *runtime.skeleton);
				BuildAnimatorMatrices(animator);
				return;
			}

			const detail::StateSampleConfig initialSample = detail::BuildStateSampleConfig(runtime, runtime.currentStateIndex);
			detail::SyncActiveStateAnimators(runtime, animator, initialSample, false);

			if (runtime.autoplay && !runtime.paused)
			{
				AdvanceAnimator(animator, deltaSeconds);
				if (runtime.blendSecondaryClipIndex >= 0 && IsAnimatorReady(runtime.blendSecondaryAnimator))
				{
					AdvanceAnimator(runtime.blendSecondaryAnimator, deltaSeconds);
				}
				if (runtime.transitionActive)
				{
					runtime.transitionElapsedSeconds += deltaSeconds;
					AdvanceAnimator(runtime.transitionSourceAnimator, deltaSeconds);
					if (runtime.transitionSourceSecondaryClipIndex >= 0 && IsAnimatorReady(runtime.transitionSourceBlendSecondaryAnimator))
					{
						AdvanceAnimator(runtime.transitionSourceBlendSecondaryAnimator, deltaSeconds);
					}
				}
			}

			int targetStateIndex = -1;
			const AnimationTransitionDesc* matchedTransition = nullptr;

			if (!runtime.requestedStateName.empty())
			{
				targetStateIndex = detail::FindStateIndexByName(*runtime.stateMachineAsset, runtime.requestedStateName);
				runtime.requestedStateName.clear();
			}
			else if (!runtime.transitionActive)
			{
				for (const AnimationTransitionDesc& transition : runtime.stateMachineAsset->transitions)
				{
					if (!detail::TransitionMatchesState(transition, runtime.currentStateName))
					{
						continue;
					}
					if (transition.hasExitTime &&
						detail::GetAnimatorNormalizedTime(animator) < transition.exitTimeNormalized)
					{
						continue;
					}
					bool passed = true;
					for (const AnimationConditionDesc& condition : transition.conditions)
					{
						if (!detail::EvaluateCondition(condition, runtime.parameters))
						{
							passed = false;
							break;
						}
					}
					if (!passed)
					{
						continue;
					}
					targetStateIndex = detail::FindStateIndexByName(*runtime.stateMachineAsset, transition.toState);
					matchedTransition = &transition;
					break;
				}
			}

			if (targetStateIndex >= 0 && targetStateIndex != runtime.currentStateIndex)
			{
				const float blendDurationSeconds =
					(matchedTransition != nullptr)
					? std::max(0.0f, matchedTransition->blendDurationSeconds)
					: 0.0f;
				const bool canBlend =
					blendDurationSeconds > 1e-4f &&
					IsAnimatorReady(animator) &&
					animator.skeleton == runtime.skeleton &&
					animator.clip != nullptr;

				if (canBlend)
				{
					runtime.transitionSourceAnimator = animator;
					runtime.transitionSourceAnimator.paused = runtime.paused;
					runtime.transitionSourceBlendSecondaryAnimator = runtime.blendSecondaryAnimator;
					runtime.transitionSourceSecondaryClipIndex = runtime.blendSecondaryClipIndex;
					runtime.transitionSourceSecondaryAlpha = runtime.blendSecondaryAlpha;
					runtime.transitionSourceStateIndex = runtime.currentStateIndex;
					runtime.transitionSourceStateName = runtime.currentStateName;
					runtime.transitionElapsedSeconds = 0.0f;
					runtime.transitionDurationSeconds = blendDurationSeconds;
					runtime.transitionActive = true;
				}
				else
				{
					detail::ResetBlendState(runtime);
				}

				detail::ApplyRuntimeState(runtime, targetStateIndex, true);
				const detail::StateSampleConfig targetSample = detail::BuildStateSampleConfig(runtime, targetStateIndex);
				detail::SyncActiveStateAnimators(runtime, animator, targetSample, true);
				if (matchedTransition != nullptr)
				{
					detail::ConsumeTransitionTriggers(runtime.parameters, *matchedTransition);
				}
			}
			else
			{
				const detail::StateSampleConfig liveSample = detail::BuildStateSampleConfig(runtime, runtime.currentStateIndex);
				detail::SyncActiveStateAnimators(runtime, animator, liveSample, false);
			}

			animator.paused = runtime.paused;
			detail::EvaluateAnimatorPairToLocalPose(
				animator,
				(runtime.blendSecondaryClipIndex >= 0) ? &runtime.blendSecondaryAnimator : nullptr,
				runtime.blendSecondaryAlpha);

			if (runtime.transitionActive)
			{
				const bool validBlend =
					IsAnimatorReady(runtime.transitionSourceAnimator) &&
					runtime.transitionSourceAnimator.skeleton == runtime.skeleton &&
					runtime.transitionDurationSeconds > 1e-4f;
				if (validBlend)
				{
					runtime.transitionSourceAnimator.paused = runtime.paused;
					detail::EvaluateAnimatorPairToLocalPose(
						runtime.transitionSourceAnimator,
						(runtime.transitionSourceSecondaryClipIndex >= 0) ? &runtime.transitionSourceBlendSecondaryAnimator : nullptr,
						runtime.transitionSourceSecondaryAlpha);

					const float alpha = std::clamp(
						runtime.transitionElapsedSeconds / runtime.transitionDurationSeconds,
						0.0f,
						1.0f);
					const std::vector<LocalBoneTransform> targetPose = animator.localPose;
					BlendLocalPoses(animator.localPose, runtime.transitionSourceAnimator.localPose, targetPose, alpha);
					if (alpha >= 1.0f - 1e-6f)
					{
						detail::ResetBlendState(runtime);
					}
				}
				else
				{
					detail::ResetBlendState(runtime);
				}
			}

			detail::ApplyRootMotionModeToAnimatorPose(runtime, animator);
			detail::QueueCurrentStateNotifies(runtime, animator);

			BuildAnimatorMatrices(animator);
			return;
		}

		const AnimationClip* clip = ResolveLegacyAnimationClip(runtime);
		const bool needsInit = !IsAnimatorReady(animator) || animator.skeleton != runtime.skeleton;
		if (needsInit)
		{
			InitializeAnimator(animator, runtime.skeleton, clip);
		}
		else if (animator.clip != clip)
		{
			SetAnimatorClip(animator, clip, runtime.looping, runtime.playRate, true);
		}

		animator.looping = runtime.looping;
		animator.playRate = runtime.playRate;
		animator.paused = runtime.paused;

		if (runtime.forceBindPose)
		{
			runtime.lastAppliedRootMotionDelta = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			ResetAnimatorToBindPose(animator, *runtime.skeleton);
			BuildAnimatorMatrices(animator);
			return;
		}

		if (runtime.autoplay && !animator.paused)
		{
			AdvanceAnimator(animator, deltaSeconds);
		}
		EvaluateAnimatorLocalPose(animator);
		detail::ApplyRootMotionModeToAnimatorPose(runtime, animator);
		BuildAnimatorMatrices(animator);
	}
}