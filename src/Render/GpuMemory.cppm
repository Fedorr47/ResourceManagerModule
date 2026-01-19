module;

#include <cstddef>

export module core:render_gpu_memory;

import :rhi;

// GPU memory abstraction
//
// Most of APIs require explicit memory management for buffers and textures.
// This module defines a small allocator interface that higher-level systems can depend on
// without being tied to a specific RHI backend implementation.

export namespace renderer
{
	struct BufferAllocation 
	{
		rhi::BufferHandle buffer;
		std::size_t offsetBytes;
		std::size_t sizeInBytes;
	};

	class IGPUMemoryAllocator
	{
	public:
		virtual ~IGPUMemoryAllocator() = default;

		virtual BufferAllocation AllocateBuffer(const rhi::BufferDesc& desc) = 0;
		virtual void FreeBuffer(const BufferAllocation& allocation) noexcept = 0;
	};

	class NullGPUMemoryAllocator final : public IGPUMemoryAllocator
	{
	public:
		explicit NullGPUMemoryAllocator(rhi::IRHIDevice& device) : device_(device) {}

		BufferAllocation AllocateBuffer(const rhi::BufferDesc& desc) override
		{
			rhi::BufferHandle buffer = device_.CreateBuffer(desc);
			return BufferAllocation{ .buffer = buffer, .offsetBytes = 0, .sizeInBytes = desc.sizeInBytes };
		}

		void FreeBuffer(const BufferAllocation& allocation) noexcept override
		{
			if (allocation.buffer.id != 0)

			device_.DestroyBuffer(allocation.buffer);
		}

	private:
		rhi::IRHIDevice& device_;
	};
}