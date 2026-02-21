
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
						
						
						// --- Debug: visualize first point shadow cubemap as 3x2 atlas on the swapchain ---
						// NOTE: This pass overwrites the swapchain. Comment it out when not needed.
						if (settings_.ShowCubeAtlas && !pointShadows.empty() && psoDebugCubeAtlas_ && debugCubeAtlasLayout_ && debugCubeAtlasVB_)
						{
							rhi::ClearDesc clear{};
							clear.clearColor = false;
							clear.clearDepth = false;
						
							const float invRange = 1.0f; // point shadow stores normalized dist [0..1]. If storing world dist, set to 1/range.
							struct alignas(16) DebugCubeAtlasCB { float uInvRange; float uGamma; std::uint32_t uInvert; std::uint32_t uShowGrid; };
							const DebugCubeAtlasCB cb{ invRange, 1.0f, 1u, 1u };
						
							const auto cubeRG = pointShadows[0].cube;

							graph.AddSwapChainPass("DebugPointShadowAtlas", clear,
								[this, cubeRG](renderGraph::PassContext& ctx)
								{
									struct alignas(16) DebugCubeAtlasCB
									{
										float uInvRange;
										float uGamma;         // 1.0
										std::uint32_t uInvert;
										std::uint32_t uShowGrid;
										float uInvViewportX;  // 1/width
										float uInvViewportY;  // 1/height
										float _pad0;
										float _pad1;
									};

									DebugCubeAtlasCB cb{};
									cb.uInvRange = 20.0f;
									cb.uGamma = 1.0f;
									cb.uInvert = 1u;
									cb.uShowGrid = 1u;
									cb.uInvViewportX = 1.0f / float(std::max(1u, ctx.passExtent.width));
									cb.uInvViewportY = 1.0f / float(std::max(1u, ctx.passExtent.height));

									ctx.commandList.SetViewport(
										0, 0,
										static_cast<int>(ctx.passExtent.width),
										static_cast<int>(ctx.passExtent.height));

									ctx.commandList.SetState(debugCubeAtlasState_);
									ctx.commandList.BindPipeline(psoDebugCubeAtlas_);
									ctx.commandList.BindInputLayout(debugCubeAtlasLayout_);
									ctx.commandList.BindVertexBuffer(0, debugCubeAtlasVB_, debugCubeAtlasVBStrideBytes_, 0);
									ctx.commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

									const auto tex = ctx.resources.GetTexture(cubeRG);
									ctx.commandList.BindTextureCube(0, tex); // t0

									ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &cb, 1 })); // b0
									ctx.commandList.Draw(3);
								});
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