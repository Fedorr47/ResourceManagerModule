module;

#if defined(_WIN32)
// Prevent Windows headers from defining the `min`/`max` macros which break `std::min`/`std::max`.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#endif

export module core:visibility;

import :resource_manager_mesh;
import :math_utils;
import :skinned_mesh;
import std;

export namespace rendern
{
	[[nodiscard]] bool IsVisibleSphere(
		const mathUtils::Vec3& sphereCenter,
		float sphereRadius,
		const mathUtils::Mat4& model,
		const mathUtils::Frustum& cameraFrustum,
		bool doFrustumCulling)
	{
		if (!doFrustumCulling)
		{
			return true;
		}
		if (sphereRadius <= 0.0f)
		{
			return true;
		}

		const mathUtils::Vec4 wc4 = model * mathUtils::Vec4(sphereCenter, 1.0f);
		const mathUtils::Vec3 worldCenter{ wc4.x, wc4.y, wc4.z };

		const mathUtils::Vec3 c0{ model[0].x, model[0].y, model[0].z };
		const mathUtils::Vec3 c1{ model[1].x, model[1].y, model[1].z };
		const mathUtils::Vec3 c2{ model[2].x, model[2].y, model[2].z };
		const float s0 = mathUtils::Length(c0);
		const float s1 = mathUtils::Length(c1);
		const float s2 = mathUtils::Length(c2);
		const float maxScale = std::max(s0, std::max(s1, s2));
		const float worldRadius = sphereRadius * maxScale;

		return mathUtils::IntersectsSphere(cameraFrustum, worldCenter, worldRadius);
	}

	bool IsVisible(
		const rendern::MeshResource* meshRes,
		const mathUtils::Mat4& model,
		const mathUtils::Frustum& cameraFrustum,
		bool doFrustumCulling)
	{
		if (!doFrustumCulling || !meshRes)
		{
			return true;
		}
		const auto& b = meshRes->GetBounds();
		return IsVisibleSphere(b.sphereCenter, b.sphereRadius, model, cameraFrustum, doFrustumCulling);
	}

	bool IsVisible(
		const rendern::SkinnedAssetBundle* asset,
		const mathUtils::Mat4& model,
		const mathUtils::Frustum& cameraFrustum,
		bool doFrustumCulling)
	{
		if (!doFrustumCulling || asset == nullptr)
		{
			return true;
		}

		const SkinnedBounds& bounds =
			(asset->mesh.bounds.maxAnimatedBounds.sphereRadius > 0.0f)
			? asset->mesh.bounds.maxAnimatedBounds
			: asset->mesh.bounds.bindPoseBounds;

		return IsVisibleSphere(bounds.sphereCenter, bounds.sphereRadius, model, cameraFrustum, doFrustumCulling);
	}
}