module;

#include <algorithm>

export module core:editor_scale_gizmo;

import :scene;
import :level;
import :picking;
import :geometry;
import :math_utils;

import std;

namespace
{
	constexpr float kPlaneHandleInnerFraction = 0.24f;
	constexpr float kPlaneHandleOuterFraction = 0.40f;
	constexpr float kMinScaleComponent = 0.001f;

	static mathUtils::Vec3 SafeNormalizeOr(const mathUtils::Vec3& v, const mathUtils::Vec3& fallback) noexcept
	{
		if (mathUtils::Length(v) < 1e-5f)
		{
			return fallback;
		}
		return mathUtils::Normalize(v);
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

	static bool IsAxisHandle(rendern::GizmoAxis axis) noexcept
	{
		return axis == rendern::GizmoAxis::X || axis == rendern::GizmoAxis::Y || axis == rendern::GizmoAxis::Z;
	}

	static bool IsPlaneHandle(rendern::GizmoAxis axis) noexcept
	{
		return axis == rendern::GizmoAxis::XY || axis == rendern::GizmoAxis::XZ || axis == rendern::GizmoAxis::YZ;
	}

	static mathUtils::Vec3 AxisDirection(const rendern::ScaleGizmoState& gizmo, rendern::GizmoAxis axis) noexcept
	{
		switch (axis)
		{
		case rendern::GizmoAxis::X: return gizmo.axisXWorld;
		case rendern::GizmoAxis::Y: return gizmo.axisYWorld;
		case rendern::GizmoAxis::Z: return gizmo.axisZWorld;
		default: return mathUtils::Vec3(0.0f, 0.0f, 0.0f);
		}
	}

	static void GetPlaneBasis(const rendern::ScaleGizmoState& gizmo,
		rendern::GizmoAxis axis,
		mathUtils::Vec3& basisA,
		mathUtils::Vec3& basisB,
		mathUtils::Vec3& planeNormal) noexcept
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

	static bool BuildPlaneHandleQuad(const rendern::Scene& scene,
		const rendern::ScaleGizmoState& gizmo,
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

		const float inner = gizmo.axisLengthWorld * kPlaneHandleInnerFraction;
		const float outer = gizmo.axisLengthWorld * kPlaneHandleOuterFraction;
		const mathUtils::Vec3 w0 = gizmo.pivotWorld + basisA * inner + basisB * inner;
		const mathUtils::Vec3 w1 = gizmo.pivotWorld + basisA * outer + basisB * inner;
		const mathUtils::Vec3 w2 = gizmo.pivotWorld + basisA * outer + basisB * outer;
		const mathUtils::Vec3 w3 = gizmo.pivotWorld + basisA * inner + basisB * outer;

		return ProjectWorldToScreen(scene, w0, viewportW, viewportH, p0)
			&& ProjectWorldToScreen(scene, w1, viewportW, viewportH, p1)
			&& ProjectWorldToScreen(scene, w2, viewportW, viewportH, p2)
			&& ProjectWorldToScreen(scene, w3, viewportW, viewportH, p3);
	}

	static rendern::GizmoAxis HitTestUniformHandle(const rendern::Scene& scene,
		const rendern::ScaleGizmoState& gizmo,
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
		const mathUtils::Vec2 delta = mouse - pivotScreen;
		const float radiusPx = 12.0f;
		return mathUtils::Dot(delta, delta) <= radiusPx * radiusPx ? rendern::GizmoAxis::XYZ : rendern::GizmoAxis::None;
	}

	static rendern::GizmoAxis HitTestPlaneHandle(const rendern::Scene& scene,
		const rendern::ScaleGizmoState& gizmo,
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

	static rendern::GizmoAxis HitTestAxisHandle(const rendern::Scene& scene,
		const rendern::ScaleGizmoState& gizmo,
		float mouseX,
		float mouseY,
		float viewportW,
		float viewportH) noexcept
	{
		const mathUtils::Vec2 mouse{ mouseX, mouseY };
		const float thresholdSq = 12.0f * 12.0f;
		float bestDistSq = std::numeric_limits<float>::infinity();
		rendern::GizmoAxis bestAxis = rendern::GizmoAxis::None;

		auto TestAxis = [&](rendern::GizmoAxis axis, const mathUtils::Vec3& dir) noexcept
			{
				mathUtils::Vec2 handleScreen{};
				if (!ProjectWorldToScreen(scene, gizmo.pivotWorld + dir * gizmo.axisLengthWorld, viewportW, viewportH, handleScreen))
				{
					return;
				}

				const mathUtils::Vec2 delta = mouse - handleScreen;
				const float distSq = mathUtils::Dot(delta, delta);
				if (distSq <= thresholdSq && distSq < bestDistSq)
				{
					bestDistSq = distSq;
					bestAxis = axis;
				}
			};

		TestAxis(rendern::GizmoAxis::X, gizmo.axisXWorld);
		TestAxis(rendern::GizmoAxis::Y, gizmo.axisYWorld);
		TestAxis(rendern::GizmoAxis::Z, gizmo.axisZWorld);
		return bestAxis;
	}

	static rendern::GizmoAxis HitTestHandle(const rendern::Scene& scene,
		const rendern::ScaleGizmoState& gizmo,
		float mouseX,
		float mouseY,
		float viewportW,
		float viewportH) noexcept
	{
		const rendern::GizmoAxis uniformAxis = HitTestUniformHandle(scene, gizmo, mouseX, mouseY, viewportW, viewportH);
		if (uniformAxis != rendern::GizmoAxis::None)
		{
			return uniformAxis;
		}

		const rendern::GizmoAxis planeAxis = HitTestPlaneHandle(scene, gizmo, mouseX, mouseY, viewportW, viewportH);
		if (planeAxis != rendern::GizmoAxis::None)
		{
			return planeAxis;
		}
		return HitTestAxisHandle(scene, gizmo, mouseX, mouseY, viewportW, viewportH);
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

#include "SceneImpl/EditorGizmoShared.inl"
}

export namespace rendern
{
	class ScaleGizmoController
	{
	public:
		void SyncVisual(const LevelAsset& asset, const LevelInstance& levelInst, Scene& scene) const noexcept
		{
			ScaleGizmoState& gizmo = scene.editorScaleGizmo;
			if (!gizmo.enabled || scene.editorGizmoMode != GizmoMode::Scale)
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
			gizmo.axisLengthWorld = std::clamp(distToCamera * 0.18f, 0.35f, 4.0f);
			gizmo.uniformHandleRadiusWorld = gizmo.axisLengthWorld * 0.12f;
		}

		void ClearHover(Scene& scene) const noexcept
		{
			scene.editorScaleGizmo.hoveredAxis = GizmoAxis::None;
		}

		void UpdateHover(Scene& scene, float mouseX, float mouseY, float viewportW, float viewportH) const noexcept
		{
			ScaleGizmoState& gizmo = scene.editorScaleGizmo;
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
			ScaleGizmoState& gizmo = scene.editorScaleGizmo;
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

			const GizmoAxis axis = HitTestHandle(scene, gizmo, mouseX, mouseY, viewportW, viewportH);
			if (axis == GizmoAxis::None)
			{
				return false;
			}

			const geometry::Ray ray = BuildMouseRay(scene, mouseX, mouseY, viewportW, viewportH);
			const mathUtils::Vec3 viewDir = mathUtils::Normalize(scene.camera.target - scene.camera.position);

			mathUtils::Vec3 planeNormal{};
			mathUtils::Vec3 startHit{};
			mathUtils::Vec3 basisA{};
			mathUtils::Vec3 basisB{};
			if (axis == GizmoAxis::XYZ)
			{
				planeNormal = viewDir;
				dragAxisWorld_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			}
			else if (IsAxisHandle(axis))
			{
				dragAxisWorld_ = AxisDirection(gizmo, axis);
				planeNormal = ComputeAxisDragPlaneNormal(dragAxisWorld_, viewDir);
			}
			else if (IsPlaneHandle(axis))
			{
				dragAxisWorld_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
				GetPlaneBasis(gizmo, axis, basisA, basisB, planeNormal);
			}
			if (mathUtils::Length(planeNormal) < 1e-5f)
			{
				return false;
			}

			if (!IntersectRayPlane(ray, gizmo.pivotWorld, planeNormal, startHit))
			{
				return false;
			}

			dragging_ = true;
			dragNodeIndices_.clear();
			dragStartLocalScales_.clear();
			dragNodeIndices_.reserve(scene.editorSelectedNodes.size());
			dragStartLocalScales_.reserve(scene.editorSelectedNodes.size());
			for (const int nodeIndex : scene.editorSelectedNodes)
			{
				if (!levelInst.IsNodeAlive(asset, nodeIndex))
				{
					continue;
				}
				dragNodeIndices_.push_back(nodeIndex);
				dragStartLocalScales_.push_back(asset.nodes[static_cast<std::size_t>(nodeIndex)].transform.scale);
			}
			if (dragNodeIndices_.empty())
			{
				dragging_ = false;
				return false;
			}
			dragAxis_ = axis;
			dragStartWorldHit_ = startHit;
			dragPlaneNormal_ = planeNormal;
			dragPivotWorld_ = gizmo.pivotWorld;
			dragReferenceLengthWorld_ = std::max(gizmo.axisLengthWorld, 0.001f);
			dragStartRadius_ = std::max(1e-4f, mathUtils::Length(startHit - dragPivotWorld_));
			dragBasisAWorld_ = basisA;
			dragBasisBWorld_ = basisB;

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
			if (!IntersectRayPlane(BuildMouseRay(scene, mouseX, mouseY, viewportW, viewportH), dragPivotWorld_, dragPlaneNormal_, currentHit))
			{
				return false;
			}

			// Precompute factors from the drag motion (independent from per-node start scale).
			float uniformFactor = 1.0f;
			float axisFactor = 1.0f;
			float factorA = 1.0f;
			float factorB = 1.0f;

			if (dragAxis_ == GizmoAxis::XYZ)
			{
				const float currentRadius = std::max(1e-4f, mathUtils::Length(currentHit - dragPivotWorld_));
				uniformFactor = currentRadius / dragStartRadius_;
				uniformFactor = std::clamp(uniformFactor, 0.01f, 100.0f);
				if (snapEnabled)
				{
					uniformFactor = std::max(0.1f, std::round(uniformFactor * 10.0f) / 10.0f);
				}
			}
			else if (IsAxisHandle(dragAxis_))
			{
				const float axisDelta = mathUtils::Dot(currentHit - dragStartWorldHit_, dragAxisWorld_);
				axisFactor = 1.0f + (axisDelta / dragReferenceLengthWorld_);
				axisFactor = std::max(axisFactor, kMinScaleComponent);
			}
			else if (IsPlaneHandle(dragAxis_))
			{
				const mathUtils::Vec3 worldDelta = currentHit - dragStartWorldHit_;
				const float axisDeltaA = mathUtils::Dot(worldDelta, dragBasisAWorld_);
				const float axisDeltaB = mathUtils::Dot(worldDelta, dragBasisBWorld_);
				factorA = std::max(1.0f + (axisDeltaA / dragReferenceLengthWorld_), kMinScaleComponent);
				factorB = std::max(1.0f + (axisDeltaB / dragReferenceLengthWorld_), kMinScaleComponent);
			}
			else
			{
				return false;
			}

			constexpr float kScaleSnapStep = 0.1f;
			auto SnapPositive = [](float value) noexcept
				{
					return std::max(std::round(value / kScaleSnapStep) * kScaleSnapStep, kMinScaleComponent);
				};

			const std::size_t n = dragNodeIndices_.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				const int nodeIndex = dragNodeIndices_[i];
				if (!levelInst.IsNodeAlive(asset, nodeIndex))
				{
					continue;
				}

				const mathUtils::Vec3 startScale = dragStartLocalScales_[i];
				auto scale = startScale;
				if (dragAxis_ == GizmoAxis::XYZ)
				{
					scale = mathUtils::MaxVec3(startScale * uniformFactor, mathUtils::Vec3(kMinScaleComponent, kMinScaleComponent, kMinScaleComponent));
				}
				else if (IsAxisHandle(dragAxis_))
				{
					switch (dragAxis_)
					{
					case GizmoAxis::X: scale.x = std::max(startScale.x * axisFactor, kMinScaleComponent); break;
					case GizmoAxis::Y: scale.y = std::max(startScale.y * axisFactor, kMinScaleComponent); break;
					case GizmoAxis::Z: scale.z = std::max(startScale.z * axisFactor, kMinScaleComponent); break;
					default: return false;
					}
				}
				else if (IsPlaneHandle(dragAxis_))
				{
					switch (dragAxis_)
					{
					case GizmoAxis::XY:
						scale.x = std::max(startScale.x * factorA, kMinScaleComponent);
						scale.y = std::max(startScale.y * factorB, kMinScaleComponent);
						break;
					case GizmoAxis::XZ:
						scale.x = std::max(startScale.x * factorA, kMinScaleComponent);
						scale.z = std::max(startScale.z * factorB, kMinScaleComponent);
						break;
					case GizmoAxis::YZ:
						scale.y = std::max(startScale.y * factorA, kMinScaleComponent);
						scale.z = std::max(startScale.z * factorB, kMinScaleComponent);
						break;
					default:
						return false;
					}
				}

				if (snapEnabled)
				{
					switch (dragAxis_)
					{
					case GizmoAxis::X:
						scale.x = SnapPositive(scale.x);
						break;
					case GizmoAxis::Y:
						scale.y = SnapPositive(scale.y);
						break;
					case GizmoAxis::Z:
						scale.z = SnapPositive(scale.z);
						break;
					case GizmoAxis::XY:
						scale.x = SnapPositive(scale.x);
						scale.y = SnapPositive(scale.y);
						break;
					case GizmoAxis::XZ:
						scale.x = SnapPositive(scale.x);
						scale.z = SnapPositive(scale.z);
						break;
					case GizmoAxis::YZ:
						scale.y = SnapPositive(scale.y);
						scale.z = SnapPositive(scale.z);
						break;
					case GizmoAxis::XYZ:
					case GizmoAxis::None:
					default:
						break;
					}
				}

				asset.nodes[static_cast<std::size_t>(nodeIndex)].transform.scale = scale;
			}
			scene.editorScaleGizmo.hoveredAxis = dragAxis_;
			return true;
		}

		void EndDrag(Scene& scene) noexcept
		{
			dragging_ = false;
			dragNodeIndices_.clear();
			dragStartLocalScales_.clear();
			dragAxis_ = GizmoAxis::None;
			dragStartWorldHit_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			dragAxisWorld_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			dragPlaneNormal_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			dragPivotWorld_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			dragReferenceLengthWorld_ = 1.0f;
			dragStartRadius_ = 1.0f;
			dragBasisAWorld_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			dragBasisBWorld_ = mathUtils::Vec3(0.0f, 0.0f, 0.0f);
			scene.editorScaleGizmo.activeAxis = GizmoAxis::None;
			scene.editorScaleGizmo.hoveredAxis = GizmoAxis::None;
		}

		bool IsDragging() const noexcept
		{
			return dragging_;
		}

	private:
		bool dragging_{ false };
		std::vector<int> dragNodeIndices_;
		GizmoAxis dragAxis_{ GizmoAxis::None };
		std::vector<mathUtils::Vec3> dragStartLocalScales_;
		mathUtils::Vec3 dragStartWorldHit_{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragAxisWorld_{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragBasisAWorld_{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragBasisBWorld_{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragPlaneNormal_{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 dragPivotWorld_{ 0.0f, 0.0f, 0.0f };
		float dragReferenceLengthWorld_{ 1.0f };
		float dragStartRadius_{ 1.0f };
	};
}