			// ---------------- Create shadow passes (all reuse shadowBatches) ----------------
			// Directional CSM atlas (depth-only). We clear the whole atlas once, then render each cascade
			// into its own 2048x2048 viewport tile.
			for (std::uint32_t cascade = 0; cascade < dirCascadeCount; ++cascade)
			{
				rhi::ClearDesc clear{};
				clear.clearColor = false;
				clear.clearDepth = (cascade == 0u);
				clear.depth = 1.0f;

				renderGraph::PassAttachments att{};
				att.useSwapChainBackbuffer = false;
				att.color = std::nullopt;
				att.depth = shadowRG;
				att.clearDesc = clear;

				// Pre-pack constants once (per pass)
				struct alignas(16) ShadowPassConstants
				{
					std::array<float, 16> uLightViewProj{};
				};

				ShadowPassConstants shadowPassConstants{};
				const mathUtils::Mat4 vpT = mathUtils::Transpose(dirCascadeVP[cascade]);
				std::memcpy(shadowPassConstants.uLightViewProj.data(), mathUtils::ValuePtr(vpT), sizeof(float) * 16);

				const int vpX = static_cast<int>(cascade * dirTileSize);
				const int vpY = 0;
				const int vpW = static_cast<int>(dirTileSize);
				const int vpH = static_cast<int>(dirTileSize);

				const char* passName = (cascade == 0u) ? "DirShadow_C0" : (cascade == 1u) ? "DirShadow_C1" : "DirShadow_C2";
				graph.AddPass(passName, std::move(att),
					[this, shadowPassConstants, shadowBatches, instStride, vpX, vpY, vpW, vpH](renderGraph::PassContext& ctx) mutable
					{
						ctx.commandList.SetViewport(vpX, vpY, vpW, vpH);

						ctx.commandList.SetState(shadowState_);
						ctx.commandList.BindPipeline(psoShadow_);

						ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &shadowPassConstants, 1 }));

						this->DrawInstancedShadowBatches(ctx.commandList, shadowBatches, instStride);

					});
			}

			// Collect up to kMaxSpotShadows / kMaxPointShadows from scene.lights (index aligns with UploadLights()).
			for (std::uint32_t lightIndex = 0; lightIndex < static_cast<std::uint32_t>(scene.lights.size()); ++lightIndex)
			{
				if (lightIndex >= kMaxLights)
				{
					break;
				}

				const auto& light = scene.lights[lightIndex];

				if (light.type == LightType::Spot && spotShadows.size() < kMaxSpotShadows)
				{
					const rhi::Extent2D ext{ 1024, 1024 };
					const auto rg = graph.CreateTexture(renderGraph::RGTextureDesc{
						.extent = ext,
						.format = rhi::Format::D32_FLOAT,
						.usage = renderGraph::ResourceUsage::DepthStencil,
						.debugName = "SpotShadowMap"
						});

					mathUtils::Vec3 lightDirLocal = mathUtils::Normalize(light.direction);
					mathUtils::Vec3 upVector = (std::abs(mathUtils::Dot(lightDirLocal, mathUtils::Vec3(0, 1, 0))) > 0.99f)
						? mathUtils::Vec3(0, 0, 1)
						: mathUtils::Vec3(0, 1, 0);

					mathUtils::Mat4 lightView = mathUtils::LookAt(light.position, light.position + lightDirLocal, upVector);

					const float outerHalf = std::max(1.0f, light.outerHalfAngleDeg);
					const float farZ = std::max(1.0f, light.range);
					const float nearZ = std::max(0.5f, farZ * 0.02f);
					const mathUtils::Mat4 lightProj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(outerHalf * 2.0f), 1.0f, nearZ, farZ);
					const mathUtils::Mat4 lightViewProj = lightProj * lightView;

					SpotShadowRec rec{};
					rec.tex = rg;
					rec.viewProj = lightViewProj;
					rec.lightIndex = lightIndex;
					spotShadows.push_back(rec);

					rhi::ClearDesc clear{};
					clear.clearColor = false;
					clear.clearDepth = true;
					clear.depth = 1.0f;

					renderGraph::PassAttachments att{};
					att.useSwapChainBackbuffer = false;
					att.color = std::nullopt;
					att.depth = rg;
					att.clearDesc = clear;

					const std::string passName = "SpotShadowPass_" + std::to_string(static_cast<int>(spotShadows.size() - 1));

					// Pre-pack constants once (per pass)
					struct alignas(16) SpotPassConstants
					{
						std::array<float, 16> uLightViewProj{};
					};
					SpotPassConstants spotPassConstants{};
					const mathUtils::Mat4 lightViewProjTranspose = mathUtils::Transpose(lightViewProj);
					std::memcpy(spotPassConstants.uLightViewProj.data(), mathUtils::ValuePtr(lightViewProjTranspose), sizeof(float) * 16);

					graph.AddPass(passName, std::move(att),
						[this, spotPassConstants, shadowBatches, instStride](renderGraph::PassContext& ctx) mutable
						{
							ctx.commandList.SetViewport(0, 0,
								static_cast<int>(ctx.passExtent.width),
								static_cast<int>(ctx.passExtent.height));

							ctx.commandList.SetState(shadowState_);
							ctx.commandList.BindPipeline(psoShadow_);

							ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &spotPassConstants, 1 }));

							this->DrawInstancedShadowBatches(ctx.commandList, shadowBatches, instStride);

						});
				}
				else if (light.type == LightType::Point && pointShadows.size() < kMaxPointShadows)
				{
					// Point shadows use a cubemap R32_FLOAT distance map (color) + depth for rasterization.
					// Prefer layered one-pass (SV_RenderTargetArrayIndex). If unavailable, try VI (SV_ViewID).
					// Otherwise we fall back to 6 separate passes (face-by-face).
					bool useLayered =
						(!disablePointShadowLayered_) &&
						static_cast<bool>(psoPointShadowLayered_) &&
						device_.SupportsVPAndRTArrayIndexFromAnyShader();

					bool useVI = (!disablePointShadowVI_) && static_cast<bool>(psoPointShadowVI_);

					const rhi::Extent2D cubeExtent{ 2048, 2048 };
					const auto cube = graph.CreateTexture(renderGraph::RGTextureDesc{
						.extent = cubeExtent,
						.format = rhi::Format::R32_FLOAT,
						.usage = renderGraph::ResourceUsage::RenderTarget,
						.type = renderGraph::TextureType::Cube,
						.debugName = "PointShadowCube"
						});

					// Depth: for VI we need a cubemap depth array (all faces). For fallback a temporary 2D depth buffer is enough.
					renderGraph::RGTextureHandle depth{};
					if (useVI || useLayered)
					{
						depth = graph.CreateTexture(renderGraph::RGTextureDesc{
							.extent = cubeExtent,
							.format = rhi::Format::D32_FLOAT,
							.usage = renderGraph::ResourceUsage::DepthStencil,
							.type = renderGraph::TextureType::Cube,
							.debugName = "PointShadowDepthCube"
							});
					}
					else
					{
						depth = graph.CreateTexture(renderGraph::RGTextureDesc{
							.extent = cubeExtent,
							.format = rhi::Format::D32_FLOAT,
							.usage = renderGraph::ResourceUsage::DepthStencil,
							.debugName = "PointShadowDepthTmp"
							});
					}

					PointShadowRec rec{};
					rec.cube = cube;
					rec.depthTmp = depth;
					rec.pos = light.position;
					rec.range = std::max(1.0f, light.range);
					rec.lightIndex = lightIndex;
					pointShadows.push_back(rec);

					auto FaceView = [](const mathUtils::Vec3& pos, int face) -> mathUtils::Mat4
						{
							// +X, -X, +Y, -Y, +Z, -Z
							static const mathUtils::Vec3 dirs[6] = {
								{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
							};

							static const mathUtils::Vec3 ups[6] = {
								{ 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 }
							};
							return mathUtils::LookAtRH(pos, pos + dirs[face], ups[face]);
						};

					const float pointNearZ = 0.01f;
					const mathUtils::Mat4 proj90 = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(90.0f), 1.0f, pointNearZ, rec.range);

					if (useLayered)
					{
						// One pass: render all faces using SV_RenderTargetArrayIndex (layered rendering).
						rhi::ClearDesc clear{};
						clear.clearColor = true;
						clear.clearDepth = true;
						clear.color = { 1.0f, 1.0f, 1.0f, 1.0f }; // far
						clear.depth = 1.0f;

						renderGraph::PassAttachments att{};
						att.useSwapChainBackbuffer = false;
						att.color = cube;
						att.colorCubeAllFaces = true;
						att.depth = depth;
						att.clearDesc = clear;

						const std::string passName =
							"PointShadowPassLayered_" + std::to_string(static_cast<int>(pointShadows.size() - 1));

						struct alignas(16) PointShadowLayeredConstants
						{
							// 6 matrices as ROWS (transposed on CPU).
							std::array<float, 16 * 6> uFaceViewProj{};
							std::array<float, 4>      uLightPosRange{}; // xyz + range
							std::array<float, 4>      uMisc{};          // unused (bias is texel-based in main shader)
						};

						PointShadowLayeredConstants pointShadowConstants{};

						for (int face = 0; face < 6; ++face)
						{
							const mathUtils::Mat4 faceViewProj = proj90 * FaceView(rec.pos, face);
							const mathUtils::Mat4 faceViewProjTranspose = mathUtils::Transpose(faceViewProj);
							std::memcpy(pointShadowConstants.uFaceViewProj.data() + (face * 16),
								mathUtils::ValuePtr(faceViewProjTranspose), sizeof(float) * 16);
						}

						pointShadowConstants.uLightPosRange = { rec.pos.x, rec.pos.y, rec.pos.z, rec.range };
						pointShadowConstants.uMisc = { 0, 0, 0, 0 };

						graph.AddPass(passName, std::move(att),
							[this, pointShadowConstants, shadowBatchesLayered, instStride](renderGraph::PassContext& ctx) mutable
							{
								ctx.commandList.SetViewport(0, 0,
									static_cast<int>(ctx.passExtent.width),
									static_cast<int>(ctx.passExtent.height));
								ctx.commandList.SetState(pointShadowState_);
								ctx.commandList.BindPipeline(psoPointShadowLayered_);
								ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &pointShadowConstants, 1 }));
								this->DrawInstancedShadowBatches(ctx.commandList, shadowBatchesLayered, instStride);
							});

					}
					else if (useVI)
					{
						// One pass: render all faces using SV_ViewID.
						rhi::ClearDesc clear{};
						clear.clearColor = true;
						clear.clearDepth = true;
						clear.color = { 1.0f, 1.0f, 1.0f, 1.0f }; // far
						clear.depth = 1.0f;

						renderGraph::PassAttachments att{};
						att.useSwapChainBackbuffer = false;
						att.color = cube;
						att.colorCubeAllFaces = true;
						att.depth = depth;
						att.clearDesc = clear;

						const std::string passName =
							"PointShadowPassVI_" + std::to_string(static_cast<int>(pointShadows.size() - 1));

						struct alignas(16) PointShadowVIConstants
						{
							// 6 matrices as ROWS (transposed on CPU).
							std::array<float, 16 * 6> uFaceViewProj{};
							std::array<float, 4>      uLightPosRange{}; // xyz + range
							std::array<float, 4>      uMisc{};          // unused (bias is texel-based in main shader)
						};

						PointShadowVIConstants pointShadowConstants{};
						for (int face = 0; face < 6; ++face)
						{
							const mathUtils::Mat4 faceViewProj = proj90 * FaceView(rec.pos, face);
							const mathUtils::Mat4 faceViewProjTranspose = mathUtils::Transpose(faceViewProj);
							std::memcpy(pointShadowConstants.uFaceViewProj.data() + (face * 16),
								mathUtils::ValuePtr(faceViewProjTranspose), sizeof(float) * 16);
						}
						pointShadowConstants.uLightPosRange = { rec.pos.x, rec.pos.y, rec.pos.z, rec.range };
						pointShadowConstants.uMisc = { 0, 0, 0, 0 };

						graph.AddPass(passName, std::move(att),
							[this, pointShadowConstants, shadowBatches, instStride](renderGraph::PassContext& ctx) mutable
							{
								ctx.commandList.SetViewport(0, 0,
									static_cast<int>(ctx.passExtent.width),
									static_cast<int>(ctx.passExtent.height));

								ctx.commandList.SetState(pointShadowState_);
								ctx.commandList.BindPipeline(psoPointShadowVI_);

								ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &pointShadowConstants, 1 }));

								this->DrawInstancedShadowBatches(ctx.commandList, shadowBatches, instStride);

							});
					}
					else
					{
						// Fallback: 6 passes (one cubemap face per pass).
						for (int face = 0; face < 6; ++face)
						{
							const mathUtils::Mat4 faceViewProj = proj90 * FaceView(rec.pos, face);

							rhi::ClearDesc clear{};
							clear.clearColor = true;
							clear.clearDepth = true;

							clear.color = { 1.0f, 1.0f, 1.0f, 1.0f }; // far
							clear.depth = 1.0f;

							renderGraph::PassAttachments att{};
							att.useSwapChainBackbuffer = false;
							att.color = cube;
							att.colorCubeFace = static_cast<std::uint32_t>(face);
							att.depth = depth;
							att.clearDesc = clear;

							const std::string passName =
								"PointShadowPass_" + std::to_string(static_cast<int>(pointShadows.size() - 1)) +
								"_F" + std::to_string(face);

							struct alignas(16) PointShadowConstants
							{
								std::array<float, 16> uFaceViewProj{};
								std::array<float, 4>  uLightPosRange{}; // xyz + range
								std::array<float, 4>  uMisc{};          // unused (bias is texel-based in main shader)
							};

							PointShadowConstants pointShadowConstants{};
							const mathUtils::Mat4 faceViewProjTranspose = mathUtils::Transpose(faceViewProj);
							std::memcpy(pointShadowConstants.uFaceViewProj.data(), mathUtils::ValuePtr(faceViewProjTranspose), sizeof(float) * 16);
							pointShadowConstants.uLightPosRange = { rec.pos.x, rec.pos.y, rec.pos.z, rec.range };
							pointShadowConstants.uMisc = { 0, 0, 0, 0 };


							graph.AddPass(passName, std::move(att),
								[this, pointShadowConstants, shadowBatches, instStride](renderGraph::PassContext& ctx) mutable
								{
									ctx.commandList.SetViewport(0, 0,
										static_cast<int>(ctx.passExtent.width),
										static_cast<int>(ctx.passExtent.height));

									ctx.commandList.SetState(pointShadowState_);
									ctx.commandList.BindPipeline(psoPointShadow_);

									ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &pointShadowConstants, 1 }));

									this->DrawInstancedShadowBatches(ctx.commandList, shadowBatches, instStride);

								});
						}
					}
				}

			}

			// Upload shadow metadata (t11).
			{
				ShadowDataSB sd{};

				// Directional CSM (atlas)
				{
					// Store up to 3 cascades; shader reads only the first dirCascadeCount entries.
					for (std::uint32_t c = 0; c < dirCascadeCount; ++c)
					{
						// Mat4 is column-major (GLM convention). In the HLSL we multiply as `mul(v, M)`
						// (row-vector), so we want to feed the *transposed* matrix. To avoid an extra CPU transpose,
						// we pack the matrix columns and the shader reconstructs a float4x4 from them as rows.
						const mathUtils::Mat4& vp = dirCascadeVP[c];
						sd.dirVPRows[c * 4 + 0] = vp[0];
						sd.dirVPRows[c * 4 + 1] = vp[1];
						sd.dirVPRows[c * 4 + 2] = vp[2];
						sd.dirVPRows[c * 4 + 3] = vp[3];
					}

					const float split1 = (dirCascadeCount >= 2) ? dirSplits[1] : dirSplits[dirCascadeCount];
					const float split2 = (dirCascadeCount >= 3) ? dirSplits[2] : dirSplits[dirCascadeCount];
					const float split3 = dirSplits[dirCascadeCount];
					const float fadeFrac = 0.10f; // blend width as a fraction of cascade length
					sd.dirSplits = mathUtils::Vec4(split1, split2, split3, fadeFrac);

					const float invAtlasW = 1.0f / static_cast<float>(shadowExtent.width);
					const float invAtlasH = 1.0f / static_cast<float>(shadowExtent.height);
					const float invTile = 1.0f / static_cast<float>(dirTileSize);
					sd.dirInfo = mathUtils::Vec4(invAtlasW, invAtlasH, invTile, static_cast<float>(dirCascadeCount));
				}

				for (std::size_t spotShadowIndex = 0; spotShadowIndex < spotShadows.size(); ++spotShadowIndex)
				{
					const auto& spotShadow = spotShadows[spotShadowIndex];

					const mathUtils::Mat4 vp = spotShadow.viewProj;

					sd.spotVPRows[spotShadowIndex * 4 + 0] = vp[0];
					sd.spotVPRows[spotShadowIndex * 4 + 1] = vp[1];
					sd.spotVPRows[spotShadowIndex * 4 + 2] = vp[2];
					sd.spotVPRows[spotShadowIndex * 4 + 3] = vp[3];

					sd.spotInfo[spotShadowIndex] = mathUtils::Vec4(AsFloatBits(spotShadow.lightIndex), 0, 0.0f, 0);
				}

				for (std::size_t pointShadowIndex = 0; pointShadowIndex < pointShadows.size(); ++pointShadowIndex)
				{
					const auto& pointShadow = pointShadows[pointShadowIndex];
					sd.pointPosRange[pointShadowIndex] = mathUtils::Vec4(pointShadow.pos, pointShadow.range);
					sd.pointInfo[pointShadowIndex] = mathUtils::Vec4(AsFloatBits(pointShadow.lightIndex), 0, 0.0f, 0);
				}

				device_.UpdateBuffer(shadowDataBuffer_, std::as_bytes(std::span{ &sd, 1 }));
			}

