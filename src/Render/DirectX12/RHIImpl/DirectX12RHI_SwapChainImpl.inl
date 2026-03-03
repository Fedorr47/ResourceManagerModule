DX12SwapChain::DX12SwapChain(DX12Device& owner, DX12SwapChainDesc desc)
    : device_(owner)
    , chainSwapDesc_(std::move(desc))
{
    if (!chainSwapDesc_.hwnd)
    {
        throw std::runtime_error("DX12SwapChain: hwnd is null");
    }

    ComPtr<IDXGIFactory6> factory;
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DX12: CreateDXGIFactory2 failed");

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width = chainSwapDesc_.base.extent.width;
    swapChainDesc.Height = chainSwapDesc_.base.extent.height;
    swapChainDesc.Format = ToDXGIFormat(chainSwapDesc_.base.backbufferFormat);
    bbFormat_ = swapChainDesc.Format;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = std::max(2u, chainSwapDesc_.bufferCount);
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        device_.NativeQueue(),
        chainSwapDesc_.hwnd,
        &swapChainDesc,
        nullptr, nullptr,
        &swapChain1),
        "DX12: CreateSwapChainForHwnd failed");

    ThrowIfFailed(swapChain1.As(&swapChain_), "DX12: swapchain As IDXGISwapChain4 failed");
    ThrowIfFailed(factory->MakeWindowAssociation(chainSwapDesc_.hwnd, DXGI_MWA_NO_ALT_ENTER), "DX12: MakeWindowAssociation failed");

    // RTV heap for backbuffers
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = swapChainDesc.BufferCount;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(device_.NativeDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_)),
            "DX12: Create swapchain RTV heap failed");
        rtvInc_ = device_.NativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    backBuffers_.resize(swapChainDesc.BufferCount);
    for (UINT i = 0; i < swapChainDesc.BufferCount; ++i)
    {
        ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])),
            "DX12: GetBuffer failed");

        D3D12_CPU_DESCRIPTOR_HANDLE descHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        descHandle.ptr += static_cast<SIZE_T>(i) * rtvInc_;
        device_.NativeDevice()->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, descHandle);
    }

    currBackBuffer_ = swapChain_->GetCurrentBackBufferIndex();

    // Depth+Stencil (swapchain depth as regular RHI texture; typeless + SRV + DSV).
    depthTexture_ = device_.CreateTexture2D(chainSwapDesc_.base.extent, depthFormat_);

    backBufferStates_.resize(backBuffers_.size());
    ResetBackBufferStates(D3D12_RESOURCE_STATE_PRESENT);
    currBackBuffer_ = swapChain_->GetCurrentBackBufferIndex();
}

SwapChainDesc DX12SwapChain::GetDesc() const
{
    return chainSwapDesc_.base;
}

FrameBufferHandle DX12SwapChain::GetCurrentBackBuffer() const
{
    // similar to GL: 0 stands to swapchain backbuffer
    return FrameBufferHandle{ 0 };
}


TextureHandle DX12SwapChain::GetDepthTexture() const
{
    return depthTexture_;
}

DX12SwapChain::~DX12SwapChain()
{
    device_.DestroyTexture(depthTexture_);
    depthTexture_ = TextureHandle{};
}

void DX12SwapChain::Resize(Extent2D newExtent)
{
    // NOTE: ResizeBuffers requires that all references to the swapchain buffers are released.
    if (newExtent.width == 0 || newExtent.height == 0)
    {
        // Minimized / hidden. Keep desc in sync, but don't touch DXGI buffers.
        chainSwapDesc_.base.extent = newExtent;
        return;
    }

    if (newExtent.width == chainSwapDesc_.base.extent.width && newExtent.height == chainSwapDesc_.base.extent.height)
    {
        return;
    }

    // Make sure GPU is not using the current backbuffers/depth.
    device_.WaitIdle();

    // Release current backbuffer/depth resources before ResizeBuffers.
    for (auto& bb : backBuffers_)
    {
        bb.Reset();
    }
    device_.DestroyTexture(depthTexture_);
    depthTexture_ = TextureHandle{};

    const UINT bufferCount = static_cast<UINT>(backBuffers_.size());

    ThrowIfFailed(swapChain_->ResizeBuffers(
        bufferCount,
        static_cast<UINT>(newExtent.width),
        static_cast<UINT>(newExtent.height),
        bbFormat_,
        0),
        "DX12: ResizeBuffers failed");

    // Recreate RTVs.
    for (UINT i = 0; i < bufferCount; ++i)
    {
        ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])),
            "DX12: GetBuffer failed");

        D3D12_CPU_DESCRIPTOR_HANDLE descHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        descHandle.ptr += static_cast<SIZE_T>(i) * rtvInc_;
        device_.NativeDevice()->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, descHandle);
    }

    // Recreate depth-stencil texture (typeless + SRV + DSV).
    depthTexture_ = device_.CreateTexture2D(newExtent, depthFormat_);

    chainSwapDesc_.base.extent = newExtent;

    ResetBackBufferStates(D3D12_RESOURCE_STATE_PRESENT);
    currBackBuffer_ = swapChain_->GetCurrentBackBufferIndex();
}

void DX12SwapChain::EnsureSizeUpToDate()
{
    // TODO: could be extended
}

void DX12SwapChain::Present()
{
    const UINT syncInterval = chainSwapDesc_.base.vsync ? 1u : 0u;
    ThrowIfFailed(swapChain_->Present(syncInterval, 0), "DX12: Present failed");
    currBackBuffer_ = swapChain_->GetCurrentBackBufferIndex();
}

// Public factory functions
inline std::unique_ptr<IRHIDevice> CreateDX12Device()
{
    return std::make_unique<DX12Device>();
}

inline std::unique_ptr<IRHISwapChain> CreateDX12SwapChain(IRHIDevice& device, DX12SwapChainDesc desc)
{
    auto* dxDev = dynamic_cast<DX12Device*>(&device);
    if (!dxDev)
    {
        throw std::runtime_error("CreateDX12SwapChain: device is not DX12Device");
    }

    auto swapChainDesc = std::make_unique<DX12SwapChain>(*dxDev, std::move(desc));
    return swapChainDesc;
}
