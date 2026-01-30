module;

#include <glm/glm.hpp>
#include <cmath>

export module core:math_utils;

export namespace mathUtils
{
    using Vec3 = glm::vec3;

    [[nodiscard]] inline Vec3 Normalize(Vec3 v) noexcept
    {
        const float len2 = glm::dot(v, v);
        if (len2 > 0.0f)
        {
            const float invLen = 1.0f / std::sqrt(len2);
            return v * invLen;
        }
        return v;
    }

    [[nodiscard]] inline Vec3 Sub(const Vec3& a, const Vec3& b) noexcept
    {
        return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
    }

    [[nodiscard]] inline float Dot(const Vec3& a, const Vec3& b) noexcept
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
}
