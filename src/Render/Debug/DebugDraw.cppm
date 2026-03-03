module;

#include <cstdint>

export module core:debug_draw;

import std;

import :math_utils;

export namespace rendern::debugDraw
{
	struct DebugVertex
	{
		mathUtils::Vec3 pos{};
		std::uint32_t rgba{ 0xffffffffu }; // 0xAABBGGRR in memory (little-endian -> RR GG BB AA)
	};

	// Packs 0..255 components into a uint32 that matches R8G8B8A8_UNORM.
	// Memory layout on little-endian is: RR GG BB AA.
	constexpr std::uint32_t PackRGBA8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) noexcept
	{
		return static_cast<std::uint32_t>(r)
			| (static_cast<std::uint32_t>(g) << 8)
			| (static_cast<std::uint32_t>(b) << 16)
			| (static_cast<std::uint32_t>(a) << 24);
	}

	struct DebugDrawList
	{
		std::vector<DebugVertex> lineVertices;
		std::vector<DebugVertex> overlayLineVertices;
		std::vector<DebugVertex> screenOverlayLineVertices;

		void Clear()
		{
			lineVertices.clear();
			overlayLineVertices.clear();
			screenOverlayLineVertices.clear();
		}

		void ReserveLines(std::size_t lineCount)
		{
			lineVertices.reserve(lineCount * 2);
		}

		void ReserveOverlayLines(std::size_t lineCount)
		{
			overlayLineVertices.reserve(lineCount * 2);
		}

		void ReserveScreenOverlayLines(std::size_t lineCount)
		{
			screenOverlayLineVertices.reserve(lineCount * 2);
		}

		std::size_t VertexCount() const noexcept
		{
			return lineVertices.size() + overlayLineVertices.size() + screenOverlayLineVertices.size();
		}

		std::size_t DepthVertexCount() const noexcept
		{
			return lineVertices.size();
		}

		std::size_t OverlayVertexCount() const noexcept
		{
			return overlayLineVertices.size();
		}

		std::size_t ScreenOverlayVertexCount() const noexcept
		{
			return screenOverlayLineVertices.size();
		}

		void AddLine(const mathUtils::Vec3& a, const mathUtils::Vec3& b, std::uint32_t rgba, bool overlay = false)
		{
			auto& dst = overlay ? overlayLineVertices : lineVertices;
			dst.push_back(DebugVertex{ a, rgba });
			dst.push_back(DebugVertex{ b, rgba });
		}

		void AddScreenSpaceLineNdc(const mathUtils::Vec3& a, const mathUtils::Vec3& b, std::uint32_t rgba)
		{
			screenOverlayLineVertices.push_back(DebugVertex{ a, rgba });
			screenOverlayLineVertices.push_back(DebugVertex{ b, rgba });
		}

		void AddScreenSpaceRectNdc(float x0, float y0, float x1, float y1, std::uint32_t rgba)
		{
			const mathUtils::Vec3 p00(x0, y0, 0.0f);
			const mathUtils::Vec3 p10(x1, y0, 0.0f);
			const mathUtils::Vec3 p11(x1, y1, 0.0f);
			const mathUtils::Vec3 p01(x0, y1, 0.0f);
			AddScreenSpaceLineNdc(p00, p10, rgba);
			AddScreenSpaceLineNdc(p10, p11, rgba);
			AddScreenSpaceLineNdc(p11, p01, rgba);
			AddScreenSpaceLineNdc(p01, p00, rgba);
		}

		void AddScreenSpaceFilledRectNdc(float x0, float y0, float x1, float y1, std::uint32_t rgba, std::uint32_t columns = 64)
		{
			if (x1 < x0)
			{
				std::swap(x0, x1);
			}
			if (y1 < y0)
			{
				std::swap(y0, y1);
			}
			if (columns == 0u || x1 <= x0 || y1 <= y0)
			{
				return;
			}

			if (columns == 1u)
			{
				const float xc = (x0 + x1) * 0.5f;
				AddScreenSpaceLineNdc(mathUtils::Vec3(xc, y0, 0.0f), mathUtils::Vec3(xc, y1, 0.0f), rgba);
				return;
			}

			for (std::uint32_t i = 0; i < columns; ++i)
			{
				const float t = static_cast<float>(i) / static_cast<float>(columns - 1u);
				const float x = std::lerp(x0, x1, t);
				AddScreenSpaceLineNdc(mathUtils::Vec3(x, y0, 0.0f), mathUtils::Vec3(x, y1, 0.0f), rgba);
			}
		}

		// Pixel-aligned variant for NDC space [-1, 1].
		// Uses the provided pxToNdcX (2 / viewportWidth) to place vertical lines at exact pixel centers.
		// This avoids visible gaps/"black stripes" that can appear when using the lerp-based column placement.
		void AddScreenSpaceFilledRectNdcPixelAligned(
			float x0, float y0, float x1, float y1,
			std::uint32_t rgba,
			float pxToNdcX)
		{
			if (x1 < x0)
			{
				std::swap(x0, x1);
			}
			if (y1 < y0)
			{
				std::swap(y0, y1);
			}
			if (x1 <= x0 || y1 <= y0)
			{
				return;
			}
			if (pxToNdcX <= 1e-7f)
			{
				// Fallback: draw a single column.
				const float xc = (x0 + x1) * 0.5f;
				AddScreenSpaceLineNdc(mathUtils::Vec3(xc, y0, 0.0f), mathUtils::Vec3(xc, y1, 0.0f), rgba);
				return;
			}

			// Convert x range to pixel-center indices.
			// Pixel center i maps to: x = -1 + (i + 0.5) * pxToNdcX.
			const float inv = 1.0f / pxToNdcX;
			const int i0 = static_cast<int>(std::ceil(((x0 + 1.0f) * inv) - 0.5f));
			const int i1 = static_cast<int>(std::floor(((x1 + 1.0f) * inv) - 0.5f));
			if (i1 < i0)
			{
				// Too thin to span a full pixel column.
				const float xc = (x0 + x1) * 0.5f;
				AddScreenSpaceLineNdc(mathUtils::Vec3(xc, y0, 0.0f), mathUtils::Vec3(xc, y1, 0.0f), rgba);
				return;
			}

			for (int i = i0; i <= i1; ++i)
			{
				const float x = -1.0f + (static_cast<float>(i) + 0.5f) * pxToNdcX;
				AddScreenSpaceLineNdc(mathUtils::Vec3(x, y0, 0.0f), mathUtils::Vec3(x, y1, 0.0f), rgba);
			}
		}

		void AddAxesCross(const mathUtils::Vec3& origin, float halfSize, std::uint32_t rgba, bool overlay = false)
		{
			AddLine(origin - mathUtils::Vec3(halfSize, 0.0f, 0.0f), origin + mathUtils::Vec3(halfSize, 0.0f, 0.0f), rgba, overlay);
			AddLine(origin - mathUtils::Vec3(0.0f, halfSize, 0.0f), origin + mathUtils::Vec3(0.0f, halfSize, 0.0f), rgba, overlay);
			AddLine(origin - mathUtils::Vec3(0.0f, 0.0f, halfSize), origin + mathUtils::Vec3(0.0f, 0.0f, halfSize), rgba, overlay);
		}

		void AddArrow(const mathUtils::Vec3& start,
			const mathUtils::Vec3& end,
			std::uint32_t rgba,
			float headFrac = 0.25f,
			float headWidthFrac = 0.15f,
			bool overlay = false)
		{
			AddLine(start, end, rgba, overlay);

			const mathUtils::Vec3 dir = end - start;
			const float len = mathUtils::Length(dir);
			if (len <= 1e-5f)
			{
				return;
			}

			const mathUtils::Vec3 fwd = dir / len;
			mathUtils::Vec3 up = mathUtils::Vec3(0.0f, 1.0f, 0.0f);
			if (std::abs(mathUtils::Dot(fwd, up)) > 0.95f)
			{
				up = mathUtils::Vec3(1.0f, 0.0f, 0.0f);
			}
			const mathUtils::Vec3 right = mathUtils::Normalize(mathUtils::Cross(fwd, up));
			const mathUtils::Vec3 up2 = mathUtils::Normalize(mathUtils::Cross(right, fwd));

			const float headLen = len * std::clamp(headFrac, 0.05f, 0.45f);
			const float headW = len * std::clamp(headWidthFrac, 0.02f, 0.35f);
			const mathUtils::Vec3 base = end - fwd * headLen;

			AddLine(end, base + right * headW, rgba, overlay);
			AddLine(end, base - right * headW, rgba, overlay);
			AddLine(end, base + up2 * headW, rgba, overlay);
			AddLine(end, base - up2 * headW, rgba, overlay);
		}

		void AddWireCone(const mathUtils::Vec3& apex,
			const mathUtils::Vec3& direction,
			float length,
			float outerHalfAngleRad,
			std::uint32_t rgba,
			std::uint32_t segments = 24,
			bool overlay = false)
		{
			if (length <= 1e-5f || segments < 3)
			{
				return;
			}

			const mathUtils::Vec3 dir = mathUtils::Normalize(direction);

			mathUtils::Vec3 up = mathUtils::Vec3(0.0f, 1.0f, 0.0f);
			if (std::abs(mathUtils::Dot(dir, up)) > 0.95f)
			{
				up = mathUtils::Vec3(1.0f, 0.0f, 0.0f);
			}
			const mathUtils::Vec3 right = mathUtils::Normalize(mathUtils::Cross(dir, up));
			const mathUtils::Vec3 up2 = mathUtils::Normalize(mathUtils::Cross(right, dir));

			const float radius = std::tan(outerHalfAngleRad) * length;
			const mathUtils::Vec3 baseCenter = apex + dir * length;

			std::vector<mathUtils::Vec3> circle;
			circle.reserve(segments);
			for (std::uint32_t i = 0; i < segments; ++i)
			{
				const float t = (static_cast<float>(i) / static_cast<float>(segments)) * (mathUtils::Pi * 2.0f);
				circle.push_back(baseCenter + right * (std::cos(t) * radius) + up2 * (std::sin(t) * radius));
			}

			for (std::uint32_t i = 0; i < segments; ++i)
			{
				const std::uint32_t j = (i + 1) % segments;
				AddLine(circle[i], circle[j], rgba, overlay);
				AddLine(apex, circle[i], rgba, overlay);
			}
		}

		void AddWireCircle(const mathUtils::Vec3& center,
			const mathUtils::Vec3& axisA,
			const mathUtils::Vec3& axisB,
			float radius,
			std::uint32_t rgba,
			std::uint32_t segments = 48,
			bool overlay = false)
		{
			if (radius <= 1e-5f || segments < 6)
			{
				return;
			}

			const mathUtils::Vec3 a = mathUtils::Normalize(axisA);
			const mathUtils::Vec3 b = mathUtils::Normalize(axisB);
			mathUtils::Vec3 prev{};
			for (std::uint32_t i = 0; i <= segments; ++i)
			{
				const float t = (static_cast<float>(i) / static_cast<float>(segments)) * (mathUtils::Pi * 2.0f);
				const mathUtils::Vec3 p = center + a * (std::cos(t) * radius) + b * (std::sin(t) * radius);
				if (i != 0)
				{
					AddLine(prev, p, rgba, overlay);
				}
				prev = p;
			}
		}

		void AddCircle3D(
			const mathUtils::Vec3& center,
			const mathUtils::Vec3& axisA,
			const mathUtils::Vec3& axisB,
			float radius,
			std::uint32_t rgba,
			std::uint32_t segments,
			bool overlay = false)
		{
			mathUtils::Vec3 prev{};
			for (std::uint32_t i = 0; i <= segments; ++i)
			{
				const float t = (static_cast<float>(i) / static_cast<float>(segments)) * (mathUtils::Pi * 2.0f);
				const mathUtils::Vec3 p = center + axisA * (std::cos(t) * radius) + axisB * (std::sin(t) * radius);
				if (i != 0)
				{
					AddLine(prev, p, rgba, overlay);
				}
				prev = p;
			}
		}

		void AddWireSphere(const mathUtils::Vec3& center,
			float radius,
			std::uint32_t rgba,
			std::uint32_t segments = 24,
			bool overlay = false)
		{
			if (radius <= 1e-5f || segments < 6)
			{
				return;
			}

			AddCircle3D(center, mathUtils::Vec3(1.0f, 0.0f, 0.0f), mathUtils::Vec3(0.0f, 1.0f, 0.0f), radius, rgba, segments, overlay);
			AddCircle3D(center, mathUtils::Vec3(1.0f, 0.0f, 0.0f), mathUtils::Vec3(0.0f, 0.0f, 1.0f), radius, rgba, segments, overlay);
			AddCircle3D(center, mathUtils::Vec3(0.0f, 1.0f, 0.0f), mathUtils::Vec3(0.0f, 0.0f, 1.0f), radius, rgba, segments, overlay);
		}
	};
}