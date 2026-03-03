
// Debug primitives (no ImGui dependency) - rendered in the main view.
debugDraw::DebugDrawList debugList;
if (settings_.drawLightGizmos)
{
	const float scale = settings_.debugLightGizmoScale;
	const float halfSize = settings_.lightGizmoHalfSize * scale;
	const float arrowLen = settings_.lightGizmoArrowLength * scale;

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
			break;
		}
		default:
			break;
		}
	}
}

if (scene.editorGizmoMode == GizmoMode::Translate && scene.editorTranslateGizmo.enabled && scene.editorTranslateGizmo.visible)
{
	const mathUtils::Vec3 pivot = scene.editorTranslateGizmo.pivotWorld;
	const float axisLen = scene.editorTranslateGizmo.axisLengthWorld;
	const float planeInner = axisLen * 0.28f;
	const float planeOuter = axisLen * 0.46f;

	auto AxisColor = [&](GizmoAxis axis, std::uint32_t baseColor) -> std::uint32_t
		{
			if (scene.editorTranslateGizmo.activeAxis == axis)
			{
				return debugDraw::PackRGBA8(255, 255, 255, 255);
			}
			if (scene.editorTranslateGizmo.hoveredAxis == axis)
			{
				return debugDraw::PackRGBA8(255, 255, 0, 255);
			}
			return baseColor;
		};

	auto AddPlaneHandle = [&](GizmoAxis axis, const mathUtils::Vec3& a, const mathUtils::Vec3& b, std::uint32_t baseColor)
		{
			const std::uint32_t color = AxisColor(axis, baseColor);
			const mathUtils::Vec3 p00 = pivot + a * planeInner + b * planeInner;
			const mathUtils::Vec3 p10 = pivot + a * planeOuter + b * planeInner;
			const mathUtils::Vec3 p11 = pivot + a * planeOuter + b * planeOuter;
			const mathUtils::Vec3 p01 = pivot + a * planeInner + b * planeOuter;
			debugList.AddLine(p00, p10, color, true);
			debugList.AddLine(p10, p11, color, true);
			debugList.AddLine(p11, p01, color, true);
			debugList.AddLine(p01, p00, color, true);
		};

	debugList.AddArrow(pivot, pivot + mathUtils::Vec3(axisLen, 0.0f, 0.0f), AxisColor(GizmoAxis::X, debugDraw::PackRGBA8(255, 80, 80, 255)), 0.25f, 0.15f, true);
	debugList.AddArrow(pivot, pivot + mathUtils::Vec3(0.0f, axisLen, 0.0f), AxisColor(GizmoAxis::Y, debugDraw::PackRGBA8(80, 255, 80, 255)), 0.25f, 0.15f, true);
	debugList.AddArrow(pivot, pivot + mathUtils::Vec3(0.0f, 0.0f, axisLen), AxisColor(GizmoAxis::Z, debugDraw::PackRGBA8(80, 160, 255, 255)), 0.25f, 0.15f, true);
	AddPlaneHandle(GizmoAxis::XY, mathUtils::Vec3(1.0f, 0.0f, 0.0f), mathUtils::Vec3(0.0f, 1.0f, 0.0f), debugDraw::PackRGBA8(255, 220, 80, 255));
	AddPlaneHandle(GizmoAxis::XZ, mathUtils::Vec3(1.0f, 0.0f, 0.0f), mathUtils::Vec3(0.0f, 0.0f, 1.0f), debugDraw::PackRGBA8(255, 80, 255, 255));
	AddPlaneHandle(GizmoAxis::YZ, mathUtils::Vec3(0.0f, 1.0f, 0.0f), mathUtils::Vec3(0.0f, 0.0f, 1.0f), debugDraw::PackRGBA8(80, 255, 255, 255));
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

	debugList.AddWireCircle(pivot, scene.editorRotateGizmo.axisYWorld, scene.editorRotateGizmo.axisZWorld, ringRadius, RingColor(GizmoAxis::X, debugDraw::PackRGBA8(255, 80, 80, 255)), 64, true);
	debugList.AddWireCircle(pivot, scene.editorRotateGizmo.axisXWorld, scene.editorRotateGizmo.axisZWorld, ringRadius, RingColor(GizmoAxis::Y, debugDraw::PackRGBA8(80, 255, 80, 255)), 64, true);
	debugList.AddWireCircle(pivot, scene.editorRotateGizmo.axisXWorld, scene.editorRotateGizmo.axisYWorld, ringRadius, RingColor(GizmoAxis::Z, debugDraw::PackRGBA8(80, 160, 255, 255)), 64, true);
}
if (scene.editorGizmoMode == GizmoMode::Scale && scene.editorScaleGizmo.enabled && scene.editorScaleGizmo.visible)
{
	const mathUtils::Vec3 pivot = scene.editorScaleGizmo.pivotWorld;
	const float axisLen = scene.editorScaleGizmo.axisLengthWorld;
	const float handleHalf = std::max(axisLen * 0.08f, 0.03f);
	const float planeInner = axisLen * 0.24f;
	const float planeOuter = axisLen * 0.40f;

	auto AxisColor = [&](GizmoAxis axis, std::uint32_t baseColor) -> std::uint32_t
		{
			if (scene.editorScaleGizmo.activeAxis == axis)
			{
				return debugDraw::PackRGBA8(255, 255, 255, 255);
			}
			if (scene.editorScaleGizmo.hoveredAxis == axis)
			{
				return debugDraw::PackRGBA8(255, 255, 0, 255);
			}
			return baseColor;
		};

	auto AddPlaneHandle = [&](GizmoAxis axis, const mathUtils::Vec3& a, const mathUtils::Vec3& b, std::uint32_t baseColor)
		{
			const std::uint32_t color = AxisColor(axis, baseColor);
			const mathUtils::Vec3 p00 = pivot + a * planeInner + b * planeInner;
			const mathUtils::Vec3 p10 = pivot + a * planeOuter + b * planeInner;
			const mathUtils::Vec3 p11 = pivot + a * planeOuter + b * planeOuter;
			const mathUtils::Vec3 p01 = pivot + a * planeInner + b * planeOuter;
			debugList.AddLine(p00, p10, color, true);
			debugList.AddLine(p10, p11, color, true);
			debugList.AddLine(p11, p01, color, true);
			debugList.AddLine(p01, p00, color, true);
		};

	auto AddScaleHandle = [&](GizmoAxis axis, const mathUtils::Vec3& dir, std::uint32_t baseColor)
		{
			const std::uint32_t color = AxisColor(axis, baseColor);
			const mathUtils::Vec3 end = pivot + dir * axisLen;

			debugList.AddLine(pivot, end, color, true);
			debugList.AddAxesCross(end, handleHalf, color, true);
		};

	AddPlaneHandle(GizmoAxis::XY, scene.editorScaleGizmo.axisXWorld, scene.editorScaleGizmo.axisYWorld, debugDraw::PackRGBA8(255, 220, 80, 255));
	AddPlaneHandle(GizmoAxis::XZ, scene.editorScaleGizmo.axisXWorld, scene.editorScaleGizmo.axisZWorld, debugDraw::PackRGBA8(255, 80, 255, 255));
	AddPlaneHandle(GizmoAxis::YZ, scene.editorScaleGizmo.axisYWorld, scene.editorScaleGizmo.axisZWorld, debugDraw::PackRGBA8(80, 255, 255, 255));
	AddScaleHandle(GizmoAxis::X, scene.editorScaleGizmo.axisXWorld, debugDraw::PackRGBA8(255, 80, 80, 255));
	AddScaleHandle(GizmoAxis::Y, scene.editorScaleGizmo.axisYWorld, debugDraw::PackRGBA8(80, 255, 80, 255));
	AddScaleHandle(GizmoAxis::Z, scene.editorScaleGizmo.axisZWorld, debugDraw::PackRGBA8(80, 160, 255, 255));
	debugList.AddWireSphere(
		pivot,
		scene.editorScaleGizmo.uniformHandleRadiusWorld,
		AxisColor(GizmoAxis::XYZ, debugDraw::PackRGBA8(230, 230, 230, 255)),
		16,
		true);
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

debugDrawRenderer_.Upload(debugList);
if (debugList.VertexCount() > 0)
{
	rhi::ClearDesc clear{};
	clear.clearColor = false;
	clear.clearDepth = false;

	const mathUtils::Mat4 proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
	const mathUtils::Mat4 view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
	const mathUtils::Mat4 viewProj = proj * view;

	graph.AddSwapChainPass("DebugPrimitivesPass", clear, [this, viewProj](renderGraph::PassContext& ctx)
		{
			debugDrawRenderer_.Draw(ctx.commandList, viewProj, settings_.debugDrawDepthTest);
		});
}

graph.Execute(device_, swapChain);
swapChain.Present();