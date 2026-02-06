module;

#include <cstdint>
#include <vector>
#include <string>
#include <span>

export module core:mesh;

import :rhi;

export namespace rendern
{

	struct VertexDesc
	{
		float px, py, pz;
		float nx, ny, nz;
		float u, v;
	};

	constexpr std::uint32_t strideVDBytes = static_cast<std::uint32_t>(sizeof(VertexDesc));

	struct MeshCPU
	{
		std::vector<VertexDesc> vertices;
		std::vector<std::uint32_t> indices;
	};

	struct MeshRHI
	{
		rhi::BufferHandle vertexBuffer;
		rhi::BufferHandle indexBuffer;
		// Base (per-vertex) layout: POSITION/NORMAL/TEXCOORD0
		rhi::InputLayoutHandle layout;
		// Instanced layout: base slot0 + model matrix (4x float4) in slot1 (DX12 only)
		rhi::InputLayoutHandle layoutInstanced;

		std::uint32_t vertexStrideBytes{ sizeof(VertexDesc) };
		std::uint32_t indexCount{ 0 };
		rhi::IndexType indexType{ rhi::IndexType::UINT32 };
	};

	inline rhi::InputLayoutHandle CreateVertexDescLayout(rhi::IRHIDevice& device, std::string_view name = "VertexDecs")
	{
		rhi::InputLayoutDesc desc{};
		desc.debugName = std::string(name);
		desc.strideBytes = strideVDBytes;
		desc.attributes = {
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::Position, .semanticIndex = 0, .format = rhi::VertexFormat::R32G32B32_FLOAT, .offsetBytes = 0},
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::Normal,	.semanticIndex = 0, .format = rhi::VertexFormat::R32G32B32_FLOAT, .offsetBytes = 12},
			rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::TexCoord, .semanticIndex = 0, .format = rhi::VertexFormat::R32G32_FLOAT,	  .offsetBytes = 24},
		};
		return device.CreateInputLayout(desc);
	}

	inline rhi::InputLayoutHandle CreateVertexDescLayoutInstanced(rhi::IRHIDevice& device, std::string_view name = "VertexDescInstanced")
	{
		rhi::InputLayoutDesc desc{};
		desc.debugName = std::string(name);
		desc.strideBytes = strideVDBytes; // slot0 stride
		desc.attributes = {
		rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::Position,.semanticIndex = 0,.format = rhi::VertexFormat::R32G32B32_FLOAT,.inputSlot = 0,.offsetBytes = 0},
		rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::Normal,  .semanticIndex = 0,.format = rhi::VertexFormat::R32G32B32_FLOAT,.inputSlot = 0,.offsetBytes = 12},
		rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::TexCoord,.semanticIndex = 0,.format = rhi::VertexFormat::R32G32_FLOAT,    .inputSlot = 0,.offsetBytes = 24},

		// Instance matrix columns in slot1: TEXCOORD1..4
		rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::TexCoord,.semanticIndex = 1,.format = rhi::VertexFormat::R32G32B32A32_FLOAT,.inputSlot = 1,.offsetBytes = 0},
		rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::TexCoord,.semanticIndex = 2,.format = rhi::VertexFormat::R32G32B32A32_FLOAT,.inputSlot = 1,.offsetBytes = 16},
		rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::TexCoord,.semanticIndex = 3,.format = rhi::VertexFormat::R32G32B32A32_FLOAT,.inputSlot = 1,.offsetBytes = 32},
		rhi::VertexAttributeDesc{.semantic = rhi::VertexSemantic::TexCoord,.semanticIndex = 4,.format = rhi::VertexFormat::R32G32B32A32_FLOAT,.inputSlot = 1,.offsetBytes = 48},
		};
		return device.CreateInputLayout(desc);
	}

	inline MeshRHI UploadMesh(rhi::IRHIDevice& device, const MeshCPU& cpu, std::string_view debugName = "Mesh")
	{
		MeshRHI outMeshRHI;
		outMeshRHI.vertexStrideBytes = strideVDBytes;
		outMeshRHI.indexCount = static_cast<std::uint32_t>(cpu.indices.size());
		
		outMeshRHI.layout = CreateVertexDescLayout(device, debugName);
		if (device.GetBackend() == rhi::Backend::DirectX12)
		{
			outMeshRHI.layoutInstanced = CreateVertexDescLayoutInstanced(device, std::string(debugName) + "_Instanced");
		}
		else
		{
			outMeshRHI.layoutInstanced = outMeshRHI.layout;
		}

		// Vertex buffer
		{
			rhi::BufferDesc vertexBuffer{};
			vertexBuffer.bindFlag = rhi::BufferBindFlag::VertexBuffer;
			vertexBuffer.usageFlag = rhi::BufferUsageFlag::Static;
			vertexBuffer.sizeInBytes = cpu.vertices.size() * sizeof(VertexDesc);
			vertexBuffer.debugName = std::string(debugName) + "_VB";

			outMeshRHI.vertexBuffer = device.CreateBuffer(vertexBuffer);
			if (!cpu.vertices.empty())
			{
				device.UpdateBuffer(outMeshRHI.vertexBuffer, std::as_bytes(std::span(cpu.vertices)));
			}
		}

		// Index Buffer
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

	inline void DestroyMesh(rhi::IRHIDevice& device, MeshRHI& mesh) noexcept
	{
		if (mesh.indexBuffer)
		{
			device.DestroyBuffer(mesh.indexBuffer);
		}
		if (mesh.vertexBuffer)
		{
			device.DestroyBuffer(mesh.vertexBuffer);
		}
		if (mesh.layoutInstanced && mesh.layoutInstanced.id != mesh.layout.id)
		{
			device.DestroyInputLayout(mesh.layoutInstanced);
		}
		if (mesh.layout)
		{
			device.DestroyInputLayout(mesh.layout);
		}
		mesh = {};
	}

	MeshCPU MakeSkyboxCubeCPU()
	{
		using rendern::VertexDesc;
		rendern::MeshCPU cpu{};

		cpu.vertices = {
			// px,py,pz,  nx,ny,nz,   u,v
			VertexDesc{-1,-1,-1, 0,0,0, 0,0},
			VertexDesc{ 1,-1,-1, 0,0,0, 0,0},
			VertexDesc{ 1, 1,-1, 0,0,0, 0,0},
			VertexDesc{-1, 1,-1, 0,0,0, 0,0},
			VertexDesc{-1,-1, 1, 0,0,0, 0,0},
			VertexDesc{ 1,-1, 1, 0,0,0, 0,0},
			VertexDesc{ 1, 1, 1, 0,0,0, 0,0},
			VertexDesc{-1, 1, 1, 0,0,0, 0,0},
		};

		cpu.indices = {
			// -Z
			0,1,2,  2,3,0,
			// +Z
			4,6,5,  6,4,7,
			// -X
			4,0,3,  3,7,4,
			// +X
			1,5,6,  6,2,1,
			// -Y
			4,5,1,  1,0,4,
			// +Y
			3,2,6,  6,7,3
		};

		return cpu;
	}
}