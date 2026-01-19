module;

#include <algorithm>
#include <vector>

export module core:sync;

import :rhi;

export namespace render
{
	struct FrameSyncDesc
	{
		int framesInRuntime{ 2 };
	};

	class FrameSync
	{
	public:
		FrameSync(rhi::IRHIDevice& device, FrameSyncDesc desc = {})
			: device_(device)
			, frameInRuntime_(std::max(1, desc.framesInRuntime))
		{
			frameFences_.reserve(static_cast<std::size_t>(frameInRuntime_));
			for (int i = 0; i < frameInRuntime_; ++i)
			{
				frameFences_.push_back(device_.CreateFence(true));
			}
		}

		~FrameSync()
		{
			for (auto& fence : frameFences_)
			{
				device_.DestroyFence(fence);
			}
		}

		FrameSync(const FrameSync&) = delete;
		FrameSync& operator=(const FrameSync&) = delete;

		int GetFrameInRuntime() const
		{
			return frameInRuntime_;
		}

		std::uint16_t BeginFrame() const
		{
			const auto idx = currentFrame_;
			auto fence = frameFences_[static_cast<std::size_t>(idx % static_cast<std::uint64_t>(frameInRuntime_))];
			device_.WaitFence(fence);
			return idx;
		}

		void EndFrame()
		{
			auto fence = frameFences_[static_cast<std::size_t>(currentFrame_ % static_cast<std::uint64_t>(frameInRuntime_))];
			device_.SignalFence(fence);
			++currentFrame_;
		}

	private:
		rhi::IRHIDevice& device_;
		int frameInRuntime_{ 2 };
		std::vector<rhi::FenceHandle> frameFences_;
		std::uint64_t currentFrame_{ 0 };
	};
}


