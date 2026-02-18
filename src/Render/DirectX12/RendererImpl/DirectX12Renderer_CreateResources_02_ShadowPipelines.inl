			// Shadow pipeline (depth-only)
			if (device_.GetBackend() == rhi::Backend::DirectX12)
			{
				const auto vsShadow = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Vertex,
					.name = "VS_Shadow",
					.filePath = shadowPath.string(),
					.defines = {}
					});
				const auto psShadow = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Pixel,
					.name = "PS_Shadow",
					.filePath = shadowPath.string(),
					.defines = {}
					});

				psoShadow_ = psoCache_.GetOrCreate("PSO_Shadow", vsShadow, psShadow);

				shadowState_.depth.testEnable = true;
				shadowState_.depth.writeEnable = true;
				shadowState_.depth.depthCompareOp = rhi::CompareOp::LessEqual;

				// Disable culling for the shadow pass in stage-1 (avoid winding issues).
				shadowState_.rasterizer.cullMode = rhi::CullMode::None;
				shadowState_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;

				shadowState_.blend.enable = false;
			}

			// Point shadow pipeline (R32_FLOAT distance cubemap)
			if (device_.GetBackend() == rhi::Backend::DirectX12)
			{
				const std::filesystem::path pointShadowVIPath = corefs::ResolveAsset("shaders\\ShadowPointVI_dx12.hlsl");
				const std::filesystem::path pointShadowLayeredPath = corefs::ResolveAsset("shaders\\ShadowPointLayered_dx12.hlsl");

				const auto vsPoint = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Vertex,
					.name = "VS_ShadowPoint",
					.filePath = pointShadowPath.string(),
					.defines = {}
					});
				const auto psPoint = shaderLibrary_.GetOrCreateShader(ShaderKey{
					.stage = rhi::ShaderStage::Pixel,
					.name = "PS_ShadowPoint",
					.filePath = pointShadowPath.string(),
					.defines = {}
					});
				psoPointShadow_ = psoCache_.GetOrCreate("PSO_PointShadow", vsPoint, psPoint);

			// Optional View-Instancing variant (single pass renders all 6 cubemap faces).
			// Requires SM6.1 + DXC + ViewInstancingTier support.
			//
			// Safety: if it fails once (DXC missing/compile error/PSO creation failure), we disable further attempts
			// until restart to avoid repeated DXC/PSO work.
			if (!disablePointShadowVI_)
			{
				if (device_.SupportsShaderModel6() && device_.SupportsViewInstancing())
				{
					const auto vsPointVI = shaderLibrary_.GetOrCreateShader(ShaderKey{
						.stage = rhi::ShaderStage::Vertex,
						.name = "VS_ShadowPointVI",
						.filePath = pointShadowVIPath.string(),
						.defines = {},
						.shaderModel = rhi::ShaderModel::SM6_1
						});
					const auto psPointVI = shaderLibrary_.GetOrCreateShader(ShaderKey{
						.stage = rhi::ShaderStage::Pixel,
						.name = "PS_ShadowPointVI",
						.filePath = pointShadowVIPath.string(),
						.defines = {},
						.shaderModel = rhi::ShaderModel::SM6_1
						});
			
					if (vsPointVI && psPointVI)
					{
						psoPointShadowVI_ = psoCache_.GetOrCreate("PSO_PointShadow_VI", vsPointVI, psPointVI, rhi::PrimitiveTopologyType::Triangle, 6);
					}
				}
				else
				{
					// Not supported on this device; avoid checking again.
					disablePointShadowVI_ = true;
				}
			
				if (!psoPointShadowVI_)
				{
					// Failed to create (DXC/compile/PSO), stick to the 6-pass fallback for this run.
					disablePointShadowVI_ = true;
				}
			}

			// Optional Layered variant (single pass renders all 6 cubemap faces into a Texture2DArray).
			// Uses SV_RenderTargetArrayIndex from VS, so it requires SM6.1 + DXC and
			// D3D12_OPTIONS3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer.
			//
			// Safety: if it fails once (DXC missing/compile error/PSO creation failure), we disable further attempts
			// until restart to avoid repeated DXC/PSO work.
			if (!disablePointShadowLayered_)
			{
				if (device_.SupportsShaderModel6() && device_.SupportsVPAndRTArrayIndexFromAnyShader())
				{
					const auto vsPointLayered = shaderLibrary_.GetOrCreateShader(ShaderKey{
						.stage = rhi::ShaderStage::Vertex,
						.name = "VS_ShadowPointLayered",
						.filePath = pointShadowLayeredPath.string(),
						.defines = {},
						.shaderModel = rhi::ShaderModel::SM6_1
						});
					const auto psPointLayered = shaderLibrary_.GetOrCreateShader(ShaderKey{
						.stage = rhi::ShaderStage::Pixel,
						.name = "PS_ShadowPointLayered",
						.filePath = pointShadowLayeredPath.string(),
						.defines = {},
						.shaderModel = rhi::ShaderModel::SM6_1
						});
					
					if (vsPointLayered && psPointLayered)
					{
						psoPointShadowLayered_ = psoCache_.GetOrCreate("PSO_PointShadow_Layered", vsPointLayered, psPointLayered);
					}
				}
				else
				{
					// Not supported on this device; avoid checking again.
					disablePointShadowLayered_ = true;
				}
				
				if (!psoPointShadowLayered_)
				{
					// Failed to create (DXC/compile/PSO), stick to the 6-pass fallback for this run.
					disablePointShadowLayered_ = true;
				}
			}
			pointShadowState_.depth.testEnable = true;
			pointShadowState_.depth.writeEnable = true;
			pointShadowState_.depth.depthCompareOp = rhi::CompareOp::LessEqual;
			
			pointShadowState_.rasterizer.cullMode = rhi::CullMode::None;
			pointShadowState_.rasterizer.frontFace = rhi::FrontFace::CounterClockwise;
			
			pointShadowState_.blend.enable = false;
			}
