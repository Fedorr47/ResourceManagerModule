module;

#include <algorithm>
#include <concepts>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

	struct AnimationStateDesc
	{
		std::string name;
		std::string clipName;
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

	enum class AnimationControllerMode : std::uint8_t
	{
		LegacyClip = 0,
		StateMachine = 1
	};

	struct AnimationControllerRuntime
	{
		AnimationControllerMode mode{ AnimationControllerMode::LegacyClip };
		const Skeleton* skeleton{ nullptr };
		const std::vector<AnimationClip>* clips{ nullptr };

		std::string controllerAssetId;
		std::string currentStateName;
		std::string requestedStateName;
		const AnimationControllerAsset* stateMachineAsset{ nullptr };
		int currentStateIndex{ -1 };
		std::vector<int> resolvedStateClipIndices;

		int legacyClipIndex{ -1 };
		bool autoplay{ true };
		bool looping{ true };
		float playRate{ 1.0f };
		bool paused{ false };
		bool forceBindPose{ false };

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

		inline void ResolveStateClipIndices(AnimationControllerRuntime& runtime)
		{
			runtime.resolvedStateClipIndices.clear();
			if (runtime.stateMachineAsset == nullptr || runtime.clips == nullptr)
			{
				return;
			}
			runtime.resolvedStateClipIndices.resize(runtime.stateMachineAsset->states.size(), -1);
			for (std::size_t i = 0; i < runtime.stateMachineAsset->states.size(); ++i)
			{
				runtime.resolvedStateClipIndices[i] =
					ResolveClipIndexByName(*runtime.clips, runtime.stateMachineAsset->states[i].clipName);
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

		inline void ApplyRuntimeState(AnimationControllerRuntime& runtime, int stateIndex)
		{
			if (runtime.stateMachineAsset == nullptr ||
				stateIndex < 0 ||
				static_cast<std::size_t>(stateIndex) >= runtime.stateMachineAsset->states.size())
			{
				runtime.currentStateIndex = -1;
				runtime.currentStateName.clear();
				runtime.legacyClipIndex = -1;
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
		}

		inline void EnsureAnimatorClipMatchesRuntime(AnimationControllerRuntime& runtime, AnimatorState& animator, bool resetTime)
		{
			const AnimationClip* clip = ResolveLegacyAnimationClip(runtime);
			const bool needsInit = !IsAnimatorReady(animator) || animator.skeleton != runtime.skeleton;
			if (needsInit)
			{
				InitializeAnimator(animator, runtime.skeleton, clip);
			}
			else if (animator.clip != clip)
			{
				SetAnimatorClip(animator, clip, runtime.looping, runtime.playRate, resetTime);
			}
			else
			{
				animator.looping = runtime.looping;
				animator.playRate = runtime.playRate;
			}
			animator.paused = runtime.paused;
		}

		[[nodiscard]] inline bool TransitionMatchesState(const AnimationTransitionDesc& transition, std::string_view currentState) noexcept
		{
			return transition.fromState.empty() || transition.fromState == "*" || transition.fromState == currentState;
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
		runtime.stateMachineAsset = nullptr;
		runtime.currentStateIndex = -1;
		runtime.resolvedStateClipIndices.clear();
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
		const AnimationControllerAsset& asset,
		bool autoplay,
		bool paused,
		bool forceBindPose)
	{
		const bool sameAsset = runtime.mode == AnimationControllerMode::StateMachine && runtime.stateMachineAsset == &asset;
		runtime.mode = AnimationControllerMode::StateMachine;
		runtime.skeleton = &skeleton;
		runtime.clips = &clips;
		runtime.stateMachineAsset = &asset;
		runtime.controllerAssetId = asset.id;
		runtime.autoplay = autoplay;
		runtime.paused = paused;
		runtime.forceBindPose = forceBindPose;
		detail::ResolveStateClipIndices(runtime);

		if (!sameAsset)
		{
			detail::ApplyParameterDefaults(runtime.parameters, asset);
			runtime.requestedStateName.clear();
			const int defaultIndex =
				!asset.defaultState.empty()
				? detail::FindStateIndexByName(asset, asset.defaultState)
				: (asset.states.empty() ? -1 : 0);
			detail::ApplyRuntimeState(runtime, defaultIndex);
		}
		else if (runtime.currentStateIndex >= 0)
		{
			detail::ApplyRuntimeState(runtime, runtime.currentStateIndex);
		}
	}

	inline void RefreshAnimationControllerRuntimeBindings(
		AnimationControllerRuntime& runtime,
		const Skeleton& skeleton,
		const std::vector<AnimationClip>& clips,
		bool autoplay,
		bool paused,
		bool forceBindPose)
	{
		runtime.skeleton = &skeleton;
		runtime.clips = &clips;
		runtime.autoplay = autoplay;
		runtime.paused = paused;
		runtime.forceBindPose = forceBindPose;
		if (runtime.mode == AnimationControllerMode::StateMachine && runtime.stateMachineAsset != nullptr)
		{
			detail::ResolveStateClipIndices(runtime);
			if (runtime.currentStateIndex >= 0)
			{
				detail::ApplyRuntimeState(runtime, runtime.currentStateIndex);
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
				detail::ApplyRuntimeState(runtime, defaultIndex);
			}
			detail::EnsureAnimatorClipMatchesRuntime(runtime, animator, false);

			if (runtime.forceBindPose)
			{
				ResetAnimatorToBindPose(animator, *runtime.skeleton);
				EvaluateAnimator(animator);
				return;
			}

			if (runtime.autoplay && !runtime.paused)
			{
				AdvanceAnimator(animator, deltaSeconds);
			}

			int targetStateIndex = -1;
			const AnimationTransitionDesc* matchedTransition = nullptr;

			if (!runtime.requestedStateName.empty())
			{
				targetStateIndex = detail::FindStateIndexByName(*runtime.stateMachineAsset, runtime.requestedStateName);
				runtime.requestedStateName.clear();
			}
			else
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
				detail::ApplyRuntimeState(runtime, targetStateIndex);
				detail::EnsureAnimatorClipMatchesRuntime(runtime, animator, true);
				if (matchedTransition != nullptr)
				{
					detail::ConsumeTransitionTriggers(runtime.parameters, *matchedTransition);
				}
			}

			animator.paused = runtime.paused;
			EvaluateAnimator(animator);
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
			ResetAnimatorToBindPose(animator, *runtime.skeleton);
			EvaluateAnimator(animator);
			return;
		}

		if (runtime.autoplay && !animator.paused)
		{
			UpdateAnimator(animator, deltaSeconds);
		}
		else
		{
			EvaluateAnimator(animator);
		}
	}
}