module;

#if defined(_WIN32)
// Prevent Windows headers from defining the `min`/`max` macros which break `std::min`/`std::max`.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>

#include "d3dx12.h"
#endif

export module core:render_core_dx12;

import :resource_manager_core;
import :rhi;
import :rhi_dx12;

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
#endif

struct MipLevelRGBA8
{
	std::uint32_t width{};
	std::uint32_t height{};
	std::vector<std::uint8_t> rgba; // width*height*4
};

std::vector<std::uint8_t> ConvertToRGBA8(const TextureCPUData& cpu, std::uint32_t width, std::uint32_t height)
{
	if (cpu.pixels.empty()) 
	{
		return {};
	}
	const int channel = cpu.channels > 0 ? cpu.channels : 4;
	const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

	std::vector<std::uint8_t> out;
	out.resize(pixelCount * 4u);

	const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(cpu.pixels.data());

	// NOTE: stb usually gives back 3 (RGB), or 4 (RGBA), or 1 (gray)
	for (std::size_t i = 0; i < pixelCount; ++i)
	{
		std::uint8_t r = 255, g = 255, b = 255, a = 255;

		if (cpu.format == TextureFormat::GRAYSCALE || channel == 1)
		{
			const std::uint8_t v = src[i * static_cast<std::size_t>(channel) + 0];
			r = g = b = v;
			a = 255;
		}
		else if (channel >= 3)
		{
			r = src[i * static_cast<std::size_t>(channel) + 0];
			g = src[i * static_cast<std::size_t>(channel) + 1];
			b = src[i * static_cast<std::size_t>(channel) + 2];
			a = (channel >= 4) ? src[i * static_cast<std::size_t>(channel) + 3] : 255;
		}
		else
		{
			// fallback
			const std::uint8_t v = src[i * static_cast<std::size_t>(channel) + 0];
			r = g = b = v;
			a = 255;
		}

		out[i * 4u + 0] = r;
		out[i * 4u + 1] = g;
		out[i * 4u + 2] = b;
		out[i * 4u + 3] = a;
	}

	return out;
}

std::vector<MipLevelRGBA8> MakeMipChain_Box2x2_RGBA8(
	const std::vector<std::uint8_t>& mip0,
	std::uint32_t width0,
	std::uint32_t height0,
	bool genMips)
{
	std::vector<MipLevelRGBA8> chain;

	MipLevelRGBA8 base{};
	base.width = width0;
	base.height = height0;
	base.rgba = mip0;
	chain.push_back(std::move(base));

	if (!genMips)
	{
		return chain;
	}

	std::uint32_t curW = width0;
	std::uint32_t curH = height0;

	while (curW > 1 || curH > 1)
	{
		const std::uint32_t nextW = std::max(1u, curW / 2u);
		const std::uint32_t nextH = std::max(1u, curH / 2u);

		const auto& prev = chain.back();

		MipLevelRGBA8 next{};
		next.width = nextW;
		next.height = nextH;
		next.rgba.resize(static_cast<std::size_t>(nextW) * static_cast<std::size_t>(nextH) * 4u);

		for (std::uint32_t y = 0; y < nextH; ++y)
		{
			for (std::uint32_t x = 0; x < nextW; ++x)
			{
				std::uint32_t acc[4] = { 0,0,0,0 };
				std::uint32_t cnt = 0;

				for (std::uint32_t ky = 0; ky < 2; ++ky)
				{
					for (std::uint32_t kx = 0; kx < 2; ++kx)
					{
						const std::uint32_t sx = std::min(curW - 1, x * 2 + kx);
						const std::uint32_t sy = std::min(curH - 1, y * 2 + ky);

						const std::size_t si = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(curW) + static_cast<std::size_t>(sx)) * 4u;
						acc[0] += prev.rgba[si + 0];
						acc[1] += prev.rgba[si + 1];
						acc[2] += prev.rgba[si + 2];
						acc[3] += prev.rgba[si + 3];
						++cnt;
					}
				}

				const std::size_t di = (static_cast<std::size_t>(y) * static_cast<std::size_t>(nextW) + static_cast<std::size_t>(x)) * 4u;
				next.rgba[di + 0] = static_cast<std::uint8_t>((acc[0] / cnt));
				next.rgba[di + 1] = static_cast<std::uint8_t>((acc[1] / cnt));
				next.rgba[di + 2] = static_cast<std::uint8_t>((acc[2] / cnt));
				next.rgba[di + 3] = static_cast<std::uint8_t>((acc[3] / cnt));
			}
		}

		chain.push_back(std::move(next));
		curW = nextW;
		curH = nextH;
	}

	return chain;
}

DXGI_FORMAT DxgiRGBA8(bool srgb)
{
	return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
}

export namespace rendern
{
#if defined(_WIN32)

	export class DX12TextureUploader final : public ITextureUploader
	{
	public:
		explicit DX12TextureUploader(rhi::IRHIDevice& device)
			: device_(device)
		{
		}

		std::optional<GPUTexture> CreateAndUpload(const TextureCPUData& cpuData, const TextureProperties& properties) override
		{
			// 1) Validate
			const std::uint32_t width = cpuData.width ? cpuData.width : properties.width;
			const std::uint32_t height = cpuData.height ? cpuData.height : properties.height;

			if (width == 0 || height == 0 || cpuData.pixels.empty())
			{
				return std::nullopt;
			}
				

			// 2) Need DX12 device
			auto* dxDev = dynamic_cast<rhi::DX12Device*>(&device_);
			if (!dxDev)
			{
				return std::nullopt;
			}
			
			ID3D12Device* d3d = dxDev->NativeDevice();
			ID3D12CommandQueue* queue = dxDev->NativeQueue();
			if (!d3d || !queue)
			{
				return std::nullopt;
			}
			
			// 3) Convert to RGBA8 and build mip chain
			auto rgba0 = ConvertToRGBA8(cpuData, width, height);
			if (rgba0.empty())
			{
				return std::nullopt;
			}

			auto mips = MakeMipChain_Box2x2_RGBA8(rgba0, width, height, properties.generateMips);

			const DXGI_FORMAT fmt = DxgiRGBA8(properties.srgb);
			const UINT mipLevels = static_cast<UINT>(mips.size());

			// 4) Create default texture with mip levels
			D3D12_RESOURCE_DESC texDesc{};
			texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

			texDesc.Alignment = 0;
			texDesc.Width = width;
			texDesc.Height = height;
			texDesc.DepthOrArraySize = 1;
			texDesc.MipLevels = static_cast<UINT16>(mipLevels);
			texDesc.Format = fmt;
			texDesc.SampleDesc.Count = 1;
			texDesc.SampleDesc.Quality = 0;
			texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			ComPtr<ID3D12Resource> texture;
			{
				D3D12_HEAP_PROPERTIES heapProps{};
				heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

				ThrowIfFailed(
					d3d->CreateCommittedResource(
						&heapProps,
						D3D12_HEAP_FLAG_NONE,
						&texDesc,
						D3D12_RESOURCE_STATE_COPY_DEST,
						nullptr,
						IID_PPV_ARGS(&texture)),
					"DX12TextureUploader: CreateCommittedResource(texture) failed");
			}

			// 5) Create upload buffer
			UINT64 uploadBytes = 0;
			d3d->GetCopyableFootprints(&texDesc, 0, mipLevels, 0, nullptr, nullptr, nullptr, &uploadBytes);

			ComPtr<ID3D12Resource> upload;
			{
				D3D12_HEAP_PROPERTIES uploadProps{};
				uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;

				auto upDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBytes);

				ThrowIfFailed(
					d3d->CreateCommittedResource(
						&uploadProps,
						D3D12_HEAP_FLAG_NONE,
						&upDesc,
						D3D12_RESOURCE_STATE_GENERIC_READ,
						nullptr,
						IID_PPV_ARGS(&upload)),
					"DX12TextureUploader: CreateCommittedResource(upload) failed");
			}

			// 6) Prepare subresources
			std::vector<D3D12_SUBRESOURCE_DATA> subs;
			subs.reserve(mipLevels);
			for (UINT i = 0; i < mipLevels; ++i)
			{
				const auto& mipInst = mips[i];
				D3D12_SUBRESOURCE_DATA subResData{};
				subResData.pData = mipInst.rgba.data();
				subResData.RowPitch = static_cast<LONG_PTR>(mipInst.width) * 4;
				subResData.SlicePitch = subResData.RowPitch * static_cast<LONG_PTR>(mipInst.height);
				subs.push_back(subResData);
			}

			// 7) Record copy commands into a temporary list
			ComPtr<ID3D12CommandAllocator> alloc;
			ThrowIfFailed(
				d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
				"DX12TextureUploader: CreateCommandAllocator failed");

			ComPtr<ID3D12GraphicsCommandList> list;
			ThrowIfFailed(
				d3d->CreateCommandList(
				0, 
				D3D12_COMMAND_LIST_TYPE_DIRECT, 
				alloc.Get(),
				nullptr, 
				IID_PPV_ARGS(&list)),
				"DX12TextureUploader: CreateCommandList failed");

			UpdateSubresources(list.Get(), texture.Get(), upload.Get(), 0, 0, mipLevels, subs.data());

			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				texture.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			list->ResourceBarrier(1, &barrier);

			ThrowIfFailed(list->Close(), "DX12TextureUploader: Close cmdlist failed");

			ID3D12CommandList* lists[] = { list.Get() };
			queue->ExecuteCommandLists(1, lists);

			// 8) Fence wait (чтобы upload можно было сразу освободить и текстура гарантированно готова)
			ComPtr<ID3D12Fence> fence;
			ThrowIfFailed(d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
				"DX12TextureUploader: CreateFence failed");

			HANDLE eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (!eventHandle)
			{
				return std::nullopt;
			}

			const UINT64 fv = 1;
			ThrowIfFailed(queue->Signal(fence.Get(), fv), "DX12TextureUploader: Signal failed");
			if (fence->GetCompletedValue() < fv)
			{
				ThrowIfFailed(fence->SetEventOnCompletion(fv, eventHandle), "DX12TextureUploader: SetEventOnCompletion failed");
				WaitForSingleObject(eventHandle, INFINITE);
			}
			CloseHandle(eventHandle);

			// 9) Register inside DX12 RHI as a TextureHandle and create SRV in device's heap
			//    IMPORTANT: DX12Device must take ownership (AddRef/ComPtr::Attach).
			const rhi::TextureHandle handle = dxDev->RegisterSampledTexture(texture.Get(), fmt, mipLevels);

			if (!handle)
			{
				return std::nullopt;
			}	

			// Ownership is transferred to DX12Device inside RegisterSampledTexture().
			// We can safely release local ComPtr (it will Release its ref; device holds its own ref).
			// (do nothing)

			return GPUTexture{ static_cast<unsigned int>(handle.id) };
		}

		void Destroy(GPUTexture texture) noexcept override
		{
			if (texture.id == 0) 
			{
				return;
			}

			rhi::TextureHandle textureHandle{};
			textureHandle.id = static_cast<std::uint32_t>(texture.id);
			device_.DestroyTexture(textureHandle);
		}

	private:
		rhi::IRHIDevice& device_;
	};

#else // !_WIN32

	export class DX12TextureUploader final : public ITextureUploader
	{
	public:
		explicit DX12TextureUploader(rhi::IRHIDevice&) {}
		std::optional<GPUTexture> CreateAndUpload(const TextureCPUData&, const TextureProperties&) override { return std::nullopt; }
		void Destroy(GPUTexture) noexcept override {}
	};

#endif
} // namespace rendern
