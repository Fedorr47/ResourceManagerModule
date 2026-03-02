module;

export module core:hash_utils;

import std;
import :mesh;
import :rhi;
import :math_utils;
#if defined(CORE_USE_DX12)
import :common_DX12_Structs;
#endif

export namespace hashUtils
{
	void HashCombine(std::size_t& seed, std::size_t value) noexcept
	{
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
	}

#if defined(CORE_USE_DX12)
	struct BatchKeyHash
	{
		static std::size_t HashU32(std::uint32_t value) noexcept { return std::hash<std::uint32_t>{}(value); }
		static std::size_t HashPtr(const void* ptr) noexcept { return std::hash<const void*>{}(ptr); }

		static std::uint32_t FloatBits(float value) noexcept
		{
			std::uint32_t bits{};
			std::memcpy(&bits, &value, sizeof(bits));
			return bits;
		}

		std::size_t operator()(const rendern::BatchKey& key) const noexcept
		{
			std::size_t seed = HashPtr(key.mesh);

			HashCombine(seed, HashU32(key.permBits));

			HashCombine(seed, HashU32(key.envSource));
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.reflectionProbeIndex)));

			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.albedoDescIndex)));
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.normalDescIndex)));
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.metalnessDescIndex)));
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.roughnessDescIndex)));
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.aoDescIndex)));
			HashCombine(seed, HashU32(static_cast<std::uint32_t>(key.emissiveDescIndex)));

			HashCombine(seed, HashU32(FloatBits(key.baseColor.x)));
			HashCombine(seed, HashU32(FloatBits(key.baseColor.y)));
			HashCombine(seed, HashU32(FloatBits(key.baseColor.z)));
			HashCombine(seed, HashU32(FloatBits(key.baseColor.w)));

			HashCombine(seed, HashU32(FloatBits(key.shadowBias)));

			HashCombine(seed, HashU32(FloatBits(key.metallic)));
			HashCombine(seed, HashU32(FloatBits(key.roughness)));
			HashCombine(seed, HashU32(FloatBits(key.ao)));
			HashCombine(seed, HashU32(FloatBits(key.emissiveStrength)));

			// Legacy
			HashCombine(seed, HashU32(FloatBits(key.shininess)));
			HashCombine(seed, HashU32(FloatBits(key.specStrength)));
			return seed;
		}
	};
#endif
}