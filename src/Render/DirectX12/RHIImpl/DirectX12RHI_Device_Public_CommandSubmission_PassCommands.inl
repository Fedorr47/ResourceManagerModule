if constexpr (std::is_same_v<T, CommandBeginPass>)
{
    const BeginPassDesc& pass = cmd.desc;
    const ClearDesc& c = pass.clearDesc;

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> rtvs{};
    UINT numRT = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    bool hasDSV = false;

    curNumRT = 0;
    std::fill(curRTVFormats.begin(), curRTVFormats.end(), DXGI_FORMAT_UNKNOWN);
    curDSVFormat = DXGI_FORMAT_UNKNOWN;

    if (pass.frameBuffer.id == 0)
    {
        if (!pass.swapChain)
        {
            throw std::runtime_error("DX12: CommandBeginPass: pass.swapChain is null (frameBuffer.id == 0)");
        }

        auto* sc = dynamic_cast<DX12SwapChain*>(pass.swapChain);
        if (!sc)
        {
            throw std::runtime_error("DX12: CommandBeginPass: pass.swapChain is not DX12SwapChain");
        }

        curSwapChain = sc;
        TransitionBackBuffer(*sc, D3D12_RESOURCE_STATE_RENDER_TARGET);

        rtvs[0] = sc->CurrentRTV();
        numRT = 1;

        curNumRT = numRT;
        curRTVFormats[0] = sc->BackBufferFormat();

        if (pass.bindDepthStencil)
        {
            const TextureHandle depthTex = sc->GetDepthTexture();
            if (depthTex)
            {
                auto it = textures_.find(depthTex.id);
                if (it == textures_.end())
                {
                    throw std::runtime_error("DX12: CommandBeginPass: swapchain depth texture not found");
                }

                auto& te = it->second;
                if (!te.hasDSV)
                {
                    throw std::runtime_error("DX12: CommandBeginPass: swapchain depth texture has no DSV");
                }

                TransitionResource(
                    cmdList_.Get(),
                    te.resource.Get(),
                    te.state,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE);

                dsv = te.dsv;
                hasDSV = (dsv.ptr != 0);
                curDSVFormat = te.dsvFormat;
            }
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE* rtvPtr = rtvs.data();
        const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = hasDSV ? &dsv : nullptr;
        cmdList_->OMSetRenderTargets(numRT, rtvPtr, FALSE, dsvPtr);

        if (c.clearColor)
        {
            const float* col = c.color.data();
            cmdList_->ClearRenderTargetView(rtvs[0], col, 0, nullptr);
        }
        const D3D12_CLEAR_FLAGS dsClearFlags =
            (c.clearDepth ? D3D12_CLEAR_FLAG_DEPTH : static_cast<D3D12_CLEAR_FLAGS>(0)) |
            (c.clearStencil ? D3D12_CLEAR_FLAG_STENCIL : static_cast<D3D12_CLEAR_FLAGS>(0));
        if (dsClearFlags != 0 && hasDSV)
        {
            cmdList_->ClearDepthStencilView(dsv, dsClearFlags, c.depth, c.stencil, 0, nullptr);
        }

        curPassIsSwapChain = true;
    }
    else
    {
        // ----- Offscreen framebuffer pass -----
        auto fbIt = framebuffers_.find(pass.frameBuffer.id);
        if (fbIt == framebuffers_.end())
        {
            throw std::runtime_error("DX12: CommandBeginPass: framebuffer not found");
        }

        const FramebufferEntry& fb = fbIt->second;

        // Color (0..8 RT)
        if (fb.colorCount > 0)
        {
            // Cubemap rendering is only supported for a single color attachment.
            if (fb.colorCubeAllFaces || fb.colorCubeFace != 0xFFFFFFFFu)
            {
                if (fb.colorCount != 1u || fb.colors[0].id == 0)
                {
                    throw std::runtime_error("DX12: CommandBeginPass: cubemap render requires exactly one color attachment");
                }
                auto it = textures_.find(fb.colors[0].id);
                if (it == textures_.end())
                {
                    throw std::runtime_error("DX12: CommandBeginPass: framebuffer color texture not found");
                }
                auto& te = it->second;
                if (fb.colorCubeAllFaces)
                {
                    if (!te.hasRTVAllFaces)
                    {
                        throw std::runtime_error("DX12: CommandBeginPass: cubemap color texture has no RTV (all faces)");
                    }
                    TransitionResource(
                        cmdList_.Get(),
                        te.resource.Get(),
                        te.state,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                    rtvs[0] = te.rtvAllFaces;
                    numRT = 1;
                    curRTVFormats[0] = te.rtvFormat;
                }
                else
                {
                    if (!te.hasRTVFaces)
                    {
                        throw std::runtime_error("DX12: CommandBeginPass: cubemap color texture has no RTV faces");
                    }
                    if (fb.colorCubeFace >= 6)
                    {
                        throw std::runtime_error("DX12: CommandBeginPass: cubemap face index out of range");
                    }
                    TransitionResource(
                        cmdList_.Get(),
                        te.resource.Get(),
                        te.state,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                    rtvs[0] = te.rtvFaces[fb.colorCubeFace];
                    numRT = 1;
                    curRTVFormats[0] = te.rtvFormat;
                }
            }
            // Regular MRT.
            for (std::uint32_t i = 0; i < fb.colorCount; ++i)
            {
                const TextureHandle th = fb.colors[i];
                if (th.id == 0)
                {
                    throw std::runtime_error("DX12: CommandBeginPass: framebuffer color attachment is null");
                }
                auto it = textures_.find(th.id);
                if (it == textures_.end())
                {
                    throw std::runtime_error("DX12: CommandBeginPass: framebuffer color texture not found");
                }
                auto& te = it->second;
                if (!te.hasRTV)
                {
                    throw std::runtime_error("DX12: CommandBeginPass: color texture has no RTV");
                }
                TransitionResource(
                    cmdList_.Get(),
                    te.resource.Get(),
                    te.state,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
                rtvs[numRT] = te.rtv;
                curRTVFormats[numRT] = te.rtvFormat;
                ++numRT;
            }
        }
        // Depth
        if (fb.depth)
        {
            auto it = textures_.find(fb.depth.id);
            if (it == textures_.end())
            {
                throw std::runtime_error("DX12: CommandBeginPass: framebuffer depth texture not found");
            }

            auto& te = it->second;

            if (fb.colorCubeAllFaces && te.hasDSVAllFaces)
            {
                TransitionResource(
                    cmdList_.Get(),
                    te.resource.Get(),
                    te.state,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE);

                dsv = te.dsvAllFaces;
                hasDSV = true;
                curDSVFormat = te.dsvFormat;
            }
            else
            {
                if (!te.hasDSV)
                {
                    throw std::runtime_error("DX12: CommandBeginPass: depth texture has no DSV");
                }

                TransitionResource(
                    cmdList_.Get(),
                    te.resource.Get(),
                    te.state,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE);

                dsv = te.dsv;
                hasDSV = true;
                curDSVFormat = te.dsvFormat;
            }
        }

        curPassIsSwapChain = false;
        curSwapChain = nullptr;

        curNumRT = numRT;

        // Bind RT/DSV
        const D3D12_CPU_DESCRIPTOR_HANDLE* rtvPtr = (numRT > 0) ? rtvs.data() : nullptr;
        const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = (hasDSV) ? &dsv : nullptr;
        cmdList_->OMSetRenderTargets(numRT, rtvPtr, FALSE, dsvPtr);

        // Clear
        if (c.clearColor && numRT > 0)
        {
            const float* col = c.color.data();
            for (UINT i = 0; i < numRT; ++i)
            {
                cmdList_->ClearRenderTargetView(rtvs[i], col, 0, nullptr);
            }
        }

        const D3D12_CLEAR_FLAGS dsClearFlags =
            (c.clearDepth ? D3D12_CLEAR_FLAG_DEPTH : static_cast<D3D12_CLEAR_FLAGS>(0)) |
            (c.clearStencil ? D3D12_CLEAR_FLAG_STENCIL : static_cast<D3D12_CLEAR_FLAGS>(0));
        if (dsClearFlags != 0 && hasDSV)
        {
            cmdList_->ClearDepthStencilView(dsv, dsClearFlags, c.depth, c.stencil, 0, nullptr);
        }
    }
}
else if constexpr (std::is_same_v<T, CommandEndPass>)
{
    if (curPassIsSwapChain)
    {
        if (!curSwapChain)
        {
            throw std::runtime_error("DX12: CommandEndPass: curSwapChain is null");
        }
        TransitionBackBuffer(*curSwapChain, D3D12_RESOURCE_STATE_PRESENT);
    }
}
else if constexpr (std::is_same_v<T, CommandSetViewport>)
{
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = static_cast<float>(cmd.x);
    viewport.TopLeftY = static_cast<float>(cmd.y);
    viewport.Width = static_cast<float>(cmd.width);
    viewport.Height = static_cast<float>(cmd.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    cmdList_->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{};
    scissor.left = cmd.x;
    scissor.top = cmd.y;
    scissor.right = cmd.x + cmd.width;
    scissor.bottom = cmd.y + cmd.height;
    cmdList_->RSSetScissorRects(1, &scissor);
}