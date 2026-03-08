// Debug primitives (no ImGui dependency) - rendered in the main view.
debugDraw::DebugDrawList debugList;
debugText::DebugTextList textList;

auto ProjectWorldToScreenPx = [&](const mathUtils::Vec3& worldPos, mathUtils::Vec2& outPx) -> bool
	{
		const mathUtils::Vec4 clip = cameraViewProj * mathUtils::Vec4(worldPos, 1.0f);
		if (clip.w <= 1e-5f)
		{
			return false;
		}
		const float invW = 1.0f / clip.w;
		const float ndcX = clip.x * invW;
		const float ndcY = clip.y * invW;
		const float ndcZ = clip.z * invW;

		// RH + ZO: z is in [0..1] when visible
		if (ndcZ < 0.0f || ndcZ > 1.0f)
		{
			return false;
		}

		const float w = static_cast<float>(std::max(1u, scDesc.extent.width));
		const float h = static_cast<float>(std::max(1u, scDesc.extent.height));

		outPx.x = (ndcX * 0.5f + 0.5f) * w;
		outPx.y = (1.0f - (ndcY * 0.5f + 0.5f)) * h;

		// Drop very off-screen points (keeps things clean when gizmo is out of view)
		if (outPx.x < -200.0f || outPx.x >(w + 200.0f) || outPx.y < -200.0f || outPx.y >(h + 200.0f))
		{
			return false;
		}
		return true;
	};

auto AddAxisLabel = [&](const mathUtils::Vec3& startW, const mathUtils::Vec3& endW,
	std::string_view label, std::uint32_t rgba, float scale = 2.0f)
	{
		mathUtils::Vec2 startPx{};
		mathUtils::Vec2 endPx{};
		if (!ProjectWorldToScreenPx(startW, startPx) || !ProjectWorldToScreenPx(endW, endPx))
		{
			return;
		}

		mathUtils::Vec2 d = endPx - startPx;
		const float len = std::sqrt(d.x * d.x + d.y * d.y);
		if (len <= 1e-3f)
		{
			return;
		}
		d = d / len;

		// Smart offset:
		// - push further when axis projection is short (strong perspective / far away)
		// - add a perpendicular component so letters do not sit exactly on the line
		const float extraPad = std::clamp(120.0f / len, 0.0f, 24.0f);
		const float pad = 10.0f + extraPad;
		const float side = 6.0f + std::clamp(60.0f / len, 0.0f, 14.0f);
		const mathUtils::Vec2 perp{ -d.y, d.x };

		float sideSign = 1.0f;
		if (!label.empty())
		{
			// Stable separation between X/Y/Z without camera-dependent branching.
			// (Y tends to overlap in common camera angles, so give it the opposite side.)
			switch (label.front())
			{
			case 'Y':
			case 'y':
				sideSign = -1.0f;
				break;
			default:
				break;
			}
		}

		const float x = endPx.x + d.x * pad + perp.x * side * sideSign;
		const float y = endPx.y + d.y * pad + perp.y * side * sideSign;

		const float outlinePx = std::clamp(scale * 0.6f, 1.0f, 3.0f);
		textList.AddOutlinedTextAlignedPx(
			x, y, label,
			debugText::TextAlignH::Center, debugText::TextAlignV::Middle,
			rgba,
			debugText::PackRGBA8(0, 0, 0, 210),
			scale,
			outlinePx);
	};


auto AddPlaneLabel = [&](const mathUtils::Vec3& centerW, std::string_view label,
	std::uint32_t rgba, float scale = 1.75f)
	{
		mathUtils::Vec2 p{};
		if (!ProjectWorldToScreenPx(centerW, p))
		{
			return;
		}

		// For plane handles, keep the label close to the center but not exactly on it.
		const float x = p.x + 8.0f;
		const float y = p.y + 8.0f;

		const float outlinePx = std::clamp(scale * 0.6f, 1.0f, 3.0f);
		textList.AddOutlinedTextAlignedPx(
			x, y, label,
			debugText::TextAlignH::Left, debugText::TextAlignV::Top,
			rgba,
			debugText::PackRGBA8(0, 0, 0, 210),
			scale,
			outlinePx);
	};

auto AddAabbLines = [&](const mathUtils::Vec3& bmin,
	const mathUtils::Vec3& bmax,
	std::uint32_t rgba)
	{
		const mathUtils::Vec3 p000(bmin.x, bmin.y, bmin.z);
		const mathUtils::Vec3 p100(bmax.x, bmin.y, bmin.z);
		const mathUtils::Vec3 p110(bmax.x, bmax.y, bmin.z);
		const mathUtils::Vec3 p010(bmin.x, bmax.y, bmin.z);

		const mathUtils::Vec3 p001(bmin.x, bmin.y, bmax.z);
		const mathUtils::Vec3 p101(bmax.x, bmin.y, bmax.z);
		const mathUtils::Vec3 p111(bmax.x, bmax.y, bmax.z);
		const mathUtils::Vec3 p011(bmin.x, bmax.y, bmax.z);

		debugList.AddLine(p000, p100, rgba);
		debugList.AddLine(p100, p110, rgba);
		debugList.AddLine(p110, p010, rgba);
		debugList.AddLine(p010, p000, rgba);

		debugList.AddLine(p001, p101, rgba);
		debugList.AddLine(p101, p111, rgba);
		debugList.AddLine(p111, p011, rgba);
		debugList.AddLine(p011, p001, rgba);

		debugList.AddLine(p000, p001, rgba);
		debugList.AddLine(p100, p101, rgba);
		debugList.AddLine(p110, p111, rgba);
		debugList.AddLine(p010, p011, rgba);
	};

auto AddProbeCenterMarker = [&](const mathUtils::Vec3& p, float s, std::uint32_t rgba)
	{
		debugList.AddLine(
			mathUtils::Vec3(p.x - s, p.y, p.z),
			mathUtils::Vec3(p.x + s, p.y, p.z),
			rgba);

		debugList.AddLine(
			mathUtils::Vec3(p.x, p.y - s, p.z),
			mathUtils::Vec3(p.x, p.y + s, p.z),
			rgba);

		debugList.AddLine(
			mathUtils::Vec3(p.x, p.y, p.z - s),
			mathUtils::Vec3(p.x, p.y, p.z + s),
			rgba);
	};

auto ResolveGizmoAxisColor = [&](GizmoAxis activeAxis, GizmoAxis hoveredAxis, GizmoAxis axis, std::uint32_t baseColor) -> std::uint32_t
	{
		if (activeAxis == axis)
		{
			return debugDraw::PackRGBA8(255, 255, 255, 255);
		}
		if (hoveredAxis == axis)
		{
			return debugDraw::PackRGBA8(255, 255, 0, 255);
		}
		return baseColor;
	};

auto AddGizmoPlaneHandle = [&](const mathUtils::Vec3& pivot,
	float planeInner,
	float planeOuter,
	const mathUtils::Vec3& a,
	const mathUtils::Vec3& b,
	std::uint32_t color)
	{
		const mathUtils::Vec3 p00 = pivot + a * planeInner + b * planeInner;
		const mathUtils::Vec3 p10 = pivot + a * planeOuter + b * planeInner;
		const mathUtils::Vec3 p11 = pivot + a * planeOuter + b * planeOuter;
		const mathUtils::Vec3 p01 = pivot + a * planeInner + b * planeOuter;
		debugList.AddLine(p00, p10, color, true);
		debugList.AddLine(p10, p11, color, true);
		debugList.AddLine(p11, p01, color, true);
		debugList.AddLine(p01, p00, color, true);
	};

if (settings_.enableReflectionCapture)
{
	const float h = settings_.reflectionProbeBoxHalfExtent;
	const float markerSize = std::max(0.1f, h * 0.08f);

	for (std::size_t i = 0; i < reflectionProbes_.size(); ++i)
	{
		const auto& probe = reflectionProbes_[i];

		const mathUtils::Vec3 bmin(
			probe.capturePos.x - h,
			probe.capturePos.y - h,
			probe.capturePos.z - h);

		const mathUtils::Vec3 bmax(
			probe.capturePos.x + h,
			probe.capturePos.y + h,
			probe.capturePos.z + h);

		const bool validProbe = (probe.cube && probe.cubeDescIndex != 0);

		const std::uint32_t boxColor = validProbe ? 0xFF00FFFFu : 0xFF0000FFu;   // cyan / red
		const std::uint32_t centerColor = 0xFFFFFF00u;                            // yellow

		AddAabbLines(bmin, bmax, boxColor);
		AddProbeCenterMarker(probe.capturePos, markerSize, centerColor);
	}
}

if (settings_.drawLightGizmos)
{
	const float scale = settings_.debugLightGizmoScale;
	const float halfSize = settings_.lightGizmoHalfSize * scale;
	const float arrowLen = settings_.lightGizmoArrowLength * scale;
	const float axisLen = scene.editorTranslateGizmo.axisLengthWorld;

	for (const auto& light : scene.lights)
	{
		const std::uint32_t colDir = debugDraw::PackRGBA8(255, 255, 255, 255);
		const std::uint32_t colPoint = debugDraw::PackRGBA8(255, 220, 80, 255);
		const std::uint32_t colSpot = debugDraw::PackRGBA8(80, 220, 255, 255);

		switch (light.type)
		{
		case LightType::Directional:
		{
			const mathUtils::Vec3 dir = mathUtils::Normalize(light.direction);
			const mathUtils::Vec3 anchor = scene.camera.target;
			debugList.AddArrow(anchor, anchor + dir * arrowLen, colDir);
			break;
		}
		case LightType::Point:
		{
			const mathUtils::Vec3 p = light.position;
			debugList.AddLine(p - mathUtils::Vec3(halfSize, 0.0f, 0.0f), p + mathUtils::Vec3(halfSize, 0.0f, 0.0f), colPoint);
			debugList.AddLine(p - mathUtils::Vec3(0.0f, halfSize, 0.0f), p + mathUtils::Vec3(0.0f, halfSize, 0.0f), colPoint);
			debugList.AddLine(p - mathUtils::Vec3(0.0f, 0.0f, halfSize), p + mathUtils::Vec3(0.0f, 0.0f, halfSize), colPoint);

			debugList.AddArrow(
				p,
				p + mathUtils::Vec3(axisLen, 0.0f, 0.0f),
				ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis,
					scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::X, debugDraw::PackRGBA8(255, 80, 80, 255)), 0.25f, 0.15f, true);
			debugList.AddArrow(
				p,
				p + mathUtils::Vec3(0.0f, axisLen, 0.0f),
				ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis,
					scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::Y, debugDraw::PackRGBA8(80, 255, 80, 255)), 0.25f, 0.15f, true);
			debugList.AddArrow(
				p,
				p + mathUtils::Vec3(0.0f, 0.0f, axisLen),
				ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::Z,
					debugDraw::PackRGBA8(80, 160, 255, 255)), 0.25f, 0.15f, true);

			debugList.AddWireSphere(p, halfSize, colPoint, 16);
			break;
		}
		case LightType::Spot:
		{
			const mathUtils::Vec3 p = light.position;
			const mathUtils::Vec3 dir = mathUtils::Normalize(light.direction);
			debugList.AddArrow(p, p + dir * arrowLen, colSpot);
			const float outerRad = mathUtils::DegToRad(light.outerHalfAngleDeg);
			debugList.AddWireCone(p, dir, arrowLen, outerRad, colSpot, 24);

			debugList.AddArrow(
				p,
				p + mathUtils::Vec3(axisLen, 0.0f, 0.0f),
				ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis,
					scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::X, debugDraw::PackRGBA8(255, 80, 80, 255)), 0.25f, 0.15f, true);
			debugList.AddArrow(
				p,
				p + mathUtils::Vec3(0.0f, axisLen, 0.0f),
				ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis,
					scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::Y, debugDraw::PackRGBA8(80, 255, 80, 255)), 0.25f, 0.15f, true);
			debugList.AddArrow(
				p,
				p + mathUtils::Vec3(0.0f, 0.0f, axisLen),
				ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::Z,
					debugDraw::PackRGBA8(80, 160, 255, 255)), 0.25f, 0.15f, true);

			break;
		}
		default:
			break;
		}
	}
}

// Particle emitter editor visualization
if (!scene.particleEmitters.empty())
{
	for (std::size_t i = 0; i < scene.particleEmitters.size(); ++i)
	{
		const ParticleEmitter& emitter = scene.particleEmitters[i];
		const bool selected = scene.editorSelectedParticleEmitter == static_cast<int>(i);
		const std::uint32_t colMain = !emitter.enabled
			? debugDraw::PackRGBA8(140, 140, 140, 255)
			: (selected ? debugDraw::PackRGBA8(255, 220, 80, 255) : debugDraw::PackRGBA8(255, 140, 80, 255));
		const std::uint32_t colDir = selected ? debugDraw::PackRGBA8(255, 255, 255, 255) : debugDraw::PackRGBA8(80, 220, 255, 255);

		const mathUtils::Vec3 p = emitter.position;
		const float markerSize = std::clamp(mathUtils::Length(scene.camera.position - p) * 0.015f, 0.05f, 0.35f);
		debugList.AddAxesCross(p, markerSize, colMain, true);

		const mathUtils::Vec3 jitter = emitter.positionJitter;
		if (mathUtils::Length(jitter) > 1e-4f)
		{
			AddAabbLines(p - jitter, p + jitter, colMain);
		}

		mathUtils::Vec3 dir = emitter.velocityMin + emitter.velocityMax;
		const float dirLen = mathUtils::Length(dir);
		if (dirLen > 1e-4f)
		{
			dir = dir / dirLen;
			const float arrowLen = std::clamp(markerSize * 6.0f, 0.4f, 2.0f);
			debugList.AddArrow(p, p + dir * arrowLen, colDir, 0.18f, 0.10f, true);
		}

		int aliveCount = 0;
		for (const Particle& particle : scene.particles)
		{
			if (particle.alive && particle.ownerEmitter == static_cast<int>(i))
			{
				++aliveCount;
			}
		}

		mathUtils::Vec2 pPx{};
		if (ProjectWorldToScreenPx(p, pPx))
		{
			char label[256]{};
			const char* name = emitter.name.empty() ? "ParticleEmitter" : emitter.name.c_str();
			std::snprintf(label, sizeof(label), "%s  [%d/%u]", name, aliveCount, emitter.maxParticles);
			textList.AddOutlinedTextAlignedPx(
				pPx.x + 10.0f,
				pPx.y - 10.0f,
				label,
				debugText::TextAlignH::Left,
				debugText::TextAlignV::Bottom,
				colMain,
				debugText::PackRGBA8(0, 0, 0, 210),
				1.5f,
				1.5f);
		}
	}
}

if (scene.editorGizmoMode == GizmoMode::Translate && scene.editorTranslateGizmo.enabled && scene.editorTranslateGizmo.visible)
{
	const mathUtils::Vec3 pivot = scene.editorTranslateGizmo.pivotWorld;
	const float axisLen = scene.editorTranslateGizmo.axisLengthWorld;
	const float planeInner = axisLen * 0.28f;
	const float planeOuter = axisLen * 0.46f;

	debugList.AddArrow(
		pivot,
		pivot + mathUtils::Vec3(axisLen, 0.0f, 0.0f),
		ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis,
			scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::X, debugDraw::PackRGBA8(255, 80, 80, 255)), 0.25f, 0.15f, true);
	debugList.AddArrow(
		pivot,
		pivot + mathUtils::Vec3(0.0f, axisLen, 0.0f),
		ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis,
			scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::Y, debugDraw::PackRGBA8(80, 255, 80, 255)), 0.25f, 0.15f, true);
	debugList.AddArrow(
		pivot,
		pivot + mathUtils::Vec3(0.0f, 0.0f, axisLen),
		ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::Z,
			debugDraw::PackRGBA8(80, 160, 255, 255)), 0.25f, 0.15f, true);
	AddGizmoPlaneHandle(
		pivot,
		planeInner,
		planeOuter,
		mathUtils::Vec3(1.0f, 0.0f, 0.0f),
		mathUtils::Vec3(0.0f, 1.0f, 0.0f),
		ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::XY,
			debugDraw::PackRGBA8(255, 220, 80, 255)));
	AddGizmoPlaneHandle(
		pivot,
		planeInner,
		planeOuter,
		mathUtils::Vec3(1.0f, 0.0f, 0.0f),
		mathUtils::Vec3(0.0f, 0.0f, 1.0f),
		ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::XZ,
			debugDraw::PackRGBA8(255, 80, 255, 255)));
	AddGizmoPlaneHandle(
		pivot,
		planeInner,
		planeOuter,
		mathUtils::Vec3(0.0f, 1.0f, 0.0f),
		mathUtils::Vec3(0.0f, 0.0f, 1.0f),
		ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::YZ, debugDraw::PackRGBA8(80, 255, 255, 255)));

	// Axis/plane labels (screen-space text anchored to projected gizmo geometry)
	{
		const mathUtils::Vec3 xEnd = pivot + mathUtils::Vec3(axisLen, 0.0f, 0.0f);
		const mathUtils::Vec3 yEnd = pivot + mathUtils::Vec3(0.0f, axisLen, 0.0f);
		const mathUtils::Vec3 zEnd = pivot + mathUtils::Vec3(0.0f, 0.0f, axisLen);

		AddAxisLabel(
			pivot,
			xEnd,
			"X",
			ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::X,
				debugDraw::PackRGBA8(255, 80, 80, 255)));
		AddAxisLabel(
			pivot,
			yEnd,
			"Y",
			ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::Y,
				debugDraw::PackRGBA8(80, 255, 80, 255)));
		AddAxisLabel(
			pivot,
			zEnd,
			"Z",
			ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::Z,
				debugDraw::PackRGBA8(80, 160, 255, 255)));

		const float planeMid = (planeInner + planeOuter) * 0.5f;
		AddPlaneLabel(
			pivot + mathUtils::Vec3(planeMid, planeMid, 0.0f),
			"XY", ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis,
				scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::XY, debugDraw::PackRGBA8(255, 220, 80, 255)));
		AddPlaneLabel(
			pivot + mathUtils::Vec3(planeMid, 0.0f, planeMid),
			"XZ", ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis,
				scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::XZ, debugDraw::PackRGBA8(255, 80, 255, 255)));
		AddPlaneLabel(
			pivot + mathUtils::Vec3(0.0f, planeMid, planeMid),
			"YZ", ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis,
				scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::YZ, debugDraw::PackRGBA8(80, 255, 255, 255)));
	}
}

if (settings_.drawPlanarMirrorNormals)
{
	const std::uint32_t colPosN = debugDraw::PackRGBA8(80, 255, 120, 255);
	const std::uint32_t colNegN = debugDraw::PackRGBA8(255, 80, 80, 255);
	const std::uint32_t colOrigin = debugDraw::PackRGBA8(255, 220, 80, 255);

	const float normalLen = std::max(0.05f, settings_.planarMirrorNormalLength);
	const float originCross = std::max(0.01f, normalLen * 0.06f);

	for (const PlanarMirrorDraw& mirror : planarMirrorDraws)
	{
		mathUtils::Vec3 n = mirror.planeNormal;
		const float nLen = mathUtils::Length(n);
		if (nLen <= 1e-5f)
		{
			continue;
		}

		n = n / nLen;

		const mathUtils::Vec3 p = mirror.planePoint;
		const mathUtils::Vec3 pOff = p + n * 0.02f;

		debugList.AddAxesCross(pOff, originCross, colOrigin);
		debugList.AddArrow(pOff, pOff + n * normalLen, colPosN);
		debugList.AddArrow(pOff, pOff - n * (normalLen * 0.6f), colNegN);
	}
}

if (scene.editorGizmoMode == GizmoMode::Rotate && scene.editorRotateGizmo.enabled && scene.editorRotateGizmo.visible)
{
	const mathUtils::Vec3 pivot = scene.editorRotateGizmo.pivotWorld;
	const float ringRadius = scene.editorRotateGizmo.ringRadiusWorld;

	auto RingColor = [&](GizmoAxis axis, std::uint32_t baseColor) -> std::uint32_t
		{
			if (scene.editorRotateGizmo.activeAxis == axis)
			{
				return debugDraw::PackRGBA8(255, 255, 255, 255);
			}
			if (scene.editorRotateGizmo.hoveredAxis == axis)
			{
				return debugDraw::PackRGBA8(255, 255, 0, 255);
			}
			return baseColor;
		};

	debugList.AddWireCircle(
		pivot,
		scene.editorRotateGizmo.axisYWorld,
		scene.editorRotateGizmo.axisZWorld, ringRadius, RingColor(GizmoAxis::X, debugDraw::PackRGBA8(255, 80, 80, 255)), 64, true);
	debugList.AddWireCircle(
		pivot,
		scene.editorRotateGizmo.axisXWorld,
		scene.editorRotateGizmo.axisZWorld, ringRadius, RingColor(GizmoAxis::Y, debugDraw::PackRGBA8(80, 255, 80, 255)), 64, true);
	debugList.AddWireCircle(
		pivot,
		scene.editorRotateGizmo.axisXWorld,
		scene.editorRotateGizmo.axisYWorld, ringRadius, RingColor(GizmoAxis::Z, debugDraw::PackRGBA8(80, 160, 255, 255)), 64, true);

	// Axis labels (screen-space text anchored to a projected point on each ring)
	{
		auto SafeNormalizeOr = [&](const mathUtils::Vec3& v, const mathUtils::Vec3& fallback) -> mathUtils::Vec3
			{
				const float len = mathUtils::Length(v);
				if (len < 1e-5f)
				{
					return fallback;
				}
				return v * (1.0f / len);
			};

		const mathUtils::Vec3 camPos = scene.camera.position;
		const mathUtils::Vec3 camFwd = SafeNormalizeOr(scene.camera.target - scene.camera.position, mathUtils::Vec3(0.0f, 0.0f, -1.0f));
		mathUtils::Vec3 camRight = mathUtils::Cross(camFwd, scene.camera.up);
		camRight = SafeNormalizeOr(camRight, mathUtils::Vec3(1.0f, 0.0f, 0.0f));
		mathUtils::Vec3 camUp = mathUtils::Cross(camRight, camFwd);
		camUp = SafeNormalizeOr(camUp, mathUtils::Vec3(0.0f, 1.0f, 0.0f));

		auto AddRotateAxisLabel = [&](GizmoAxis axis, const mathUtils::Vec3& axisNWorld, std::string_view label, std::uint32_t baseColor, float textScale = 2.0f)
			{
				const std::uint32_t rgba = RingColor(axis, baseColor);

				// View direction from pivot towards the camera.
				const mathUtils::Vec3 viewDir = SafeNormalizeOr(camPos - pivot, mathUtils::Vec3(0.0f, 0.0f, 1.0f));
				const mathUtils::Vec3 n = SafeNormalizeOr(axisNWorld, mathUtils::Vec3(1.0f, 0.0f, 0.0f));

				// Pick a point on the ring that is most "facing" the camera: project viewDir onto the ring plane.
				mathUtils::Vec3 t = viewDir - n * mathUtils::Dot(viewDir, n);
				if (mathUtils::Length(t) < 1e-5f)
				{
					// If camera is nearly aligned with the axis, fall back to a stable basis.
					t = camRight - n * mathUtils::Dot(camRight, n);
					if (mathUtils::Length(t) < 1e-5f)
					{
						t = camUp - n * mathUtils::Dot(camUp, n);
					}
				}
				t = SafeNormalizeOr(t, camRight);

				const mathUtils::Vec3 ringPointW = pivot + t * ringRadius;

				mathUtils::Vec2 pivotPx{};
				mathUtils::Vec2 ringPx{};
				if (!ProjectWorldToScreenPx(pivot, pivotPx) || !ProjectWorldToScreenPx(ringPointW, ringPx))
				{
					return;
				}

				mathUtils::Vec2 out2D = ringPx - pivotPx;
				const float outLen = std::sqrt(out2D.x * out2D.x + out2D.y * out2D.y);
				if (outLen < 1e-3f)
				{
					return;
				}
				out2D = out2D / outLen;

				// Push the label slightly outside the ring; push more when the projected ring gets small.
				const float extraPad = std::clamp(140.0f / outLen, 0.0f, 28.0f);
				const float pad = 10.0f + extraPad;
				const float x = ringPx.x + out2D.x * pad;
				const float y = ringPx.y + out2D.y * pad;

				const float outlinePx = std::clamp(textScale * 0.6f, 1.0f, 3.0f);
				textList.AddOutlinedTextAlignedPx(
					x, y, label,
					debugText::TextAlignH::Center, debugText::TextAlignV::Middle,
					rgba,
					debugText::PackRGBA8(0, 0, 0, 210),
					textScale,
					outlinePx);
			};

		AddRotateAxisLabel(GizmoAxis::X, scene.editorRotateGizmo.axisXWorld, "X", debugDraw::PackRGBA8(255, 80, 80, 255));
		AddRotateAxisLabel(GizmoAxis::Y, scene.editorRotateGizmo.axisYWorld, "Y", debugDraw::PackRGBA8(80, 255, 80, 255));
		AddRotateAxisLabel(GizmoAxis::Z, scene.editorRotateGizmo.axisZWorld, "Z", debugDraw::PackRGBA8(80, 160, 255, 255));
	}
}
if (scene.editorGizmoMode == GizmoMode::Scale && scene.editorScaleGizmo.enabled && scene.editorScaleGizmo.visible)
{
	const mathUtils::Vec3 pivot = scene.editorScaleGizmo.pivotWorld;
	const float axisLen = scene.editorScaleGizmo.axisLengthWorld;
	const float handleHalf = std::max(axisLen * 0.08f, 0.03f);
	const float planeInner = axisLen * 0.24f;
	const float planeOuter = axisLen * 0.40f;

	auto AddScaleHandle = [&](GizmoAxis axis, const mathUtils::Vec3& dir, std::uint32_t baseColor)
		{
			const std::uint32_t color = ResolveGizmoAxisColor(scene.editorScaleGizmo.activeAxis, scene.editorScaleGizmo.hoveredAxis, axis, baseColor);
			const mathUtils::Vec3 end = pivot + dir * axisLen;

			debugList.AddLine(pivot, end, color, true);
			debugList.AddAxesCross(end, handleHalf, color, true);
		};

	AddGizmoPlaneHandle(
		pivot,
		planeInner,
		planeOuter,
		scene.editorScaleGizmo.axisXWorld, scene.editorScaleGizmo.axisYWorld,
		ResolveGizmoAxisColor(scene.editorScaleGizmo.activeAxis, scene.editorScaleGizmo.hoveredAxis, GizmoAxis::XY,
			debugDraw::PackRGBA8(255, 220, 80, 255)));
	AddGizmoPlaneHandle(
		pivot,
		planeInner,
		planeOuter,
		scene.editorScaleGizmo.axisXWorld, scene.editorScaleGizmo.axisZWorld,
		ResolveGizmoAxisColor(scene.editorScaleGizmo.activeAxis, scene.editorScaleGizmo.hoveredAxis, GizmoAxis::XZ,
			debugDraw::PackRGBA8(255, 80, 255, 255)));
	AddGizmoPlaneHandle(
		pivot,
		planeInner,
		planeOuter,
		scene.editorScaleGizmo.axisYWorld, scene.editorScaleGizmo.axisZWorld,
		ResolveGizmoAxisColor(scene.editorScaleGizmo.activeAxis, scene.editorScaleGizmo.hoveredAxis, GizmoAxis::YZ,
			debugDraw::PackRGBA8(80, 255, 255, 255)));
	AddScaleHandle(GizmoAxis::X, scene.editorScaleGizmo.axisXWorld, debugDraw::PackRGBA8(255, 80, 80, 255));
	AddScaleHandle(GizmoAxis::Y, scene.editorScaleGizmo.axisYWorld, debugDraw::PackRGBA8(80, 255, 80, 255));
	AddScaleHandle(GizmoAxis::Z, scene.editorScaleGizmo.axisZWorld, debugDraw::PackRGBA8(80, 160, 255, 255));
	debugList.AddWireSphere(
		pivot,
		scene.editorScaleGizmo.uniformHandleRadiusWorld,
		ResolveGizmoAxisColor(scene.editorScaleGizmo.activeAxis, scene.editorScaleGizmo.hoveredAxis, GizmoAxis::XYZ, debugDraw::PackRGBA8(230, 230, 230, 255)),
		16,
		true);

	// Axis/plane labels (screen-space text anchored to projected gizmo geometry)
	{
		const mathUtils::Vec3 xEnd = pivot + scene.editorScaleGizmo.axisXWorld * axisLen;
		const mathUtils::Vec3 yEnd = pivot + scene.editorScaleGizmo.axisYWorld * axisLen;
		const mathUtils::Vec3 zEnd = pivot + scene.editorScaleGizmo.axisZWorld * axisLen;

		AddAxisLabel(pivot, xEnd, "X", ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::X, debugDraw::PackRGBA8(255, 80, 80, 255)));
		AddAxisLabel(pivot, yEnd, "Y", ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::Y, debugDraw::PackRGBA8(80, 255, 80, 255)));
		AddAxisLabel(pivot, zEnd, "Z", ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::Z, debugDraw::PackRGBA8(80, 160, 255, 255)));

		const float planeMid = (planeInner + planeOuter) * 0.5f;
		AddPlaneLabel(pivot + scene.editorScaleGizmo.axisXWorld * planeMid + scene.editorScaleGizmo.axisYWorld * planeMid, "XY", ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::XY, debugDraw::PackRGBA8(255, 220, 80, 255)));
		AddPlaneLabel(pivot + scene.editorScaleGizmo.axisXWorld * planeMid + scene.editorScaleGizmo.axisZWorld * planeMid, "XZ", ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::XZ, debugDraw::PackRGBA8(255, 80, 255, 255)));
		AddPlaneLabel(pivot + scene.editorScaleGizmo.axisYWorld * planeMid + scene.editorScaleGizmo.axisZWorld * planeMid, "YZ", ResolveGizmoAxisColor(scene.editorTranslateGizmo.activeAxis, scene.editorTranslateGizmo.hoveredAxis, GizmoAxis::YZ, debugDraw::PackRGBA8(80, 255, 255, 255)));
	}
}

// Pick ray (from the editor UI) visualized in the main view via DebugDraw.
if (scene.debugPickRay.enabled)
{
	const std::uint32_t colHit = debugDraw::PackRGBA8(80, 255, 80, 255);
	const std::uint32_t colMiss = debugDraw::PackRGBA8(255, 80, 80, 255);
	const std::uint32_t col = scene.debugPickRay.hit ? colHit : colMiss;

	mathUtils::Vec3 dir = scene.debugPickRay.direction;
	const float dirLen = mathUtils::Length(dir);
	if (dirLen > 1e-5f)
	{
		dir = dir / dirLen;
	}
	else
	{
		dir = mathUtils::Vec3(0.0f, 0.0f, 1.0f);
	}

	const mathUtils::Vec3 a = scene.debugPickRay.origin;
	const mathUtils::Vec3 b = a + dir * scene.debugPickRay.length;
	debugList.AddLine(a, b, col);
	if (scene.debugPickRay.hit)
	{
		const float cross = settings_.lightGizmoHalfSize * 0.25f;
		debugList.AddAxesCross(b, cross, col);
	}
}

if (settings_.ShowCubeAtlas)
{
	// Debug: visualize a cubemap as a 3x2 atlas inset in the main view (bottom-right)
	//
	// Priority:
	//  1) First point shadow cube (distance map, grayscale)
	//  2) Reflection capture cube (color), even if there is no skybox (owner selected by debugCubeAtlasIndex in reflection mode)
	std::optional<renderGraph::RGTexture> debugCubeRG{};
	float debugInvRange = 1.0f;
	std::uint32_t debugInvert = 1u;
	std::uint32_t debugMode = 0u; // 0 = depth grayscale, 1 = color

	if (settings_.debugShadowCubeMapType == 0 && !pointShadows.empty())
	{
		const std::uint32_t maxIdx = static_cast<std::uint32_t>(pointShadows.size() - 1u);
		const std::uint32_t idx = std::min(settings_.debugCubeAtlasIndex, maxIdx);
		debugCubeRG = pointShadows[idx].cube;
		// Point shadow map stores normalized distance [0..1] by default.
		debugInvRange = 1.0f;
		debugInvert = 1u;
		debugMode = 0u;
	}
	else if (settings_.debugShadowCubeMapType == 1 && settings_.enableReflectionCapture)
	{
		rhi::TextureHandle debugReflectionCube{};
		if (!reflectionProbes_.empty())
		{
			const std::uint32_t maxIdx = static_cast<std::uint32_t>(reflectionProbes_.size() - 1u);
			const std::uint32_t idx = std::min(settings_.debugCubeAtlasIndex, maxIdx);
			const ReflectionProbeRuntime& probe = reflectionProbes_[idx];
			if (probe.cube)
			{
				debugReflectionCube = probe.cube;
			}
		}
		else if (reflectionCube_)
		{
			debugReflectionCube = reflectionCube_;
		}

		if (debugReflectionCube)
		{
			debugCubeRG = graph.ImportTexture(debugReflectionCube, renderGraph::RGTextureDesc{
				.extent = reflectionCubeExtent_,
				.format = rhi::Format::RGBA8_UNORM,
				.usage = renderGraph::ResourceUsage::Sampled,
				.type = renderGraph::TextureType::Cube,
				.debugName = "ReflectionCaptureCube_Debug"
				});
			debugInvRange = 1.0f;
			debugInvert = 0u;
			debugMode = 1u;
		}
	}

	if (debugCubeRG && psoDebugCubeAtlas_ && debugCubeAtlasLayout_ && debugCubeAtlasVB_)
	{
		rhi::ClearDesc clear{};
		clear.clearColor = false;
		clear.clearDepth = false;

		const auto cubeRG = *debugCubeRG;

		graph.AddSwapChainPass("DebugPointShadowAtlas", clear,
			[this, cubeRG, debugInvRange, debugInvert, debugMode](renderGraph::PassContext& ctx)
			{
				struct alignas(16) DebugCubeAtlasCB
				{
					float uInvRange;
					float uGamma;
					std::uint32_t uInvert;
					std::uint32_t uShowGrid;
					std::uint32_t uMode;
					std::uint32_t _pad0;
					float uViewportOriginX;
					float uViewportOriginY;
					float uInvViewportSizeX;
					float uInvViewportSizeY;
					float _pad1;
					float _pad2;
				};

				DebugCubeAtlasCB cb{};
				cb.uInvRange = debugInvRange;
				cb.uGamma = 1.0f;
				cb.uInvert = debugInvert;

				cb.uShowGrid = 1u;
				cb.uMode = debugMode;
				cb._pad0 = 0u;

				const std::uint32_t W = std::max(1u, ctx.passExtent.width);
				const std::uint32_t H = std::max(1u, ctx.passExtent.height);
				const std::uint32_t margin = 16u;

				// Keep 3:2 aspect (3 tiles wide, 2 tiles tall)
				std::uint32_t insetW = std::min(512u, (W > margin * 2u) ? (W - margin * 2u) : 128u);
				insetW = std::max(128u, insetW);
				std::uint32_t insetH = (insetW * 2u) / 3u;
				if (insetH + margin * 2u > H)
				{
					insetH = (H > margin * 2u) ? (H - margin * 2u) : 128u;
					insetW = (insetH * 3u) / 2u;
				}

				const std::uint32_t x0 = (W > (margin + insetW)) ? (W - margin - insetW) : 0u;
				const std::uint32_t y0 = (H > (margin + insetH)) ? (H - margin - insetH) : 0u;

				cb.uViewportOriginX = float(x0);
				cb.uViewportOriginY = float(y0);

				cb.uInvViewportSizeX = 1.0f / float(std::max(1u, insetW));
				cb.uInvViewportSizeY = 1.0f / float(std::max(1u, insetH));
				cb._pad1 = 0.0f;
				cb._pad2 = 0.0f;

				ctx.commandList.SetViewport(
					static_cast<int>(x0), static_cast<int>(y0),
					static_cast<int>(insetW), static_cast<int>(insetH));

				ctx.commandList.SetState(debugCubeAtlasState_);
				ctx.commandList.BindPipeline(psoDebugCubeAtlas_);
				ctx.commandList.BindInputLayout(debugCubeAtlasLayout_);
				ctx.commandList.BindVertexBuffer(0, debugCubeAtlasVB_, debugCubeAtlasVBStrideBytes_, 0);
				ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

				const auto tex = ctx.resources.GetTexture(cubeRG);
				ctx.commandList.BindTexture2DArray(0, tex); // t0 (Texture2DArray<float4>)

				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &cb, 1 })); // b0
				ctx.commandList.Draw(3);

				// Restore full viewport for any following swapchain passes.
				ctx.commandList.SetViewport(0, 0, static_cast<int>(W), static_cast<int>(H));
			});
	}
}

if (settings_.loadingOverlayVisible)
{
	const std::uint32_t backbufferW = std::max(1u, scDesc.extent.width);
	const std::uint32_t backbufferH = std::max(1u, scDesc.extent.height);
	const float pxToNdcX = 2.0f / static_cast<float>(backbufferW);
	const float pxToNdcY = 2.0f / static_cast<float>(backbufferH);
	const float progressOfBar = std::clamp(settings_.loadingOverlayProgressBar, 0.0f, 1.0f);

	const float barWidthNdc = std::clamp(520.0f * pxToNdcX, 0.42f, 0.82f);
	const float barHeightNdc = std::clamp(24.0f * pxToNdcY, 0.035f, 0.09f);
	const float borderPadNdcX = std::max(2.0f * pxToNdcX, 0.004f);
	const float borderPadNdcY = std::max(2.0f * pxToNdcY, 0.004f);
	const float x0 = -barWidthNdc * 0.5f;
	const float x1 = barWidthNdc * 0.5f;
	const float centerY = -0.80f;
	const float y0 = centerY - barHeightNdc * 0.5f;
	const float y1 = centerY + barHeightNdc * 0.5f;

	const float innerX0 = x0 + borderPadNdcX;
	const float innerX1 = x1 - borderPadNdcX;
	const float innerY0 = y0 + borderPadNdcY;
	const float innerY1 = y1 - borderPadNdcY;
	const float fillX1 = std::lerp(innerX0, innerX1, progressOfBar);

	const std::uint32_t colFrame = debugDraw::PackRGBA8(235, 235, 235, 255);
	const std::uint32_t colBg = debugDraw::PackRGBA8(18, 24, 32, 255);
	const std::uint32_t colFill = debugDraw::PackRGBA8(80, 220, 255, 255);
	const std::uint32_t colTick = debugDraw::PackRGBA8(255, 255, 255, 255);

	// Use pixel-aligned fills to avoid "black stripes" caused by line rasterization gaps.
	debugList.AddScreenSpaceFilledRectNdcPixelAligned(innerX0, innerY0, innerX1, innerY1, colBg, pxToNdcX);
	if (fillX1 > innerX0)
	{
		debugList.AddScreenSpaceFilledRectNdcPixelAligned(innerX0, innerY0, fillX1, innerY1, colFill, pxToNdcX);
	}

	debugList.AddScreenSpaceRectNdc(x0, y0, x1, y1, colFrame);
	debugList.AddScreenSpaceRectNdc(innerX0, innerY0, innerX1, innerY1, colFrame);

	const std::uint32_t totalUnits = settings_.loadingOverlayTotalUnits;
	if (totalUnits > 1u)
	{
		std::uint32_t tickEvery = 1u;
		constexpr std::uint32_t kMaxTicks = 24u;
		while ((totalUnits / tickEvery) > kMaxTicks)
		{
			tickEvery *= 2u;
		}

		for (std::uint32_t t = tickEvery; t < totalUnits; t += tickEvery)
		{
			const float frac = static_cast<float>(t) / static_cast<float>(totalUnits);
			const float xt = std::lerp(innerX0, innerX1, frac);
			debugList.AddScreenSpaceLineNdc(
				mathUtils::Vec3(xt, innerY0, 0.0f),
				mathUtils::Vec3(xt, innerY1, 0.0f),
				colTick);
		}
	}
}

if (!textList.Empty())
{
	debugTextRenderer_.Upload(textList);
}

debugDrawRenderer_.Upload(debugList);
if (debugList.VertexCount() > 0)
{
	rhi::ClearDesc clear{};
	clear.clearColor = false;
	clear.clearDepth = false;

	const FrameCameraData camera = BuildFrameCameraData(scene, scDesc.extent);
	const mathUtils::Mat4 viewProj = camera.viewProj;

	graph.AddSwapChainPass("DebugPrimitivesPass", clear, [this, viewProj](renderGraph::PassContext& ctx)
		{
			debugDrawRenderer_.Draw(ctx.commandList, viewProj, settings_.debugDrawDepthTest);
		});
}

if (!textList.Empty())
{
	rhi::ClearDesc clear{};
	clear.clearColor = false;
	clear.clearDepth = false;

	graph.AddSwapChainPass("DebugTextPass", clear, [this](renderGraph::PassContext& ctx)
		{
			debugTextRenderer_.Draw(ctx.commandList, ctx.passExtent.width, ctx.passExtent.height);
		});
}

graph.Execute(device_, swapChain);
swapChain.Present();