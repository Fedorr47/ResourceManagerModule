module;

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

export module core:picking;

import :scene;
import :level;
import :math_utils;

namespace
{
    struct Ray
    {
        mathUtils::Vec3 origin{ 0.0f, 0.0f, 0.0f };
        mathUtils::Vec3 dir{ 0.0f, 0.0f, 1.0f }; // normalized
    };

    static mathUtils::Vec3 MinVec3(const mathUtils::Vec3& a, const mathUtils::Vec3& b) noexcept
    {
        return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
    }

    static mathUtils::Vec3 MaxVec3(const mathUtils::Vec3& a, const mathUtils::Vec3& b) noexcept
    {
        return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
    }

    static mathUtils::Vec3 TransformPoint(const mathUtils::Mat4& m, const mathUtils::Vec3& p) noexcept
    {
        const mathUtils::Vec4 w = m * mathUtils::Vec4(p, 1.0f);
        return { w.x, w.y, w.z };
    }

    static void TransformAABB(const mathUtils::Vec3& bmin, const mathUtils::Vec3& bmax, const mathUtils::Mat4& m,
        mathUtils::Vec3& outMin, mathUtils::Vec3& outMax) noexcept
    {
        const mathUtils::Vec3 c[8] =
        {
            { bmin.x, bmin.y, bmin.z }, { bmax.x, bmin.y, bmin.z }, { bmin.x, bmax.y, bmin.z }, { bmax.x, bmax.y, bmin.z },
            { bmin.x, bmin.y, bmax.z }, { bmax.x, bmin.y, bmax.z }, { bmin.x, bmax.y, bmax.z }, { bmax.x, bmax.y, bmax.z },
        };

        mathUtils::Vec3 wmin{ std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity() };
        mathUtils::Vec3 wmax{ -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };

        for (const auto& p : c)
        {
            const mathUtils::Vec3 wp = TransformPoint(m, p);
            wmin = MinVec3(wmin, wp);
            wmax = MaxVec3(wmax, wp);
        }

        outMin = wmin;
        outMax = wmax;
    }

    static bool IntersectRayAABB(const Ray& ray, const mathUtils::Vec3& bmin, const mathUtils::Vec3& bmax, float& outT) noexcept
    {
        float tmin = 0.0f;
        float tmax = std::numeric_limits<float>::infinity();

        const float o[3] = { ray.origin.x, ray.origin.y, ray.origin.z };
        const float d[3] = { ray.dir.x, ray.dir.y, ray.dir.z };
        const float mn[3] = { bmin.x, bmin.y, bmin.z };
        const float mx[3] = { bmax.x, bmax.y, bmax.z };

        for (int axis = 0; axis < 3; ++axis)
        {
            const float dir = d[axis];
            const float ori = o[axis];

            if (std::abs(dir) < 1e-8f)
            {
                if (ori < mn[axis] || ori > mx[axis])
                {
                    return false;
                }
                continue;
            }

            const float invD = 1.0f / dir;
            float t1 = (mn[axis] - ori) * invD;
            float t2 = (mx[axis] - ori) * invD;
            if (t1 > t2)
            {
                std::swap(t1, t2);
            }

            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax)
            {
                return false;
            }
        }

        outT = tmin;
        return true;
    }

    static Ray BuildMouseRay(const rendern::Scene& scene, float mouseX, float mouseY, float viewportW, float viewportH) noexcept
    {
        const float width = (viewportW > 1.0f) ? viewportW : 1.0f;
        const float height = (viewportH > 1.0f) ? viewportH : 1.0f;

        // NDC in [-1..1], with +Y up.
        const float ndcX = (mouseX / width) * 2.0f - 1.0f;
        const float ndcY = 1.0f - (mouseY / height) * 2.0f;

        const float aspect = width / height;
        const float tanHalfFov = std::tan(mathUtils::DegToRad(scene.camera.fovYDeg) * 0.5f);

        const mathUtils::Vec3 forward = mathUtils::Normalize(scene.camera.target - scene.camera.position);
        const mathUtils::Vec3 right = mathUtils::Normalize(mathUtils::Cross(forward, scene.camera.up));
        const mathUtils::Vec3 up = mathUtils::Normalize(mathUtils::Cross(right, forward));

        mathUtils::Vec3 dir = forward;
        dir = dir + right * (ndcX * aspect * tanHalfFov);
        dir = dir + up * (ndcY * tanHalfFov);
        dir = mathUtils::Normalize(dir);

        Ray ray;
        ray.origin = scene.camera.position;
        ray.dir = dir;
        return ray;
    }
}

export namespace rendern
{
    struct PickResult
    {
        int nodeIndex{ -1 };
        float t{ std::numeric_limits<float>::infinity() };
        mathUtils::Vec3 rayOrigin{ 0.0f, 0.0f, 0.0f };
        mathUtils::Vec3 rayDir{ 0.0f, 0.0f, 1.0f }; // normalized
    };

    PickResult PickNodeUnderScreenPoint(
        const rendern::Scene& scene,
        const rendern::LevelInstance& levelInst,
        float mouseX,
        float mouseY,
        float viewportW,
        float viewportH) noexcept
    {
        PickResult out{};

        const Ray ray = BuildMouseRay(scene, mouseX, mouseY, viewportW, viewportH);
        out.rayOrigin = ray.origin;
        out.rayDir = ray.dir;

        float bestT = std::numeric_limits<float>::infinity();
        int bestNode = -1;

        for (int di = 0; di < static_cast<int>(scene.drawItems.size()); ++di)
        {
            const int nodeIndex = levelInst.GetNodeIndexFromDrawIndex(di);
            if (nodeIndex < 0)
            {
                continue;
            }

            const rendern::DrawItem& item = scene.drawItems[static_cast<std::size_t>(di)];
            if (!item.mesh)
            {
                continue;
            }

            const auto& meshBounds = item.mesh->GetBounds();
            mathUtils::Vec3 wmin{}, wmax{};
            TransformAABB(meshBounds.aabbMin, meshBounds.aabbMax, item.transform.ToMatrix(), wmin, wmax);

            float t = 0.0f;
            if (IntersectRayAABB(ray, wmin, wmax, t))
            {
                if (t < bestT)
                {
                    bestT = t;
                    bestNode = nodeIndex;
                }
            }
        }

        out.nodeIndex = bestNode;
        out.t = bestT;
        return out;
    }
}
