module;

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

export module core:editor_rotate_gizmo;

import :scene;
import :level;
import :math_utils;
import :picking;
import :geometry;

namespace
{
	static mathUtils::Vec3 SafeNormalizeOr(const mathUtils::Vec3& v, const mathUtils::Vec3& fallback) noexcept
	{
		if (mathUtils::Length(v) < 1e-5f)
		{
			return fallback;
		}
		return mathUtils::Normalize(v);
	}

	static mathUtils::Vec3 AxisDirection(const rendern::RotateGizmoState& gizmo, rendern::GizmoAxis axis) noexcept
	{
		switch (axis)
		{
		case rendern::GizmoAxis::X: return gizmo.axisXWorld;
		case rendern::GizmoAxis::Y: return gizmo.axisYWorld;
		case rendern::GizmoAxis::Z: return gizmo.axisZWorld;
		default: return mathUtils::Vec3(0.0f, 0.0f, 0.0f);
		}
	}

	static bool IntersectRayPlane(const geometry::Ray& ray,
		const mathUtils::Vec3& planePoint,
		const mathUtils::Vec3& planeNormal,
		mathUtils::Vec3& outPoint) noexcept
	{
		const float denom = mathUtils::Dot(planeNormal, ray.dir);
		if (std::fabs(denom) < 1e-6f)
		{
			return false;
		}

		const float t = mathUtils::Dot(planePoint - ray.origin, planeNormal) / denom;
		if (t < 0.0f)
		{
			return false;
		}

		outPoint = ray.origin + ray.dir * t;
		return true;
	}

	static rendern::GizmoAxis HitTestRing(const rendern::Scene& scene,
		const rendern::RotateGizmoState& gizmo,
		float mouseX,
		float mouseY,
		float viewportW,
		float viewportH) noexcept
	{
		const geometry::Ray ray = rendern::BuildMouseRay(scene, mouseX, mouseY, viewportW, viewportH);
		const float tolerance = std::max(gizmo.ringRadiusWorld * 0.18f, 0.06f);

		float bestDist = std::numeric_limits<float>::infinity();
		rendern::GizmoAxis bestAxis = rendern::GizmoAxis::None;
		for (rendern::GizmoAxis axis : { rendern::GizmoAxis::X, rendern::GizmoAxis::Y, rendern::GizmoAxis::Z })
		{
			const mathUtils::Vec3 axisWorld = AxisDirection(gizmo, axis);
			mathUtils::Vec3 hit{};
			if (!IntersectRayPlane(ray, gizmo.pivotWorld, axisWorld, hit))
			{
				continue;
			}

			const float radial = mathUtils::Length(hit - gizmo.pivotWorld);
			const float dist = std::fabs(radial - gizmo.ringRadiusWorld);
			if (dist <= tolerance && dist < bestDist)
			{
				bestDist = dist;
				bestAxis = axis;
			}
		}

		return bestAxis;
	}

#include "SceneImpl/EditorGizmoShared.inl"
}

export namespace rendern
{
	class RotateGizmoController
	{
	public:
		void SyncVisual(const LevelAsset& asset, const LevelInstance& levelInst, Scene& scene) const noexcept
		{
			RotateGizmoState& gizmo = scene.editorRotateGizmo;
			if (!gizmo.enabled)
			{
				gizmo.visible = false;
				gizmo.hoveredAxis = GizmoAxis::None;
				gizmo.activeAxis = GizmoAxis::None;
				return;
			}

			// Multi-selection pivot: centroid of all selected nodes.
			mathUtils::Vec3 sum{ 0.0f, 0.0f, 0.0f };
			int count = 0;
			for (const int nodeIndex : scene.editorSelectedNodes)
			{
				if (levelInst.IsNodeAlive(asset, nodeIndex))
				{
					sum = sum + levelInst.GetNodeWorldPosition(nodeIndex);
					++count;
				}
			}

			const int selectedNode = scene.editorSelectedNode;
			if (!levelInst.IsNodeAlive(asset, selectedNode))
			{
				gizmo.visible = false;
				gizmo.hoveredAxis = GizmoAxis::None;
				gizmo.activeAxis = GizmoAxis::None;
				return;
			}
			if (count == 0)
			{
				sum = levelInst.GetNodeWorldPosition(selectedNode);
				count = 1;
			}

			const mathUtils::Mat4& nodeWorld = levelInst.GetNodeWorldMatrix(selectedNode);
			gizmo.visible = true;
			gizmo.pivotWorld = sum * (1.0f / static_cast<float>(count));
			gizmo.axisXWorld = SafeNormalizeOr(mathUtils::TransformVector(nodeWorld, mathUtils::Vec3(1.0f, 0.0f, 0.0f)), mathUtils::Vec3(1.0f, 0.0f, 0.0f));
			gizmo.axisYWorld = SafeNormalizeOr(mathUtils::TransformVector(nodeWorld, mathUtils::Vec3(0.0f, 1.0f, 0.0f)), mathUtils::Vec3(0.0f, 1.0f, 0.0f));
			gizmo.axisZWorld = SafeNormalizeOr(mathUtils::TransformVector(nodeWorld, mathUtils::Vec3(0.0f, 0.0f, 1.0f)), mathUtils::Vec3(0.0f, 0.0f, 1.0f));

			const float distToCamera = mathUtils::Length(scene.camera.position - gizmo.pivotWorld);
			gizmo.ringRadiusWorld = std::clamp(distToCamera * 0.12f, 0.35f, 3.0f);
		}

		void ClearHover(Scene& scene) const noexcept
		{
			scene.editorRotateGizmo.hoveredAxis = GizmoAxis::None;
		}

		void UpdateHover(Scene& scene, float mouseX, float mouseY, float viewportW, float viewportH) const noexcept
		{
			RotateGizmoState& gizmo = scene.editorRotateGizmo;
			if (!gizmo.enabled || !gizmo.visible || gizmo.activeAxis != GizmoAxis::None)
			{
				gizmo.hoveredAxis = GizmoAxis::None;
				return;
			}

			gizmo.hoveredAxis = HitTestRing(scene, gizmo, mouseX, mouseY, viewportW, viewportH);
		}

		bool TryBeginDrag(const LevelAsset& asset,
			const LevelInstance& levelInst,
			Scene& scene,
			float mouseX,
			float mouseY,
			float viewportW,
			float viewportH) noexcept
		{
			RotateGizmoState& gizmo = scene.editorRotateGizmo;
			if (!gizmo.enabled || !gizmo.visible || dragging_)
			{
				return false;
			}

			const int primaryNode = scene.editorSelectedNode;
			if (!levelInst.IsNodeAlive(asset, primaryNode))
			{
				return false;
			}
			if (scene.editorSelectedNodes.empty())
			{
				scene.editorSelectedNodes.push_back(primaryNode);
			}

			const GizmoAxis axis = HitTestRing(scene, gizmo, mouseX, mouseY, viewportW, viewportH);
			if (axis == GizmoAxis::None)
			{
				return false;
			}

			const mathUtils::Vec3 axisWorld = AxisDirection(gizmo, axis);
			mathUtils::Vec3 startHit{};
			if (!IntersectRayPlane(BuildMouseRay(scene, mouseX, mouseY, viewportW, viewportH), gizmo.pivotWorld, axisWorld, startHit))
			{
				return false;
			}

			const mathUtils::Vec3 startDir = startHit - gizmo.pivotWorld;
			if (mathUtils::Length(startDir) < 1e-5f)
			{
				return false;
			}

			dragging_ = true;
			dragNodeIndices_.clear();
			dragStartLocalRotations_.clear();
			dragNodeIndices_.reserve(scene.editorSelectedNodes.size());
			dragStartLocalRotations_.reserve(scene.editorSelectedNodes.size());
			for (const int nodeIndex : scene.editorSelectedNodes)
			{
				if (!levelInst.IsNodeAlive(asset, nodeIndex))
				{
					continue;
				}
				dragNodeIndices_.push_back(nodeIndex);
				dragStartLocalRotations_.push_back(asset.nodes[static_cast<std::size_t>(nodeIndex)].transform.rotationDegrees);
			}
			if (dragNodeIndices_.empty())
			{
				dragging_ = false;
				return false;
			}
			dragAxis_ = axis;
			dragStartHitDirWorld_ = mathUtils::Normalize(startDir);
			dragAxisWorld_ = axisWorld;
			dragPivotWorld_ = gizmo.pivotWorld;

			gizmo.activeAxis = axis;
			gizmo.hoveredAxis = axis;
			return true;
		}

		bool UpdateDrag(LevelAsset& asset,
			const LevelInstance& levelInst,
			Scene& scene,
			float mouseX,
			float mouseY,
			float viewportW,
			float viewportH,
			bool snapEnabled) noexcept
		{
			if (!dragging_ || dragNodeIndices_.empty())
			{
				return false;
			}

			mathUtils::Vec3 currentHit{};
			if (!IntersectRayPlane(BuildMouseRay(scene, mouseX, mouseY, viewportW, viewportH), dragPivotWorld_, dragAxisWorld_, currentHit))
			{
				return false;
			}

			const mathUtils::Vec3 currentDir = currentHit - dragPivotWorld_;
			if (mathUtils::Length(currentDir) < 1e-5f)
			{
				return false;
			}

			const mathUtils::Vec3 curN = mathUtils::Normalize(currentDir);
			const float signedAngleRad = std::atan2(
				mathUtils::Dot(dragAxisWorld_, mathUtils::Cross(dragStartHitDirWorld_, curN)),
				mathUtils::Dot(dragStartHitDirWorld_, curN));
			float angleDeg = mathUtils::RadToDeg(signedAngleRad);
			if (snapEnabled)
			{
				constexpr float kRotateSnapStepDeg = 15.0f;
				angleDeg = std::round(angleDeg / kRotateSnapStepDeg) * kRotateSnapStepDeg;
			}

			const std::size_t n = dragNodeIndices_.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				const int nodeIndex = dragNodeIndices_[i];
				if (!levelInst.IsNodeAlive(asset, nodeIndex))
				{
					continue;
				}
				auto& rotation = asset.nodes[static_cast<std::size_t>(nodeIndex)].transform.rotationDegrees;
				rotation = dragStartLocalRotations_[i];
				switch (dragAxis_)
				{
				case GizmoAxis::X: rotation.x += angleDeg; break;
				case GizmoAxis::Y: rotation.y += angleDeg; break;
				case GizmoAxis::Z: rotation.z += angleDeg; break;
				default: return false;
				}
			}

			scene.editorRotateGizmo.hoveredAxis = dragAxis_;
			return true;
		}

		void EndDrag(Scene& scene) noexcept
		{
			dragging_ = false;
			dragNodeIndices_.clear();
			dragStartLocalRotations_.clear();
			dragAxis_ = GizmoAxis::None;
			dragStartHitDirWorld_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			dragAxisWorld_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			dragPivotWorld_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			scene.editorRotateGizmo.activeAxis = GizmoAxis::None;
			scene.editorRotateGizmo.hoveredAxis = GizmoAxis::None;
		}

		bool IsDragging() const noexcept
		{
			return dragging_;
		}

	private:
		bool dragging_{ false };
		std::vector<int> dragNodeIndices_;
		GizmoAxis dragAxis_{ GizmoAxis::None };
		std::vector<mathUtils::Vec3> dragStartLocalRotations_;
		mathUtils::Vec3 dragStartHitDirWorld_{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragAxisWorld_{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragPivotWorld_{ 0.0f, 0.0f, 0.0f };
	};
}