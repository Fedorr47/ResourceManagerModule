module;

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module core:skinned_mesh;

import :rhi;
import :math_utils;
import :animation_clip;
import :skeleton;

export namespace rendern
{
	constexpr std::uint32_t kMaxSkinWeightsPerVertex = 4u;

	struct SkinnedBounds
	{
		mathUtils::Vec3 aabbMin{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 aabbMax{ 0.0f, 0.0f, 0.0f };
		mathUtils::Vec3 sphereCenter{ 0.0f, 0.0f, 0.0f };
		float sphereRadius{ 0.0f };
	};

	struct PerClipBounds
	{
		std::string clipName{};
		SkinnedBounds bounds{};
	};

	struct SkinnedMeshBounds
	{
		SkinnedBounds bindPoseBounds{};
		SkinnedBounds maxAnimatedBounds{};
		std::vector<PerClipBounds> perClipBounds{};
	};

	struct SkinnedSubmesh
	{
		std::string name{};
		std::uint32_t firstIndex{ 0 };
		std::uint32_t indexCount{ 0 };
		std::uint32_t materialIndex{ 0 };
	};

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

	constexpr std::uint32_t skinnedStrideVDBytes = static_cast<std::uint32_t>(sizeof(SkinnedVertexDesc));

	struct SkinnedMeshCPU
	{
		std::vector<SkinnedVertexDesc> vertices{};
		std::vector<std::uint32_t> indices{};
		std::vector<SkinnedSubmesh> submeshes{};
		Skeleton skeleton{};
		// Converts skeleton-global skinning output back into the mesh local space
		// expected by the renderer before the node/world transform is applied.
		mathUtils::Mat4 skinningSkeletonToMeshSpace{ 1.0f };
		SkinnedMeshBounds bounds{};
	};

	struct SkinnedMeshRHI
	{
		rhi::BufferHandle vertexBuffer{};
		rhi::BufferHandle indexBuffer{};
		rhi::InputLayoutHandle layout{};
		std::uint32_t vertexStrideBytes{ skinnedStrideVDBytes };
		std::uint32_t indexCount{ 0 };
		rhi::IndexType indexType{ rhi::IndexType::UINT32 };
	};

	struct ExternalAnimationSourceInfo
	{
		std::string assetId{};
		std::string debugName{};
		std::size_t clipCount{ 0 };
		std::size_t sourceChannelCount{ 0 };
		std::size_t matchedChannelCount{ 0 };
		std::size_t ignoredChannelCount{ 0 };
		std::string diagnosticMessage{};
	};

	struct SkinnedAssetBundle
	{
		std::string debugName{};
		SkinnedMeshCPU mesh{};
		std::vector<AnimationClip> clips{};
		std::vector<std::string> clipSourceAssetIds{}; // empty for embedded clips
		std::vector<ExternalAnimationSourceInfo> externalAnimationSources{};
	};

	inline void NormalizeBoneWeights(SkinnedVertexDesc& v) noexcept
	{
		float sum =
			v.boneWeight0 +
			v.boneWeight1 +
			v.boneWeight2 +
			v.boneWeight3;

		if (sum <= 1e-8f)
		{
			v.boneIndex0 = 0; v.boneWeight0 = 1.0f;
			v.boneIndex1 = 0; v.boneWeight1 = 0.0f;
			v.boneIndex2 = 0; v.boneWeight2 = 0.0f;
			v.boneIndex3 = 0; v.boneWeight3 = 0.0f;
			return;
		}

		const float inv = 1.0f / sum;
		v.boneWeight0 *= inv;
		v.boneWeight1 *= inv;
		v.boneWeight2 *= inv;
		v.boneWeight3 *= inv;
	}

	[[nodiscard]] inline SkinnedBounds ComputeSkinnedBoundsFromVertices(const SkinnedMeshCPU& cpu)
	{
		SkinnedBounds b{};
		if (cpu.vertices.empty())
		{
			return b;
		}

		mathUtils::Vec3 mn(cpu.vertices[0].px, cpu.vertices[0].py, cpu.vertices[0].pz);
		mathUtils::Vec3 mx = mn;

		for (const auto& v : cpu.vertices)
		{
			const mathUtils::Vec3 p(v.px, v.py, v.pz);
			mn.x = std::min(mn.x, p.x);
			mn.y = std::min(mn.y, p.y);
			mn.z = std::min(mn.z, p.z);

			mx.x = std::max(mx.x, p.x);
			mx.y = std::max(mx.y, p.y);
			mx.z = std::max(mx.z, p.z);
		}

		b.aabbMin = mn;
		b.aabbMax = mx;
		b.sphereCenter = (mn + mx) * 0.5f;
		b.sphereRadius = 0.0f;

		for (const auto& v : cpu.vertices)
		{
			const mathUtils::Vec3 p(v.px, v.py, v.pz);
			b.sphereRadius = std::max(b.sphereRadius, mathUtils::Length(p - b.sphereCenter));
		}

		return b;
	}

	inline void RefreshBindPoseBounds(SkinnedMeshCPU& cpu)
	{
		cpu.bounds.bindPoseBounds = ComputeSkinnedBoundsFromVertices(cpu);
		if (cpu.bounds.maxAnimatedBounds.sphereRadius <= 0.0f)
		{
			cpu.bounds.maxAnimatedBounds = cpu.bounds.bindPoseBounds;
		}
	}

	[[nodiscard]] inline mathUtils::Vec3 BuildFallbackTangent(const mathUtils::Vec3& normal) noexcept
	{
		const mathUtils::Vec3 axis = (std::abs(normal.z) < 0.999f)
			? mathUtils::Vec3(0.0f, 0.0f, 1.0f)
			: mathUtils::Vec3(0.0f, 1.0f, 0.0f);

		return mathUtils::Normalize(mathUtils::Cross(axis, normal));
	}

	inline void ComputeTangents(SkinnedMeshCPU& cpu)
	{
		if (cpu.vertices.empty())
		{
			return;
		}

		std::vector<mathUtils::Vec3> tan1(cpu.vertices.size(), mathUtils::Vec3(0.0f, 0.0f, 0.0f));
		std::vector<mathUtils::Vec3> tan2(cpu.vertices.size(), mathUtils::Vec3(0.0f, 0.0f, 0.0f));

		for (std::size_t i = 0; i + 2 < cpu.indices.size(); i += 3)
		{
			const std::uint32_t i0 = cpu.indices[i + 0];
			const std::uint32_t i1 = cpu.indices[i + 1];
			const std::uint32_t i2 = cpu.indices[i + 2];
			if (i0 >= cpu.vertices.size() || i1 >= cpu.vertices.size() || i2 >= cpu.vertices.size())
			{
				continue;
			}

			const SkinnedVertexDesc& v0 = cpu.vertices[i0];
			const SkinnedVertexDesc& v1 = cpu.vertices[i1];
			const SkinnedVertexDesc& v2 = cpu.vertices[i2];

			const mathUtils::Vec3 p0(v0.px, v0.py, v0.pz);
			const mathUtils::Vec3 p1(v1.px, v1.py, v1.pz);
			const mathUtils::Vec3 p2(v2.px, v2.py, v2.pz);

			const float x1 = p1.x - p0.x;
			const float x2 = p2.x - p0.x;
			const float y1 = p1.y - p0.y;
			const float y2 = p2.y - p0.y;
			const float z1 = p1.z - p0.z;
			const float z2 = p2.z - p0.z;

			const float s1 = v1.u - v0.u;
			const float s2 = v2.u - v0.u;
			const float t1 = v1.v - v0.v;
			const float t2 = v2.v - v0.v;

			const float det = s1 * t2 - s2 * t1;
			if (std::abs(det) < 1e-8f)
			{
				continue;
			}

			const float invDet = 1.0f / det;
			const mathUtils::Vec3 sdir(
				(t2 * x1 - t1 * x2) * invDet,
				(t2 * y1 - t1 * y2) * invDet,
				(t2 * z1 - t1 * z2) * invDet);
			const mathUtils::Vec3 tdir(
				(s1 * x2 - s2 * x1) * invDet,
				(s1 * y2 - s2 * y1) * invDet,
				(s1 * z2 - s2 * z1) * invDet);

			tan1[i0] = tan1[i0] + sdir;
			tan1[i1] = tan1[i1] + sdir;
			tan1[i2] = tan1[i2] + sdir;

			tan2[i0] = tan2[i0] + tdir;
			tan2[i1] = tan2[i1] + tdir;
			tan2[i2] = tan2[i2] + tdir;
		}

		for (std::size_t i = 0; i < cpu.vertices.size(); ++i)
		{
			SkinnedVertexDesc& v = cpu.vertices[i];
			const mathUtils::Vec3 n = mathUtils::Normalize(mathUtils::Vec3(v.nx, v.ny, v.nz));

			mathUtils::Vec3 t = tan1[i] - n * mathUtils::Dot(n, tan1[i]);
			if (mathUtils::Length(t) < 1e-6f)
			{
				t = BuildFallbackTangent(n);
			}
			else
			{
				t = mathUtils::Normalize(t);
			}

			const float handedness =
				(mathUtils::Dot(mathUtils::Cross(n, t), tan2[i]) < 0.0f) ? -1.0f : 1.0f;

			v.tx = t.x;
			v.ty = t.y;
			v.tz = t.z;
			v.tw = handedness;
		}
	}

	inline rhi::InputLayoutHandle CreateSkinnedVertexDescLayout(
		rhi::IRHIDevice& device,
		std::string_view name = "SkinnedVertexDesc")
	{
		rhi::InputLayoutDesc desc{};
		desc.debugName = std::string(name);
		desc.strideBytes = skinnedStrideVDBytes;
		desc.attributes = {
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::Position,    .semanticIndex = 0, .format = rhi::VertexFormat::R32G32B32_FLOAT,    .offsetBytes = 0  },
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::Normal,      .semanticIndex = 0, .format = rhi::VertexFormat::R32G32B32_FLOAT,    .offsetBytes = 12 },
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::TexCoord,    .semanticIndex = 0, .format = rhi::VertexFormat::R32G32_FLOAT,       .offsetBytes = 24 },
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::Tangent,     .semanticIndex = 0, .format = rhi::VertexFormat::R32G32B32A32_FLOAT,.offsetBytes = 32 },
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::BoneIndices, .semanticIndex = 0, .format = rhi::VertexFormat::R16G16B16A16_UINT, .offsetBytes = 48 },
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::BoneWeights, .semanticIndex = 0, .format = rhi::VertexFormat::R32G32B32A32_FLOAT,.offsetBytes = 56 },
		};
		return device.CreateInputLayout(desc);
	}

	inline SkinnedMeshRHI UploadSkinnedMesh(
		rhi::IRHIDevice& device,
		const SkinnedMeshCPU& cpu,
		std::string_view debugName = "SkinnedMesh")
	{
		SkinnedMeshRHI out{};
		out.vertexStrideBytes = skinnedStrideVDBytes;
		out.indexCount = static_cast<std::uint32_t>(cpu.indices.size());
		out.layout = CreateSkinnedVertexDescLayout(device, debugName);

		rhi::BufferDesc vb{};
		vb.bindFlag = rhi::BufferBindFlag::VertexBuffer;
		vb.usageFlag = rhi::BufferUsageFlag::Static;
		vb.sizeInBytes = cpu.vertices.size() * sizeof(SkinnedVertexDesc);
		vb.debugName = std::string(debugName) + "_VB";
		out.vertexBuffer = device.CreateBuffer(vb);
		if (!cpu.vertices.empty())
		{
			device.UpdateBuffer(out.vertexBuffer, std::as_bytes(std::span(cpu.vertices)));
		}

		rhi::BufferDesc ib{};
		ib.bindFlag = rhi::BufferBindFlag::IndexBuffer;
		ib.usageFlag = rhi::BufferUsageFlag::Static;
		ib.sizeInBytes = cpu.indices.size() * sizeof(std::uint32_t);
		ib.debugName = std::string(debugName) + "_IB";
		out.indexBuffer = device.CreateBuffer(ib);
		if (!cpu.indices.empty())
		{
			device.UpdateBuffer(out.indexBuffer, std::as_bytes(std::span(cpu.indices)));
		}

		return out;
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