module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module core:skinned_mesh;

import :rhi;
import :math_utils;
import :skeleton;

export namespace rendern
{
	inline constexpr std::uint32_t kMaxSkinWeightsPerVertex = 4;

	struct SkinnedVertexDesc
	{
		float px, py, pz;
		float nx, ny, nz;
		float u, v;
		float tx, ty, tz, tw;
		std::uint16_t boneIndex0{ 0 };
		std::uint16_t boneIndex1{ 0 };
		std::uint16_t boneIndex2{ 0 };
		std::uint16_t boneIndex3{ 0 };
		float boneWeight0{ 1.0f };
		float boneWeight1{ 0.0f };
		float boneWeight2{ 0.0f };
		float boneWeight3{ 0.0f };
	};

	constexpr std::uint32_t strideSkinnedVDBytes = static_cast<std::uint32_t>(sizeof(SkinnedVertexDesc));

	struct SkinnedSubmesh
	{
		std::string name;
		std::uint32_t firstIndex{ 0 };
		std::uint32_t indexCount{ 0 };
		std::uint32_t materialIndex{ 0 };
	};

	struct SkinnedBounds
	{
		mathUtils::Vec3 aabbMin{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 aabbMax{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 sphereCenter{ 0.0f, 0.0f, 0.0f };
		float sphereRadius{ 0.0f };
	};

	struct PerClipBounds
	{
		std::string clipName;
		SkinnedBounds bounds{};
	};

	struct SkinnedMeshBounds
	{
		SkinnedBounds bindPoseBounds{};
		SkinnedBounds maxAnimatedBounds{};
		std::vector<PerClipBounds> perClipBounds;
	};

	struct SkinnedMeshCPU
	{
		std::vector<SkinnedVertexDesc> vertices;
		std::vector<std::uint32_t> indices;
		std::vector<SkinnedSubmesh> submeshes;
		Skeleton skeleton{};
		SkinnedMeshBounds bounds{};
	};

	struct SkinnedMeshRHI
	{
		rhi::BufferHandle vertexBuffer;
		rhi::BufferHandle indexBuffer;
		rhi::InputLayoutHandle layout;

		std::uint32_t vertexStrideBytes{ sizeof(SkinnedVertexDesc) };
		std::uint32_t indexCount{ 0 };
		rhi::IndexType indexType{ rhi::IndexType::UINT32 };
	};

	inline rhi::InputLayoutHandle CreateSkinnedVertexDescLayout(rhi::IRHIDevice& device, std::string_view name = "SkinnedVertexDesc")
	{
		rhi::InputLayoutDesc desc{};
		desc.debugName = std::string(name);
		desc.strideBytes = strideSkinnedVDBytes;
		desc.attributes = {
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::Position,    .semanticIndex = 0, .format = rhi::VertexFormat::R32G32B32_FLOAT,    .offsetBytes = static_cast<std::uint32_t>(offsetof(SkinnedVertexDesc, px))},
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::Normal,      .semanticIndex = 0, .format = rhi::VertexFormat::R32G32B32_FLOAT,    .offsetBytes = static_cast<std::uint32_t>(offsetof(SkinnedVertexDesc, nx))},
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::TexCoord,    .semanticIndex = 0, .format = rhi::VertexFormat::R32G32_FLOAT,       .offsetBytes = static_cast<std::uint32_t>(offsetof(SkinnedVertexDesc, u))},
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::Tangent,     .semanticIndex = 0, .format = rhi::VertexFormat::R32G32B32A32_FLOAT, .offsetBytes = static_cast<std::uint32_t>(offsetof(SkinnedVertexDesc, tx))},
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::BoneIndices, .semanticIndex = 0, .format = rhi::VertexFormat::R16G16B16A16_UINT,  .offsetBytes = static_cast<std::uint32_t>(offsetof(SkinnedVertexDesc, boneIndex0))},
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::BoneWeights, .semanticIndex = 0, .format = rhi::VertexFormat::R32G32B32A32_FLOAT, .offsetBytes = static_cast<std::uint32_t>(offsetof(SkinnedVertexDesc, boneWeight0))},
		};
		return device.CreateInputLayout(desc);
	}

	inline void NormalizeBoneWeights(SkinnedVertexDesc& vertex) noexcept
	{
		std::array<float, kMaxSkinWeightsPerVertex> weights = {
			std::max(vertex.boneWeight0, 0.0f),
			std::max(vertex.boneWeight1, 0.0f),
			std::max(vertex.boneWeight2, 0.0f),
			std::max(vertex.boneWeight3, 0.0f)
		};

		const float sum = weights[0] + weights[1] + weights[2] + weights[3];
		if (sum > 1e-8f)
		{
			const float invSum = 1.0f / sum;
			for (float& w : weights)
			{
				w *= invSum;
			}
		}
		else
		{
			weights = { 1.0f, 0.0f, 0.0f, 0.0f };
			vertex.boneIndex0 = 0;
			vertex.boneIndex1 = 0;
			vertex.boneIndex2 = 0;
			vertex.boneIndex3 = 0;
		}

		vertex.boneWeight0 = weights[0];
		vertex.boneWeight1 = weights[1];
		vertex.boneWeight2 = weights[2];
		vertex.boneWeight3 = weights[3];
	}

	inline void NormalizeBoneWeights(SkinnedMeshCPU& cpu) noexcept
	{
		for (auto& v : cpu.vertices)
		{
			NormalizeBoneWeights(v);
		}
	}

	[[nodiscard]] inline SkinnedBounds ComputeBindPoseBounds(const SkinnedMeshCPU& cpu) noexcept
	{
		SkinnedBounds b{};
		if (cpu.vertices.empty())
		{
			return b;
		}

		const auto& v0 = cpu.vertices.front();
		float minX = v0.px, minY = v0.py, minZ = v0.pz;
		float maxX = v0.px, maxY = v0.py, maxZ = v0.pz;
		for (const auto& v : cpu.vertices)
		{
			minX = std::min(minX, v.px); minY = std::min(minY, v.py); minZ = std::min(minZ, v.pz);
			maxX = std::max(maxX, v.px); maxY = std::max(maxY, v.py); maxZ = std::max(maxZ, v.pz);
		}

		b.aabbMin = mathUtils::Vec3(minX, minY, minZ);
		b.aabbMax = mathUtils::Vec3(maxX, maxY, maxZ);
		b.sphereCenter = (b.aabbMin + b.aabbMax) * 0.5f;
		const mathUtils::Vec3 ext = b.aabbMax - b.sphereCenter;
		b.sphereRadius = mathUtils::Length(ext);
		return b;
	}

	inline void RefreshBindPoseBounds(SkinnedMeshCPU& cpu) noexcept
	{
		cpu.bounds.bindPoseBounds = ComputeBindPoseBounds(cpu);
		if (cpu.bounds.maxAnimatedBounds.sphereRadius <= 0.0f)
		{
			cpu.bounds.maxAnimatedBounds = cpu.bounds.bindPoseBounds;
		}
	}

	inline SkinnedMeshRHI UploadSkinnedMesh(rhi::IRHIDevice& device, const SkinnedMeshCPU& cpu, std::string_view debugName = "SkinnedMesh")
	{
		SkinnedMeshRHI outMeshRHI{};
		outMeshRHI.vertexStrideBytes = strideSkinnedVDBytes;
		outMeshRHI.indexCount = static_cast<std::uint32_t>(cpu.indices.size());
		outMeshRHI.layout = CreateSkinnedVertexDescLayout(device, debugName);

		{
			rhi::BufferDesc vertexBuffer{};
			vertexBuffer.bindFlag = rhi::BufferBindFlag::VertexBuffer;
			vertexBuffer.usageFlag = rhi::BufferUsageFlag::Static;
			vertexBuffer.sizeInBytes = cpu.vertices.size() * sizeof(SkinnedVertexDesc);
			vertexBuffer.debugName = std::string(debugName) + "_VB";

			outMeshRHI.vertexBuffer = device.CreateBuffer(vertexBuffer);
			if (!cpu.vertices.empty())
			{
				device.UpdateBuffer(outMeshRHI.vertexBuffer, std::as_bytes(std::span(cpu.vertices)));
			}
		}

		{
			rhi::BufferDesc indexBuffer{};
			indexBuffer.bindFlag = rhi::BufferBindFlag::IndexBuffer;
			indexBuffer.usageFlag = rhi::BufferUsageFlag::Static;
			indexBuffer.sizeInBytes = cpu.indices.size() * sizeof(std::uint32_t);
			indexBuffer.debugName = std::string(debugName) + "_IB";

			outMeshRHI.indexBuffer = device.CreateBuffer(indexBuffer);
			if (!cpu.indices.empty())
			{
				device.UpdateBuffer(outMeshRHI.indexBuffer, std::as_bytes(std::span(cpu.indices)));
			}
		}

		return outMeshRHI;
	}

	inline void DestroySkinnedMesh(rhi::IRHIDevice& device, SkinnedMeshRHI& mesh) noexcept
	{
		if (mesh.indexBuffer)
		{
			device.DestroyBuffer(mesh.indexBuffer);
		}
		if (mesh.vertexBuffer)
		{
			device.DestroyBuffer(mesh.vertexBuffer);
		}
		if (mesh.layout)
		{
			device.DestroyInputLayout(mesh.layout);
		}
		mesh = {};
	}
}