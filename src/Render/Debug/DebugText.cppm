module;

#include <cstdint>

export module core:debug_text;

import std;

export namespace rendern::debugText
{
	// Packs 0..255 components into a uint32 that matches R8G8B8A8_UNORM.
	// Memory layout on little-endian is: RR GG BB AA.
	constexpr std::uint32_t PackRGBA8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) noexcept
	{
		return static_cast<std::uint32_t>(r)
			| (static_cast<std::uint32_t>(g) << 8)
			| (static_cast<std::uint32_t>(b) << 16)
			| (static_cast<std::uint32_t>(a) << 24);
	}

	struct DebugTextItem
	{
		float xPx{ 0.0f };
		float yPx{ 0.0f };
		float scale{ 1.0f };          // glyph cell size in pixels
		std::uint32_t rgba{ 0xffffffffu };
		std::string text{};
	};

	struct DebugTextList
	{
		std::vector<DebugTextItem> items;

		void Clear()
		{
			items.clear();
		}

		void Reserve(std::size_t itemCount)
		{
			items.reserve(itemCount);
		}

		std::size_t ItemCount() const noexcept
		{
			return items.size();
		}

		bool Empty() const noexcept
		{
			return items.empty();
		}

		void AddTextPx(float xPx, float yPx, std::string_view text, std::uint32_t rgba = 0xffffffffu, float scale = 1.0f)
		{
			if (text.empty())
			{
				return;
			}

			DebugTextItem it{};
			it.xPx = xPx;
			it.yPx = yPx;
			it.scale = std::max(0.25f, scale);
			it.rgba = rgba;
			it.text = std::string(text);
			items.push_back(std::move(it));
		}
	};
}