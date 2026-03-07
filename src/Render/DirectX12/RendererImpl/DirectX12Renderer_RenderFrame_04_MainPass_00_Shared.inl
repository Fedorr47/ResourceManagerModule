// ---------------- Deferred path (DX12) ----------------
const bool canDeferred =
settings_.enableDeferred &&
device_.GetBackend() == rhi::Backend::DirectX12 &&
psoDeferredGBuffer_ &&
psoDeferredLighting_ &&
fullscreenLayout_ &&
swapChain.GetDepthTexture();

std::uint32_t activeReflectionProbeCount = 0u;

struct EditorSelectionLists
{
	std::vector<EditorSelectionDraw> opaque{};
	std::vector<EditorSelectionDraw> transparent{};
	std::vector<InstanceData> instances{};
	std::vector<std::uint32_t> opaqueStarts{};
	std::vector<std::uint32_t> transparentStarts{};
};

constexpr std::uint32_t kEditorOutlineStencilRef = 0x80u;
auto BuildEditorSelectionLists = [&]() -> EditorSelectionLists
	{
		EditorSelectionLists result{};
		constexpr std::size_t kMaxSelectionInstances = 4096;

		result.opaque.reserve(scene.editorSelectedDrawItems.size());
		result.transparent.reserve(scene.editorSelectedDrawItems.size());
		result.instances.reserve(std::min(scene.editorSelectedDrawItems.size(), kMaxSelectionInstances));
		result.opaqueStarts.reserve(scene.editorSelectedDrawItems.size());
		result.transparentStarts.reserve(scene.editorSelectedDrawItems.size());

		for (const int diIndex : scene.editorSelectedDrawItems)
		{
			if (diIndex < 0)
			{
				continue;
			}
			if (result.instances.size() >= kMaxSelectionInstances)
			{
				break;
			}

			const std::size_t idx = static_cast<std::size_t>(diIndex);
			if (idx >= scene.drawItems.size())
			{
				continue;
			}

			const DrawItem& di = scene.drawItems[idx];
			const rendern::MeshRHI* mesh = di.mesh ? &di.mesh->GetResource() : nullptr;
			if (!mesh || mesh->indexCount == 0 || !mesh->vertexBuffer || !mesh->indexBuffer)
			{
				continue;
			}

			EditorSelectionDraw sel{};
			sel.mesh = mesh;

			const mathUtils::Mat4 model = di.transform.ToMatrix();
			sel.instance.i0 = model[0];
			sel.instance.i1 = model[1];
			sel.instance.i2 = model[2];
			sel.instance.i3 = model[3];

			sel.outlineWorldOffset = 0.01f;
			if (di.mesh)
			{
				const auto& bounds = di.mesh->GetBounds();
				if (bounds.sphereRadius > 0.0f)
				{
					sel.outlineWorldOffset = std::max(0.01f, bounds.sphereRadius * 0.03f);
				}
			}

			if (di.material.id != 0)
			{
				const auto& mat = scene.GetMaterial(di.material);
				const MaterialPerm perm = EffectivePerm(mat);
				sel.isTransparent = HasFlag(perm, MaterialPerm::Transparent);
			}
			else
			{
				sel.isTransparent = false;
			}

			const std::uint32_t startInstance = static_cast<std::uint32_t>(result.instances.size());
			result.instances.push_back(sel.instance);

			if (sel.isTransparent)
			{
				result.transparent.push_back(sel);
				result.transparentStarts.push_back(startInstance);
			}
			else
			{
				result.opaque.push_back(sel);
				result.opaqueStarts.push_back(startInstance);
			}
		}

		return result;
	};

const EditorSelectionLists editorSelection = BuildEditorSelectionLists();
const auto& selectionOpaque = editorSelection.opaque;
const auto& selectionTransparent = editorSelection.transparent;
const auto& selectionInstances = editorSelection.instances;
const auto& selectionOpaqueStart = editorSelection.opaqueStarts;
const auto& selectionTransparentStart = editorSelection.transparentStarts;

auto ComputeForwardGBufferReflectionMeta = [&](MaterialHandle materialHandle, int reflectionProbeIndex, std::uint32_t activeProbeCount)
	{
		std::pair<float, float> result{ 0.0f, 0.0f };
		if (!settings_.enableReflectionCapture || materialHandle.id == 0 || activeProbeCount == 0u)
		{
			return result;
		}

		const auto& mat = scene.GetMaterial(materialHandle);
		if (mat.envSource != EnvSource::ReflectionCapture || reflectionProbeIndex < 0 || static_cast<std::uint32_t>(reflectionProbeIndex) >= activeProbeCount)
		{
			return result;
		}

		result.first = 1.0f;
		result.second = (static_cast<float>(reflectionProbeIndex) + 0.5f) / static_cast<float>(activeProbeCount);
		return result;
	};

auto ComputeDeferredGBufferReflectionMeta = [&](MaterialHandle materialHandle, int reflectionProbeIndex, const std::vector<int>& deferredProbeRemap, std::uint32_t activeProbeCount)
	{
		std::pair<float, float> result{ 0.0f, 0.0f };
		if (!settings_.enableReflectionCapture || materialHandle.id == 0 || reflectionProbeIndex < 0 || activeProbeCount == 0u || static_cast<std::size_t>(reflectionProbeIndex) >= deferredProbeRemap.size())
		{
			return result;
		}

		const auto& mat = scene.GetMaterial(materialHandle);
		if (mat.envSource != EnvSource::ReflectionCapture)
		{
			return result;
		}

		const int compactProbeIndex = deferredProbeRemap[static_cast<std::size_t>(reflectionProbeIndex)];
		if (compactProbeIndex < 0)
		{
			return result;
		}

		result.first = 1.0f;
		result.second = (static_cast<float>(compactProbeIndex) + 0.5f) / static_cast<float>(activeProbeCount);
		return result;
	};

auto MakeEditorSelectionConstants = [&](const mathUtils::Mat4& viewProj,
	const mathUtils::Mat4& dirLightViewProj,
	const mathUtils::Vec3& camPosLocal,
	const mathUtils::Vec3& camFLocal,
	const mathUtils::Vec4& baseColor) -> PerBatchConstants
	{
		PerBatchConstants constants{};
		const mathUtils::Mat4 viewProjT = mathUtils::Transpose(viewProj);
		const mathUtils::Mat4 dirVP_T = mathUtils::Transpose(dirLightViewProj);
		std::memcpy(constants.uViewProj.data(), mathUtils::ValuePtr(viewProjT), sizeof(float) * 16);
		std::memcpy(constants.uLightViewProj.data(), mathUtils::ValuePtr(dirVP_T), sizeof(float) * 16);
		constants.uCameraAmbient = { camPosLocal.x, camPosLocal.y, camPosLocal.z, 0.0f };
		constants.uCameraForward = { camFLocal.x, camFLocal.y, camFLocal.z, 0.0f };
		constants.uBaseColor = { baseColor.x, baseColor.y, baseColor.z, baseColor.w };
		constants.uMaterialFlags = { 0.0f, 0.0f, 0.0f, AsFloatBits(0u) };
		constants.uPbrParams = { 0.0f, 1.0f, 1.0f, 0.0f };
		constants.uCounts = { 0.0f, 0.0f, 0.0f, 0.0f };
		constants.uShadowBias = { 0.0f, 0.0f, 0.0f, 0.0f };
		constants.uEnvProbeBoxMin = { 0.0f, 0.0f, 0.0f, 0.0f };
		constants.uEnvProbeBoxMax = { 0.0f, 0.0f, 0.0f, 0.0f };
		return constants;
	};

auto DrawEditorSelectionGroup = [&](renderGraph::PassContext& ctx,
	const rhi::GraphicsState& restoreState,
	const mathUtils::Vec4& highlightColor,
	const mathUtils::Mat4& viewProj,
	const mathUtils::Mat4& dirLightViewProj,
	const mathUtils::Vec3& camPosLocal,
	const mathUtils::Vec3& camFLocal,
	const rhi::Extent2D& extent,
	const std::vector<EditorSelectionDraw>& group,
	const std::vector<std::uint32_t>& starts)
	{
		if (!highlightInstanceBuffer_ || group.empty() || selectionInstances.empty())
		{
			return;
		}

		device_.UpdateBuffer(highlightInstanceBuffer_, std::as_bytes(std::span{ selectionInstances }));

		auto BindEditorSelectionGeometry = [&](const rendern::MeshRHI& mesh)
			{
				ctx.commandList.BindInputLayout(mesh.layoutInstanced);
				ctx.commandList.BindVertexBuffer(0, mesh.vertexBuffer, mesh.vertexStrideBytes, 0);
				ctx.commandList.BindVertexBuffer(1, highlightInstanceBuffer_, instStride, 0);
				ctx.commandList.BindIndexBuffer(mesh.indexBuffer, mesh.indexType, 0);
			};

		const std::size_t count = std::min(group.size(), starts.size());
		for (std::size_t i = 0; i < count; ++i)
		{
			const EditorSelectionDraw& s = group[i];
			if (!s.mesh)
			{
				continue;
			}

			const std::uint32_t startInstance = starts[i];
			BindEditorSelectionGeometry(*s.mesh);

			if (psoOutline_)
			{
				ctx.commandList.SetStencilRef(kEditorOutlineStencilRef);
				ctx.commandList.SetState(outlineMarkState_);
				ctx.commandList.BindPipeline(psoHighlight_);

				PerBatchConstants markConstants = MakeEditorSelectionConstants(
					viewProj,
					dirLightViewProj,
					camPosLocal,
					camFLocal,
					mathUtils::Vec4(1.0f, 1.0f, 1.0f, 0.0f));
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &markConstants, 1 }));
				ctx.commandList.DrawIndexed(s.mesh->indexCount, s.mesh->indexType, 0, 0, 1, startInstance);

				ctx.commandList.SetState(outlineState_);
				ctx.commandList.BindPipeline(psoOutline_);

				PerBatchConstants outlineConstants = MakeEditorSelectionConstants(
					viewProj,
					dirLightViewProj,
					camPosLocal,
					camFLocal,
					mathUtils::Vec4(1.0f, 0.72f, 0.10f, 0.95f));
				outlineConstants.uPbrParams = { s.outlineWorldOffset, 0.0f, 0.0f, 0.0f };
				outlineConstants.uCounts = { 0.0f, 0.0f, 0.0f, 3.0f };
				outlineConstants.uShadowBias = {
					extent.width ? (1.0f / static_cast<float>(extent.width)) : 0.0f,
					extent.height ? (1.0f / static_cast<float>(extent.height)) : 0.0f,
					0.0f,
					0.0f
				};
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &outlineConstants, 1 }));
				ctx.commandList.DrawIndexed(s.mesh->indexCount, s.mesh->indexType, 0, 0, 1, startInstance);

				ctx.commandList.SetState(outlineMarkState_);
				ctx.commandList.BindPipeline(psoHighlight_);
				ctx.commandList.SetStencilRef(0u);
				ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &markConstants, 1 }));
				ctx.commandList.DrawIndexed(s.mesh->indexCount, s.mesh->indexType, 0, 0, 1, startInstance);
				ctx.commandList.SetStencilRef(0u);
			}

			ctx.commandList.SetState(highlightState_);
			ctx.commandList.BindPipeline(psoHighlight_);

			PerBatchConstants highlightConstants = MakeEditorSelectionConstants(
				viewProj,
				dirLightViewProj,
				camPosLocal,
				camFLocal,
				highlightColor);
			ctx.commandList.SetConstants(0, std::as_bytes(std::span{ &highlightConstants, 1 }));
			ctx.commandList.DrawIndexed(s.mesh->indexCount, s.mesh->indexType, 0, 0, 1, startInstance);
			ctx.commandList.SetState(restoreState);
		}
	};

#include "DirectX12Renderer_RenderFrame_04_SharedMaterialEnvHelpers.inl"