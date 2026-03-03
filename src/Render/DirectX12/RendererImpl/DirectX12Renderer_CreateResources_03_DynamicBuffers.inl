			// DX12-only dynamic buffers
			if (device_.GetBackend() == rhi::Backend::DirectX12)
			{
				// Lights structured buffer (t2)
				{
					rhi::BufferDesc ld{};
					ld.bindFlag = rhi::BufferBindFlag::StructuredBuffer;
					ld.usageFlag = rhi::BufferUsageFlag::Dynamic;
					ld.sizeInBytes = sizeof(GPULight) * kMaxLights;
					ld.structuredStrideBytes = static_cast<std::uint32_t>(sizeof(GPULight));
					ld.debugName = "LightsSB";
					lightsBuffer_ = device_.CreateBuffer(ld);
				}


				// Shadow metadata structured buffer (t11) — holds spot VP rows + indices/bias, and point pos/range + indices/bias.
				{
					rhi::BufferDesc sd{};
					sd.bindFlag = rhi::BufferBindFlag::StructuredBuffer;
					sd.usageFlag = rhi::BufferUsageFlag::Dynamic;
					sd.sizeInBytes = sizeof(ShadowDataSB);
					sd.structuredStrideBytes = static_cast<std::uint32_t>(sizeof(ShadowDataSB));
					sd.debugName = "ShadowDataSB";
					shadowDataBuffer_ = device_.CreateBuffer(sd);
				}

				// Per-instance model matrices VB (slot1)
				{
					rhi::BufferDesc id{};
					id.bindFlag = rhi::BufferBindFlag::VertexBuffer;
					id.usageFlag = rhi::BufferUsageFlag::Dynamic;
					id.sizeInBytes = instanceBufferSizeBytes_;
					id.debugName = "InstanceVB";
					instanceBuffer_ = device_.CreateBuffer(id);
				}

				// Editor selection highlight: single-instance model matrix VB (slot1).
				{
					rhi::BufferDesc id{};
					id.bindFlag = rhi::BufferBindFlag::VertexBuffer;
					id.usageFlag = rhi::BufferUsageFlag::Dynamic;
					id.sizeInBytes = sizeof(InstanceData) * 4096u; // multi-select outline/highlight
					id.debugName = "HighlightInstanceVB";
					highlightInstanceBuffer_ = device_.CreateBuffer(id);
				}

				// Persistent reflection capture cubemap.
				// The texture is (re)created based on current RendererSettings (resolution).
				EnsureReflectionCaptureResources();
			}
