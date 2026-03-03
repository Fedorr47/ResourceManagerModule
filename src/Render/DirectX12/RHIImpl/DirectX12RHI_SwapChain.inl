struct DX12SwapChainDesc
{
    SwapChainDesc base{};
    HWND hwnd{ nullptr };
    std::uint32_t bufferCount{ 2 };
};
class DX12SwapChain final : public IRHISwapChain
{
public:
    DX12SwapChain(DX12Device& device, DX12SwapChainDesc desc);
    ~DX12SwapChain() override;

    SwapChainDesc GetDesc() const override;
    FrameBufferHandle GetCurrentBackBuffer() const override;
    TextureHandle GetDepthTexture() const override;
    void Present() override;
    void Resize(Extent2D newExtent) override;

    std::uint32_t FrameIndex() const noexcept { return static_cast<std::uint32_t>(currBackBuffer_); }

    ID3D12Resource* CurrentBackBuffer() const { return backBuffers_[currBackBuffer_].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTV() const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(currBackBuffer_) * rtvInc_;
        return handle;
    }

    DXGI_FORMAT BackBufferFormat() const { return bbFormat_; }

    D3D12_RESOURCE_STATES& CurrentBackBufferState()
    {
        return backBufferStates_[currBackBuffer_];
    }

    const D3D12_RESOURCE_STATES& CurrentBackBufferState() const
    {
        return backBufferStates_[currBackBuffer_];
    }

    void ResetBackBufferStates(D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT)
    {
        for (auto& st : backBufferStates_)
        {
            st = state;
        }
    }


    void EnsureSizeUpToDate();

private:
    DX12Device& device_;
    DX12SwapChainDesc chainSwapDesc_;

    ComPtr<IDXGISwapChain4> swapChain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    UINT rtvInc_{ 0 };

    std::vector<ComPtr<ID3D12Resource>> backBuffers_;
    UINT currBackBuffer_{ 0 };
    DXGI_FORMAT bbFormat_{ DXGI_FORMAT_B8G8R8A8_UNORM };
    TextureHandle depthTexture_{};
    Format depthFormat_{ Format::D24_UNORM_S8_UINT };

    std::vector<D3D12_RESOURCE_STATES> backBufferStates_;
};
