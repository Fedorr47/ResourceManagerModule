module;

#if defined(_WIN32)
// Prevent Windows headers from defining the `min`/`max` macros which break `std::min`/`std::max`.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#endif


// D3D-style clip-space helpers (Z in [0..1]).

#include <array>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include "assert.h"

export module core:renderer_dx12;

import :rhi;
import :scene;
import :visibility;
import :math_utils;
import :hash_utils;
import :renderer_settings;
import :render_core;
import :render_graph;
import :file_system;
import :mesh;
import :scene_bridge;
import :debug_draw;
import :debug_draw_renderer_dx12;
import :debug_text;
import :debug_text_renderer_dx12;
import :common_DX12_Structs;

export namespace rendern
{
	// Cubemap capture face conventions.
	// Face order is D3D cube array order: +X, -X, +Y, -Y, +Z, -Z (slices 0..5).
	// This must stay consistent across point shadows and reflection captures.
	mathUtils::Mat4 CubeFaceViewRH(const mathUtils::Vec3& pos, int face) noexcept
	{
		// +X, -X, +Y, -Y, +Z, -Z
		static const mathUtils::Vec3 kDirs[6] = {
			{ 1, 0, 0 },
			{ -1, 0, 0 },
			{ 0, 1, 0 },
			{ 0, -1, 0 },
			{ 0, 0, 1 },
			{ 0, 0, -1 }
		};
		static const mathUtils::Vec3 kUps[6] = {
			{ 0, 1, 0 },
			{ 0, 1, 0 },
			{ 0, 0, -1 },
			{ 0, 0, 1 },
			{ 0, 1, 0 },
			{ 0, 1, 0 }
		};
		const std::uint32_t f = (face < 6u) ? face : 0u;

		const mathUtils::Vec3 forward = kDirs[f];
		const mathUtils::Vec3 up = kUps[f];

		return mathUtils::LookAtRH(pos, pos + forward, up);
	}

	class DX12Renderer
	{
	public:
		DX12Renderer(rhi::IRHIDevice& device, RendererSettings settings = {})
			: device_(device)
			, settings_(std::move(settings))
			, shaderLibrary_(device)
			, psoCache_(device)
			, debugDrawRenderer_(device, shaderLibrary_, psoCache_)
			, debugTextRenderer_(device, shaderLibrary_, psoCache_)
		{
			CreateResources();
		}

		void SetSettings(const RendererSettings& settings)
		{
			settings_ = settings;
			EnsureReflectionCaptureResources();
		}

		void RenderFrame(rhi::IRHISwapChain& swapChain, const Scene& scene, const void* imguiDrawData)
		{
#include "RendererImpl/DirectX12Renderer_RenderFrame_00_SetupCSM.inl"
#include "RendererImpl/DirectX12Renderer_RenderFrame_01_BuildInstances.inl"
#include "RendererImpl/DirectX12Renderer_RenderFrame_02_ShadowPasses.inl"
#include "RendererImpl/DirectX12Renderer_RenderFrame_02_ReflectionCapture.inl"
#include "RendererImpl/DirectX12Renderer_RenderFrame_03_PreDepth.inl"
#include "RendererImpl/DirectX12Renderer_RenderFrame_04_MainPass.inl"
#include "RendererImpl/DirectX12Renderer_RenderFrame_05_DebugAndPresent.inl"
		}

		void Shutdown()
		{
#include "RendererImpl/DirectX12Renderer_Shutdown.inl"
		}

	private:

		void DrawInstancedShadowBatches(rhi::CommandList& commandList, std::span<const ShadowBatch> shadowBatches, std::uint32_t instanceStrideBytes) const
		{
			for (std::size_t batchIndex = 0; batchIndex < shadowBatches.size(); ++batchIndex)
			{
				const ShadowBatch& shadowBatch = shadowBatches[batchIndex];
				if (!shadowBatch.mesh || shadowBatch.instanceCount == 0)
				{
					continue;
				}

				commandList.BindInputLayout(shadowBatch.mesh->layoutInstanced);
				commandList.BindVertexBuffer(0, shadowBatch.mesh->vertexBuffer, shadowBatch.mesh->vertexStrideBytes, 0);
				commandList.BindVertexBuffer(1, instanceBuffer_, instanceStrideBytes, shadowBatch.instanceOffset * instanceStrideBytes);
				commandList.BindIndexBuffer(shadowBatch.mesh->indexBuffer, shadowBatch.mesh->indexType, 0);

				commandList.DrawIndexed(
					shadowBatch.mesh->indexCount,
					shadowBatch.mesh->indexType,
					0,
					0,
					shadowBatch.instanceCount,
					0);
			}
		}

		rhi::PipelineHandle PlanarPipelineFor(MaterialPerm perm) const noexcept
		{
			const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
			const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);
			const std::uint32_t idx = (useTex ? 1u : 0u) | (useShadow ? 2u : 0u);
			return psoPlanar_[idx];
		}

		rhi::PipelineHandle MainPipelineFor(MaterialPerm perm) const noexcept
		{
			const bool useTex = HasFlag(perm, MaterialPerm::UseTex);
			const bool useShadow = HasFlag(perm, MaterialPerm::UseShadow);
			const std::uint32_t idx = (useTex ? 1u : 0u) | (useShadow ? 2u : 0u);
			return psoMain_[idx];
		}

		static float AsFloatBits(std::uint32_t packedBits) noexcept
		{
			float floatValue{};
			std::memcpy(&floatValue, &packedBits, sizeof(packedBits));
			return floatValue;
		}

		FrameCameraData BuildFrameCameraData(const Scene& scene, const rhi::Extent2D& extent) const
		{
			const float aspect = extent.height
				? (static_cast<float>(extent.width) / static_cast<float>(extent.height))
				: 1.0f;

			FrameCameraData data{};
			data.proj = mathUtils::PerspectiveRH_ZO(mathUtils::DegToRad(scene.camera.fovYDeg), aspect, scene.camera.nearZ, scene.camera.farZ);
			data.view = mathUtils::LookAt(scene.camera.position, scene.camera.target, scene.camera.up);
			data.viewProj = data.proj * data.view;
			data.invViewProj = mathUtils::Inverse(data.viewProj);
			data.invViewProjT = mathUtils::Transpose(data.invViewProj);
			data.camPos = scene.camera.position;
			data.camForward = mathUtils::Normalize(scene.camera.target - scene.camera.position);
			return data;
		}

		std::uint32_t UploadLights(const Scene& scene, const mathUtils::Vec3& camPos)
		{
#include "RendererImpl/DirectX12Renderer_UploadLights.inl"
		}

		void CreateResources()
		{
#include "RendererImpl/DirectX12Renderer_CreateResources_00_PathsSkybox.inl"
#include "RendererImpl/DirectX12Renderer_CreateResources_01_MainPipelines.inl"
#include "RendererImpl/DirectX12Renderer_CreateResources_02_ShadowPipelines.inl"
#include "RendererImpl/DirectX12Renderer_CreateResources_03_DynamicBuffers.inl"

			EnsureReflectionCaptureResources();
		}

		void EnsureReflectionCaptureResources()
		{
			if (device_.GetBackend() != rhi::Backend::DirectX12)
			{
				return;
			}

			const std::uint32_t res = std::clamp(settings_.reflectionCaptureResolution, 32u, 2048u);
			const rhi::Extent2D desired{ res, res };

			const bool needRecreate =
				(!reflectionCube_) ||
				(reflectionCubeExtent_.width != desired.width) ||
				(reflectionCubeExtent_.height != desired.height);

			if (!needRecreate)
			{
				return;
			}

			// (Re)create both color and depth cubemaps.
			const rhi::TextureHandle newCube = device_.CreateTextureCube(desired, rhi::Format::RGBA8_UNORM);
			const rhi::TextureHandle newDepthCube = device_.CreateTextureCube(desired, rhi::Format::D32_FLOAT);

			if (!newCube || !newDepthCube)
			{
				if (newCube)
				{
					device_.DestroyTexture(newCube);
				}
				if (newDepthCube)
				{
					device_.DestroyTexture(newDepthCube);
				}
				return;
			}

			if (reflectionCube_)
			{
				device_.DestroyTexture(reflectionCube_);
				reflectionCube_ = {};
			}
			if (reflectionDepthCube_)
			{
				device_.DestroyTexture(reflectionDepthCube_);
				reflectionDepthCube_ = {};
			}

			reflectionCube_ = newCube;
			reflectionDepthCube_ = newDepthCube;
			reflectionCubeExtent_ = desired;

			if (reflectionCubeDescIndex_ == 0)
			{
				reflectionCubeDescIndex_ = device_.AllocateTextureDesctiptor(reflectionCube_);
			}
			else
			{
				device_.UpdateTextureDescriptor(reflectionCubeDescIndex_, reflectionCube_);
			}


			for (ReflectionProbeRuntime& probe : reflectionProbes_)
			{
				if (probe.cube)
				{
					device_.DestroyTexture(probe.cube);
					probe.cube = {};
				}
				if (probe.depthCube)
				{
					device_.DestroyTexture(probe.depthCube);
					probe.depthCube = {};
				}
				probe.dirty = true;
				probe.hasLastPos = false;
			}
		}



		void EnsureReflectionProbeResources(std::size_t requiredCount)
		{
			if (requiredCount > kMaxReflectionProbes)
			{
				requiredCount = kMaxReflectionProbes;
			}

			if (reflectionProbes_.size() < requiredCount)
			{
				reflectionProbes_.resize(requiredCount);
			}

			for (std::size_t i = 0; i < requiredCount; ++i)
			{
				ReflectionProbeRuntime& probe = reflectionProbes_[i];
				const bool needCreate = (!probe.cube) || (!probe.depthCube);
				if (!needCreate)
				{
					continue;
				}

				if (probe.cube)
				{
					device_.DestroyTexture(probe.cube);
					probe.cube = {};
				}
				if (probe.depthCube)
				{
					device_.DestroyTexture(probe.depthCube);
					probe.depthCube = {};
				}

				probe.cube = device_.CreateTextureCube(reflectionCubeExtent_, rhi::Format::RGBA8_UNORM);
				probe.depthCube = device_.CreateTextureCube(reflectionCubeExtent_, rhi::Format::D32_FLOAT);

				if (!probe.cube || !probe.depthCube)
				{
					if (probe.cube)
					{
						device_.DestroyTexture(probe.cube);
						probe.cube = {};
					}
					if (probe.depthCube)
					{
						device_.DestroyTexture(probe.depthCube);
						probe.depthCube = {};
					}
					continue;
				}

				if (probe.cubeDescIndex == 0)
				{
					probe.cubeDescIndex = device_.AllocateTextureDesctiptor(probe.cube);
				}
				else
				{
					device_.UpdateTextureDescriptor(probe.cubeDescIndex, probe.cube);
				}

				probe.dirty = true;
				probe.hasLastPos = false;
				probe.ownerDrawItem = -1;
				probe.capturePos = {};
				probe.lastPos = {};
			}
		}

	private:
		static constexpr std::uint32_t kMaxLights = 64;
		static constexpr std::uint32_t kDefaultInstanceBufferSizeBytes = 8u * 1024u * 1024u; // 8 MB (combined shadow+main instances)

		rhi::IRHIDevice& device_;
		RendererSettings settings_{};

		ShaderLibrary shaderLibrary_;
		PSOCache psoCache_;
		debugDraw::DebugDrawRendererDX12 debugDrawRenderer_;
		debugText::DebugTextRendererDX12 debugTextRenderer_;

		// Main pass
		std::array<rhi::PipelineHandle, 4> psoMain_{}; // idx: (UseTex?1:0)|(UseShadow?2:0)
		std::array<rhi::PipelineHandle, 4> psoPlanar_{}; // same indexing, compiled with CORE_PLANAR_CLIP
		rhi::PipelineHandle psoPlanarComposite_{}; // fullscreen planar composite (mask+color -> SceneColor)
		rhi::PipelineHandle psoHighlight_{}; // editor selection highlight overlay
		rhi::PipelineHandle psoOutline_{}; // editor selection outline shell
		rhi::PipelineHandle psoDeferredGBuffer_{}; // MRT G-Buffer writer
		rhi::PipelineHandle psoDeferredLighting_{}; // fullscreen deferred lighting
		rhi::PipelineHandle psoSSAO_{};          // deferred SSAO (normal+depth -> R32_FLOAT)
		rhi::PipelineHandle psoSSAOForward_{};   // forward SSAO (depth-only reconstruction -> R32_FLOAT)
		rhi::PipelineHandle psoSSAOBlur_{};      // fullscreen SSAO blur (R32_FLOAT)
		rhi::PipelineHandle psoSSAOComposite_{}; // fullscreen SceneColor * AO
		rhi::PipelineHandle psoFog_{};           // fullscreen fog post effect
		rhi::PipelineHandle psoCopyToSwapChain_{}; // fullscreen copy SceneColor -> swapchain
		rhi::InputLayoutHandle fullscreenLayout_{}; // empty input layout for fullscreen VS (SV_VertexID)
		rhi::GraphicsState deferredLightingState_{};
		rhi::GraphicsState planarCompositeState_{};
		rhi::GraphicsState copyToSwapChainState_{};
		rhi::GraphicsState state_{};
		rhi::GraphicsState transparentState_{};
		rhi::GraphicsState highlightState_{};
		rhi::GraphicsState outlineMarkState_{};
		rhi::GraphicsState outlineState_{};
		rhi::GraphicsState preDepthState_{};
		rhi::GraphicsState mainAfterPreDepthState_{};
		rhi::GraphicsState planarMaskState_{};
		rhi::GraphicsState planarReflectedState_{};

		rhi::BufferHandle instanceBuffer_{};
		std::uint32_t instanceBufferSizeBytes_{ kDefaultInstanceBufferSizeBytes };
		rhi::BufferHandle highlightInstanceBuffer_{}; // single-instance VB for selection highlight

		// Shadow pass
		rhi::PipelineHandle psoShadow_{};
		rhi::GraphicsState shadowState_{};

		rhi::BufferHandle lightsBuffer_{};
		rhi::BufferHandle shadowDataBuffer_{};

		rhi::BufferHandle reflectionProbeMetaBuffer_{};

		// Point shadow pass (R32_FLOAT distance cubemap)
		rhi::PipelineHandle psoPointShadow_{};
		rhi::PipelineHandle psoPointShadowLayered_{}; // SM6.1 + SV_RenderTargetArrayIndex layered rendering (single-pass cubemap)
		bool disablePointShadowLayered_{ false };// do not try again after first failure (until restart)
		rhi::PipelineHandle psoPointShadowVI_{}; // SM6.1 + SV_ViewID + view instancing (single-pass cubemap)
		bool disablePointShadowVI_{ false };// do not try again after first failure (until restart)
		rhi::GraphicsState pointShadowState_{};

		// Reflection capture cubemap (persistent). RenderGraph will import these textures when building passes.
		rhi::TextureHandle reflectionCube_{};
		rhi::TextureHandle reflectionDepthCube_{};
		rhi::TextureDescIndex reflectionCubeDescIndex_{}; // SRV for sampling in main pass
		rhi::Extent2D reflectionCubeExtent_{}; // last created face size

		// Reflection capture pipelines (created in 0007).
		rhi::PipelineHandle psoReflectionCapture_{};            // optional (6-pass shader later)
		rhi::PipelineHandle psoReflectionCaptureLayered_{};     // SM6.1 layered
		bool disableReflectionCaptureLayered_{ false };
		rhi::PipelineHandle psoReflectionCaptureVI_{};          // SM6.1 view instancing
		bool disableReflectionCaptureVI_{ false };


		std::vector<ReflectionProbeRuntime> reflectionProbes_;
		std::vector<int> reflectiveOwnerDrawItems_;           // frame list of owners
		std::vector<int> drawItemReflectionProbeIndices_;     // size == scene.drawItems.size()
		static constexpr std::size_t kMaxReflectionProbes = 16;

		int reflectionCaptureLastAnchorKind_{ 0 }; // 0=auto/none, 1=selected, 2=owner, 3=debugOwnerIndex
		int reflectionCaptureLastAnchorNode_{ -1 }; // LevelAsset node index (or -1)

		MeshRHI skyboxMesh_{};
		rhi::PipelineHandle psoSkybox_{};
		// Debug: visualize a cubemap as a 3x2 atlas (swapchain pass).
		rhi::PipelineHandle psoDebugCubeAtlas_{};
		rhi::GraphicsState debugCubeAtlasState_{};
		rhi::InputLayoutHandle debugCubeAtlasLayout_{};
		rhi::BufferHandle debugCubeAtlasVB_{};
		std::uint32_t debugCubeAtlasVBStrideBytes_{ 0 };
		rhi::GraphicsState skyboxState_{};
	};
} // namespace rendern