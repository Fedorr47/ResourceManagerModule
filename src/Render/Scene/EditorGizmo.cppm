module;


#include <algorithm>
#include <cmath>
#include <limits>

export module core:editor_gizmo;

import :scene;
import :level;
import :picking;
import :math_utils;
import :geometry;

namespace
{
	static bool ProjectWorldToScreen(const rendern::Scene& scene,
		const mathUtils::Vec3& worldPos,
		float viewportW,
		float viewportH,
		mathUtils::Vec2& outScreen) noexcept
	{
		if (viewportW <= 1.0f || viewportH <= 1.0f)
		{
			return false;
		}

		const float aspect = viewportW / viewportH;
		const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
		const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
		const mathUtils::Vec4 clip = (proj * view) * mathUtils::Vec4(worldPos, 1.0f);
		if (std::fabs(clip.w) < 1e-6f || clip.w <= 0.0f)
		{
			return false;
		}

		const float ndcX = clip.x / clip.w;
		const float ndcY = clip.y / clip.w;
		outScreen.x = (ndcX * 0.5f + 0.5f) * viewportW;
		outScreen.y = (1.0f - (ndcY * 0.5f + 0.5f)) * viewportH;
		return true;
	}

	static float DistancePointToSegmentSq(const mathUtils::Vec2& p, const mathUtils::Vec2& a, const mathUtils::Vec2& b) noexcept
	{
		const mathUtils::Vec2 ab = b - a;
		const mathUtils::Vec2 ap = p - a;
		const float abLenSq = mathUtils::Dot(ab, ab);
		if (abLenSq <= 1e-6f)
		{
			return mathUtils::Dot(ap, ap);
		}

		const float t = std::clamp(mathUtils::Dot(ap, ab) / abLenSq, 0.0f, 1.0f);
		const mathUtils::Vec2 closest = a + ab * t;
		const mathUtils::Vec2 delta = p - closest;
		return mathUtils::Dot(delta, delta);
	}

	static bool PointInTriangle2D(const mathUtils::Vec2& p,
		const mathUtils::Vec2& a,
		const mathUtils::Vec2& b,
		const mathUtils::Vec2& c) noexcept
	{
		const float c0 = mathUtils::Cross2D(b - a, p - a);
		const float c1 = mathUtils::Cross2D(c - b, p - b);
		const float c2 = mathUtils::Cross2D(a - c, p - c);
		const bool hasNeg = (c0 < 0.0f) || (c1 < 0.0f) || (c2 < 0.0f);
		const bool hasPos = (c0 > 0.0f) || (c1 > 0.0f) || (c2 > 0.0f);
		return !(hasNeg && hasPos);
	}

	static bool PointInQuad2D(const mathUtils::Vec2& p,
		const mathUtils::Vec2& a,
		const mathUtils::Vec2& b,
		const mathUtils::Vec2& c,
		const mathUtils::Vec2& d) noexcept
	{
		return PointInTriangle2D(p, a, b, c) || PointInTriangle2D(p, a, c, d);
	}

	static mathUtils::Vec3 SafeNormalizeOr(const mathUtils::Vec3& v, const mathUtils::Vec3& fallback) noexcept
	{
		if (mathUtils::Length(v) < 1e-5f)
		{
			return fallback;
		}
		return mathUtils::Normalize(v);
	}

	static bool IsAxisHandle(rendern::GizmoAxis axis) noexcept
	{
		return axis == rendern::GizmoAxis::X || axis == rendern::GizmoAxis::Y || axis == rendern::GizmoAxis::Z;
	}

	static bool IsPlaneHandle(rendern::GizmoAxis axis) noexcept
	{
		return axis == rendern::GizmoAxis::XY || axis == rendern::GizmoAxis::XZ || axis == rendern::GizmoAxis::YZ;
	}

	static mathUtils::Vec3 AxisDirection(const rendern::TranslateGizmoState& gizmo, rendern::GizmoAxis axis) noexcept
	{
		switch (axis)
		{
		case rendern::GizmoAxis::X: return gizmo.axisXWorld;
		case rendern::GizmoAxis::Y: return gizmo.axisYWorld;
		case rendern::GizmoAxis::Z: return gizmo.axisZWorld;
		default: return mathUtils::Vec3(0.0f, 0.0f, 0.0f);
		}
	}

	static void GetPlaneBasis(const rendern::TranslateGizmoState& gizmo, rendern::GizmoAxis axis, mathUtils::Vec3& basisA, mathUtils::Vec3& basisB, mathUtils::Vec3& planeNormal) noexcept
	{
		switch (axis)
		{
		case rendern::GizmoAxis::XY:
			basisA = gizmo.axisXWorld;
			basisB = gizmo.axisYWorld;
			planeNormal = mathUtils::Normalize(mathUtils::Cross(basisA, basisB));
			break;
		case rendern::GizmoAxis::XZ:
			basisA = gizmo.axisXWorld;
			basisB = gizmo.axisZWorld;
			planeNormal = mathUtils::Normalize(mathUtils::Cross(basisA, basisB));
			break;
		case rendern::GizmoAxis::YZ:
			basisA = gizmo.axisYWorld;
			basisB = gizmo.axisZWorld;
			planeNormal = mathUtils::Normalize(mathUtils::Cross(basisA, basisB));
			break;
		default:
			basisA = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			basisB = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			planeNormal = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			break;
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

	static mathUtils::Vec3 ComputeAxisDragPlaneNormal(const mathUtils::Vec3& axisWorld, const mathUtils::Vec3& viewDir) noexcept
	{
		mathUtils::Vec3 planeNormal = mathUtils::Cross(axisWorld, mathUtils::Cross(viewDir, axisWorld));
		if (mathUtils::Length(planeNormal) < 1e-5f)
		{
			planeNormal = mathUtils::Cross(axisWorld, mathUtils::Vec3(0.0f, 1.0f, 0.0f));
			if (mathUtils::Length(planeNormal) < 1e-5f)
			{
				planeNormal = mathUtils::Cross(axisWorld, mathUtils::Vec3(1.0f, 0.0f, 0.0f));
			}
		}
		return mathUtils::Normalize(planeNormal);
	}

	static bool BuildPlaneHandleQuad(const rendern::Scene& scene,
		const rendern::TranslateGizmoState& gizmo,
		rendern::GizmoAxis axis,
		float viewportW,
		float viewportH,
		mathUtils::Vec2& p0,
		mathUtils::Vec2& p1,
		mathUtils::Vec2& p2,
		mathUtils::Vec2& p3) noexcept
	{
		mathUtils::Vec3 basisA{};
		mathUtils::Vec3 basisB{};
		mathUtils::Vec3 planeNormal{};
		GetPlaneBasis(gizmo, axis, basisA, basisB, planeNormal);
		if (mathUtils::Length(planeNormal) < 1e-5f)
		{
			return false;
		}

		const float inner = gizmo.axisLengthWorld * 0.28f;
		const float outer = gizmo.axisLengthWorld * 0.46f;
		const mathUtils::Vec3 w0 = gizmo.pivotWorld + basisA * inner + basisB * inner;
		const mathUtils::Vec3 w1 = gizmo.pivotWorld + basisA * outer + basisB * inner;
		const mathUtils::Vec3 w2 = gizmo.pivotWorld + basisA * outer + basisB * outer;
		const mathUtils::Vec3 w3 = gizmo.pivotWorld + basisA * inner + basisB * outer;

		return ProjectWorldToScreen(scene, w0, viewportW, viewportH, p0)
			&& ProjectWorldToScreen(scene, w1, viewportW, viewportH, p1)
			&& ProjectWorldToScreen(scene, w2, viewportW, viewportH, p2)
			&& ProjectWorldToScreen(scene, w3, viewportW, viewportH, p3);
	}

	static rendern::GizmoAxis HitTestPlaneHandle(const rendern::Scene& scene,
		const rendern::TranslateGizmoState& gizmo,
		float mouseX,
		float mouseY,
		float viewportW,
		float viewportH) noexcept
	{
		const mathUtils::Vec2 mouse{ mouseX, mouseY };
		const rendern::GizmoAxis planeOrder[] =
		{
			rendern::GizmoAxis::XY,
			rendern::GizmoAxis::XZ,
			rendern::GizmoAxis::YZ,
		};

		for (rendern::GizmoAxis axis : planeOrder)
		{
			mathUtils::Vec2 p0{}, p1{}, p2{}, p3{};
			if (!BuildPlaneHandleQuad(scene, gizmo, axis, viewportW, viewportH, p0, p1, p2, p3))
			{
				continue;
			}

			if (PointInQuad2D(mouse, p0, p1, p2, p3))
			{
				return axis;
			}
		}

		return rendern::GizmoAxis::None;
	}

	static void TestProjectedAxis(
		const rendern::Scene& scene,
		const mathUtils::Vec3& pivotWorld,
		float axisLengthWorld,
		const mathUtils::Vec2& mouse,
		const mathUtils::Vec2& pivotScreen,
		float viewportW,
		float viewportH,
		float thresholdSq,
		rendern::GizmoAxis axis,
		const mathUtils::Vec3& dir,
		float& bestDistSq,
		rendern::GizmoAxis& bestAxis) noexcept
	{
		mathUtils::Vec2 endScreen{};
		if (!ProjectWorldToScreen(scene, pivotWorld + dir * axisLengthWorld, viewportW, viewportH, endScreen))
		{
			return;
		}

		const float distSq = DistancePointToSegmentSq(mouse, pivotScreen, endScreen);
		if (distSq <= thresholdSq && distSq < bestDistSq)
		{
			bestDistSq = distSq;
			bestAxis = axis;
		}
	}

	static rendern::GizmoAxis HitTestAxis(const rendern::Scene& scene,
		const rendern::TranslateGizmoState& gizmo,
		float mouseX,
		float mouseY,
		float viewportW,
		float viewportH) noexcept
	{
		mathUtils::Vec2 pivotScreen{};
		if (!ProjectWorldToScreen(scene, gizmo.pivotWorld, viewportW, viewportH, pivotScreen))
		{
			return rendern::GizmoAxis::None;
		}

		const mathUtils::Vec2 mouse{ mouseX, mouseY };
		const float thresholdSq = 10.0f * 10.0f;
		float bestDistSq = std::numeric_limits<float>::infinity();
		rendern::GizmoAxis bestAxis = rendern::GizmoAxis::None;

		TestProjectedAxis(scene, gizmo.pivotWorld, gizmo.axisLengthWorld, mouse, pivotScreen, viewportW, viewportH, thresholdSq,
			rendern::GizmoAxis::X, gizmo.axisXWorld, bestDistSq, bestAxis);
		TestProjectedAxis(scene, gizmo.pivotWorld, gizmo.axisLengthWorld, mouse, pivotScreen, viewportW, viewportH, thresholdSq,
			rendern::GizmoAxis::Y, gizmo.axisYWorld, bestDistSq, bestAxis);
		TestProjectedAxis(scene, gizmo.pivotWorld, gizmo.axisLengthWorld, mouse, pivotScreen, viewportW, viewportH, thresholdSq,
			rendern::GizmoAxis::Z, gizmo.axisZWorld, bestDistSq, bestAxis);

		return bestAxis;
	}

	static rendern::GizmoAxis HitTestHandle(const rendern::Scene& scene,
		const rendern::TranslateGizmoState& gizmo,
		float mouseX,
		float mouseY,
		float viewportW,
		float viewportH) noexcept
	{
		const rendern::GizmoAxis planeAxis = HitTestPlaneHandle(scene, gizmo, mouseX, mouseY, viewportW, viewportH);
		if (planeAxis != rendern::GizmoAxis::None)
		{
			return planeAxis;
		}
		return HitTestAxis(scene, gizmo, mouseX, mouseY, viewportW, viewportH);
	}
}

export namespace rendern
{
	class TranslateGizmoController
	{
	public:
		void SyncVisual(const LevelAsset& asset, const LevelInstance& levelInst, Scene& scene) const noexcept
		{
			TranslateGizmoState& gizmo = scene.editorTranslateGizmo;
			if (!gizmo.enabled || scene.editorGizmoMode != GizmoMode::Translate)
			{
				gizmo.visible = false;
				gizmo.hoveredAxis = GizmoAxis::None;
				gizmo.activeAxis = GizmoAxis::None;
				return;
			}

			const int selectedNode = scene.editorSelectedNode;
			if (!levelInst.IsNodeAlive(asset, selectedNode))
			{
				gizmo.visible = false;
				gizmo.hoveredAxis = GizmoAxis::None;
				gizmo.activeAxis = GizmoAxis::None;
				return;
			}

			gizmo.visible = true;
			gizmo.pivotWorld = levelInst.GetNodeWorldPosition(selectedNode);

			if (scene.editorTranslateSpace == GizmoSpace::Local)
			{
				const mathUtils::Mat4 world = levelInst.GetNodeWorldMatrix(selectedNode);
				gizmo.axisXWorld = SafeNormalizeOr(mathUtils::TransformVector(world, mathUtils::Vec3(1.0f, 0.0f, 0.0f)), mathUtils::Vec3(1.0f, 0.0f, 0.0f));
				gizmo.axisYWorld = SafeNormalizeOr(mathUtils::TransformVector(world, mathUtils::Vec3(0.0f, 1.0f, 0.0f)), mathUtils::Vec3(0.0f, 1.0f, 0.0f));
				gizmo.axisZWorld = SafeNormalizeOr(mathUtils::TransformVector(world, mathUtils::Vec3(0.0f, 0.0f, 1.0f)), mathUtils::Vec3(0.0f, 0.0f, 1.0f));
			}
			else
			{
				gizmo.axisXWorld = mathUtils::Vec3(1.0f, 0.0f, 0.0f);
				gizmo.axisYWorld = mathUtils::Vec3(0.0f, 1.0f, 0.0f);
				gizmo.axisZWorld = mathUtils::Vec3(0.0f, 0.0f, 1.0f);
			}

			const float distToCamera = mathUtils::Length(scene.camera.position - gizmo.pivotWorld);
			gizmo.axisLengthWorld = std::clamp(distToCamera * 0.18f, 0.35f, 4.0f);
		}

		void ClearHover(Scene& scene) const noexcept
		{
			scene.editorTranslateGizmo.hoveredAxis = GizmoAxis::None;
		}

		void UpdateHover(Scene& scene, float mouseX, float mouseY, float viewportW, float viewportH) const noexcept
		{
			TranslateGizmoState& gizmo = scene.editorTranslateGizmo;
			if (!gizmo.enabled || !gizmo.visible || gizmo.activeAxis != GizmoAxis::None)
			{
				gizmo.hoveredAxis = GizmoAxis::None;
				return;
			}

			gizmo.hoveredAxis = HitTestHandle(scene, gizmo, mouseX, mouseY, viewportW, viewportH);
		}

		bool TryBeginDrag(const LevelAsset& asset,
			const LevelInstance& levelInst,
			Scene& scene,
			float mouseX,
			float mouseY,
			float viewportW,
			float viewportH) noexcept
		{
			TranslateGizmoState& gizmo = scene.editorTranslateGizmo;
			if (!gizmo.enabled || !gizmo.visible || dragging_)
			{
				return false;
			}

			const int selectedNode = scene.editorSelectedNode;
			if (!levelInst.IsNodeAlive(asset, selectedNode))
			{
				return false;
			}

			const GizmoAxis axis = HitTestHandle(scene, gizmo, mouseX, mouseY, viewportW, viewportH);
			if (axis == GizmoAxis::None)
			{
				return false;
			}

			mathUtils::Vec3 planeNormal{};
			mathUtils::Vec3 axisWorld{};
			if (IsAxisHandle(axis))
			{
				axisWorld = AxisDirection(gizmo, axis);
				const mathUtils::Vec3 viewDir = mathUtils::Normalize(scene.camera.target - scene.camera.position);
				planeNormal = ComputeAxisDragPlaneNormal(axisWorld, viewDir);
			}
			else if (IsPlaneHandle(axis))
			{
				mathUtils::Vec3 basisA{};
				mathUtils::Vec3 basisB{};
				GetPlaneBasis(gizmo, axis, basisA, basisB, planeNormal);
			}

			if (mathUtils::Length(planeNormal) < 1e-5f)
			{
				return false;
			}

			mathUtils::Vec3 startHit{};
			if (!IntersectRayPlane(BuildMouseRay(scene, mouseX, mouseY, viewportW, viewportH), gizmo.pivotWorld, planeNormal, startHit))
			{
				return false;
			}

			dragging_ = true;
			dragNodeIndex_ = selectedNode;
			dragAxis_ = axis;
			dragStartLocalPosition_ = asset.nodes[static_cast<std::size_t>(selectedNode)].transform.position;
			dragStartWorldHit_ = startHit;
			dragAxisWorld_ = axisWorld;
			dragPlaneNormal_ = planeNormal;
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
			if (!dragging_ || dragNodeIndex_ < 0 || !levelInst.IsNodeAlive(asset, dragNodeIndex_))
			{
				return false;
			}

			mathUtils::Vec3 currentHit{};
			if (!IntersectRayPlane(BuildMouseRay(scene, mouseX, mouseY, viewportW, viewportH), dragPivotWorld_, dragPlaneNormal_, currentHit))
			{
				return false;
			}

			mathUtils::Vec3 worldDelta{};
			if (IsAxisHandle(dragAxis_))
			{
				const float axisDelta = mathUtils::Dot(currentHit - dragStartWorldHit_, dragAxisWorld_);
				worldDelta = dragAxisWorld_ * axisDelta;
			}
			else if (IsPlaneHandle(dragAxis_))
			{
				worldDelta = currentHit - dragStartWorldHit_;
			}
			else
			{
				return false;
			}

			const mathUtils::Mat4 invParentWorld = mathUtils::Inverse(levelInst.GetParentWorldMatrix(asset, dragNodeIndex_));
			const mathUtils::Vec3 localDelta = mathUtils::TransformVector(invParentWorld, worldDelta);

			mathUtils::Vec3 newLocalPosition = dragStartLocalPosition_ + localDelta;
			if (snapEnabled)
			{
				constexpr float kTranslateSnapStep = 0.5f;
				newLocalPosition.x = std::round(newLocalPosition.x / kTranslateSnapStep) * kTranslateSnapStep;
				newLocalPosition.y = std::round(newLocalPosition.y / kTranslateSnapStep) * kTranslateSnapStep;
				newLocalPosition.z = std::round(newLocalPosition.z / kTranslateSnapStep) * kTranslateSnapStep;
			}

			asset.nodes[static_cast<std::size_t>(dragNodeIndex_)].transform.position = newLocalPosition;
			scene.editorTranslateGizmo.hoveredAxis = dragAxis_;
			return true;
		}

		void EndDrag(Scene& scene) noexcept
		{
			dragging_ = false;
			dragNodeIndex_ = -1;
			dragAxis_ = GizmoAxis::None;
			dragStartLocalPosition_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			dragStartWorldHit_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			dragAxisWorld_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			dragPlaneNormal_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			dragPivotWorld_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			scene.editorTranslateGizmo.activeAxis = GizmoAxis::None;
			scene.editorTranslateGizmo.hoveredAxis = GizmoAxis::None;
		}

		bool IsDragging() const noexcept
		{
			return dragging_;
		}

	private:
		bool dragging_{ false };
		int dragNodeIndex_{ -1 };
		GizmoAxis dragAxis_{ GizmoAxis::None };
		mathUtils::Vec3 dragStartLocalPosition_{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragStartWorldHit_{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragAxisWorld_{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragPlaneNormal_{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragPivotWorld_{ 0.0f, 0.0f, 0.0f };
	};
}