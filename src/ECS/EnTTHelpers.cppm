module;

#include <entt/entt.hpp>

export module core:EnTTHelpers;

export namespace EnTT_helpers
{
	using EntityHandle = std::uint32_t;

	inline constexpr EntityHandle kNullEntity = (std::numeric_limits<EntityHandle>::max)();

	[[nodiscard]] inline entt::entity ToEnTT(const EntityHandle entity) noexcept
	{
		return static_cast<entt::entity>(entity);
	}
	[[nodiscard]] inline EntityHandle FromEnTT(const entt::entity entity) noexcept
	{
		return static_cast<EntityHandle>(entt::to_integral(entity));
	}
}