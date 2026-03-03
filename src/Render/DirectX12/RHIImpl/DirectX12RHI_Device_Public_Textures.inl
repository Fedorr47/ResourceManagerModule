        TextureHandle CreateTexture2D(Extent2D extent, Format format) override
        {
            TextureHandle textureHandle{ ++nextTexId_ };
            TextureEntry textureEntry{};
            textureEntry.extent = extent;
            textureEntry.format = format;

            const DXGI_FORMAT dxFmt = ToDXGIFormat(format);

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resourceDesc.Width = extent.width;
            resourceDesc.Height = extent.height;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = dxFmt;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            D3D12_CLEAR_VALUE clearValue{};
            clearValue.Format = dxFmt;

            D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON;

            if (IsDepthFormat(format))
            {
                DXGI_FORMAT dsvFmt = ToDXGIFormat(format);
                DXGI_FORMAT resFmt = DXGI_FORMAT_UNKNOWN;
                DXGI_FORMAT srvFmt = DXGI_FORMAT_UNKNOWN;

                if (format == Format::D32_FLOAT)
                {
                    resFmt = DXGI_FORMAT_R32_TYPELESS;
                    srvFmt = DXGI_FORMAT_R32_FLOAT;
                }
                else if (format == Format::D24_UNORM_S8_UINT)
                {
                    resFmt = DXGI_FORMAT_R24G8_TYPELESS;
                    srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                }
                else
                {
                    resFmt = dsvFmt; // fallback (no sampling)
                    srvFmt = DXGI_FORMAT_UNKNOWN;
                }

                resourceDesc.Format = resFmt;
                resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

                D3D12_CLEAR_VALUE clearValue{};
                clearValue.Format = dsvFmt;
                clearValue.DepthStencil.Depth = 1.0f;
                clearValue.DepthStencil.Stencil = 0;

                ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &resourceDesc,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    &clearValue,
                    IID_PPV_ARGS(&textureEntry.resource)),
                    "DX12: Create depth texture failed");

                textureEntry.resourceFormat = resFmt;
                textureEntry.dsvFormat = dsvFmt;
                textureEntry.srvFormat = srvFmt;
                textureEntry.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;

                EnsureDSVHeap();
                textureEntry.dsv = AllocateDSV(textureEntry.resource.Get(), dsvFmt, textureEntry.dsvIndex);
                textureEntry.hasDSV = true;

                // SRV for sampling (shadow maps)
                if (srvFmt != DXGI_FORMAT_UNKNOWN)
                {
                    AllocateSRV(textureEntry, srvFmt, 1);
                }
            }
            else
            {
                resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                clearValue.Color[0] = 0.0f;
                clearValue.Color[1] = 0.0f;
                clearValue.Color[2] = 0.0f;
                clearValue.Color[3] = 1.0f;
                initState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &resourceDesc,
                    initState,
                    &clearValue,
                    IID_PPV_ARGS(&textureEntry.resource)),
                    "DX12: Create color texture failed");

                textureEntry.resourceFormat = dxFmt;
                textureEntry.srvFormat = dxFmt;
                textureEntry.rtvFormat = dxFmt;
                textureEntry.state = initState;

                EnsureRTVHeap();
                textureEntry.rtv = AllocateRTV(textureEntry.resource.Get(), dxFmt, textureEntry.rtvIndex);
                textureEntry.hasRTV = true;

                AllocateSRV(textureEntry, dxFmt, 1);
            }

            textures_[textureHandle.id] = std::move(textureEntry);
            return textureHandle;
        }

        TextureHandle CreateTextureCube(Extent2D extent, Format format) override
        {
            TextureHandle textureHandle{ ++nextTexId_ };
            TextureEntry textureEntry{};
            textureEntry.extent = extent;
            textureEntry.format = format;
            textureEntry.type = TextureEntry::Type::Cube;

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resourceDesc.Width = extent.width;
            resourceDesc.Height = extent.height;
            resourceDesc.DepthOrArraySize = 6; // cubemap faces
            resourceDesc.MipLevels = 1;
            // Mip chain: reflections benefit from prefiltered mip levels (roughness->LOD).
            // Keep point-shadow cubes at 1 mip to save memory.
            auto CalcMipLevels = [](std::uint32_t w, std::uint32_t h) -> UINT
                {
                    const std::uint32_t m = (w > h) ? w : h;
                    UINT levels = 1;
                    std::uint32_t v = m;
                    while (v > 1u) { v >>= 1u; ++levels; }
                    return levels;
                };

            const UINT mipLevels = (format == Format::RGBA8_UNORM) ? CalcMipLevels(extent.width, extent.height) : 1u;
            resourceDesc.MipLevels = static_cast<UINT16>(mipLevels);
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

            if (IsDepthFormat(format))
            {
                // Cubemap depth (used by View-Instancing point-shadow pass).
                DXGI_FORMAT dsvFmt = ToDXGIFormat(format);
                DXGI_FORMAT resFmt = dsvFmt;
                DXGI_FORMAT srvFmt = DXGI_FORMAT_UNKNOWN;

                if (format == Format::D32_FLOAT)
                {
                    resFmt = DXGI_FORMAT_R32_TYPELESS;
                    srvFmt = DXGI_FORMAT_R32_FLOAT;
                }
                else if (format == Format::D24_UNORM_S8_UINT)
                {
                    resFmt = DXGI_FORMAT_R24G8_TYPELESS;
                    srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                }

                resourceDesc.Format = resFmt;
                resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

                D3D12_CLEAR_VALUE clearValue{};
                clearValue.Format = dsvFmt;
                clearValue.DepthStencil.Depth = 1.0f;
                clearValue.DepthStencil.Stencil = 0;

                ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &resourceDesc,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    &clearValue,
                    IID_PPV_ARGS(&textureEntry.resource)),
                    "DX12: Create cubemap depth texture failed");

                textureEntry.resourceFormat = resFmt;
                textureEntry.dsvFormat = dsvFmt;
                textureEntry.srvFormat = srvFmt;
                textureEntry.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;

                EnsureDSVHeap();
                textureEntry.dsvAllFaces = AllocateDSVTexture2DArray(textureEntry.resource.Get(), dsvFmt, 0, 6, textureEntry.dsvIndexAllFaces);
                textureEntry.hasDSVAllFaces = true;

                // Optional SRV for sampling (not required for point shadows in this engine, but useful for future features).
                if (srvFmt != DXGI_FORMAT_UNKNOWN)
                {
                    AllocateSRV(textureEntry, srvFmt, 1);
                }
            }
            else
            {
                // Color cubemap (currently used for point light shadows: R32_FLOAT distance map).
                const DXGI_FORMAT dxFmt = ToDXGIFormat(format);

                resourceDesc.Format = dxFmt;
                resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

                D3D12_CLEAR_VALUE clearValue{};
                clearValue.Format = dxFmt;
                clearValue.Color[0] = 1.0f;
                clearValue.Color[1] = 1.0f;
                clearValue.Color[2] = 1.0f;
                clearValue.Color[3] = 1.0f;

                const D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_RENDER_TARGET;

                ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &resourceDesc,
                    initState,
                    &clearValue,
                    IID_PPV_ARGS(&textureEntry.resource)),
                    "DX12: Create cubemap color texture failed");

                textureEntry.resourceFormat = dxFmt;
                textureEntry.srvFormat = dxFmt;
                textureEntry.rtvFormat = dxFmt;
                textureEntry.state = initState;

                EnsureRTVHeap();

                // One RTV per face (fallback 6-pass path).
                textureEntry.hasRTVFaces = true;
                for (UINT face = 0; face < 6; ++face)
                {
                    textureEntry.rtvFaces[face] = AllocateRTVTexture2DArraySlice(textureEntry.resource.Get(), dxFmt, face, textureEntry.rtvIndexFaces[face]);
                }

                // One RTV that targets all 6 faces as a Texture2DArray view (View-Instancing path).
                textureEntry.rtvAllFaces = AllocateRTVTexture2DArray(textureEntry.resource.Get(), dxFmt, 0, 6, textureEntry.rtvIndexAllFaces);
                textureEntry.hasRTVAllFaces = true;

                AllocateSRV(textureEntry, dxFmt, 1);
            }

            textures_[textureHandle.id] = std::move(textureEntry);
            return textureHandle;
        }

        void DestroyTexture(TextureHandle texture) noexcept override
        {
            if (texture.id == 0)
            {
                return;
            }

            auto it = textures_.find(texture.id);
            if (it == textures_.end())
            {
                return;
            }

            TextureEntry entry = std::move(it->second);
            textures_.erase(it);

            // Keep the resource alive until GPU finishes the frame that referenced it.
            if (entry.resource)
            {
                CurrentFrame().deferredResources.push_back(std::move(entry.resource));
            }

            // Recycle SRV index after the frame fence is completed (see BeginFrame()).
            if (entry.hasSRV && entry.srvIndex != 0)
            {
                CurrentFrame().deferredFreeSrv.push_back(entry.srvIndex);
            }
            // If we also created a cube-as-array SRV, recycle it too.
            if (entry.hasSRVArray && entry.srvIndexArray != 0)
            {
                CurrentFrame().deferredFreeSrv.push_back(entry.srvIndexArray);
            }
            if (entry.hasRTV)
            {
                CurrentFrame().deferredFreeRtv.push_back(entry.rtvIndex);
            }
            if (entry.hasRTVFaces)
            {
                for (UINT idx : entry.rtvIndexFaces)
                {
                    CurrentFrame().deferredFreeRtv.push_back(idx);
                }
            }
            if (entry.hasRTVAllFaces)
            {
                CurrentFrame().deferredFreeRtv.push_back(entry.rtvIndexAllFaces);
            }
            if (entry.hasDSV)
            {
                CurrentFrame().deferredFreeDsv.push_back(entry.dsvIndex);
            }
            if (entry.hasDSVAllFaces)
            {
                CurrentFrame().deferredFreeDsv.push_back(entry.dsvIndexAllFaces);
            }
        }

        // ---------------- Framebuffers ----------------
        FrameBufferHandle CreateFramebuffer(TextureHandle color, TextureHandle depth) override
        {
            FrameBufferHandle frameBuffer{ ++nextFBId_ };
            FramebufferEntry frameBufEntry{};
            if (color.id != 0)
            {
                frameBufEntry.colors[0] = color;
                frameBufEntry.colorCount = 1u;
            }
            frameBufEntry.depth = depth;
            framebuffers_[frameBuffer.id] = frameBufEntry;
            return frameBuffer;
        }


        FrameBufferHandle CreateFramebufferMRT(std::span<const TextureHandle> colors, TextureHandle depth) override
        {
            FrameBufferHandle frameBuffer{ ++nextFBId_ };
            FramebufferEntry frameBufEntry{};
            const std::size_t count = std::min<std::size_t>(colors.size(), FramebufferEntry::kMaxColorAttachments);
            frameBufEntry.colorCount = static_cast<std::uint32_t>(count);

            for (std::size_t i = 0; i < count; ++i)
            {
                frameBufEntry.colors[i] = colors[i];
            }

            frameBufEntry.depth = depth;
            framebuffers_[frameBuffer.id] = frameBufEntry;
            return frameBuffer;
        }

        FrameBufferHandle CreateFramebufferCube(TextureHandle colorCube, TextureHandle depthCube) override
        {
            FrameBufferHandle frameBuffer{ ++nextFBId_ };
            FramebufferEntry frameBufEntry{};
            frameBufEntry.colors[0] = colorCube;
            frameBufEntry.colorCount = 1u;
            frameBufEntry.depth = depthCube;
            frameBufEntry.colorCubeAllFaces = true;
            framebuffers_[frameBuffer.id] = frameBufEntry;
            return frameBuffer;
        }

        FrameBufferHandle CreateFramebufferCubeFace(TextureHandle colorCube, std::uint32_t faceIndex, TextureHandle depth) override
        {
            FrameBufferHandle frameBuffer{ ++nextFBId_ };
            FramebufferEntry frameBufEntry{};
            frameBufEntry.colors[0] = colorCube;
            frameBufEntry.colorCount = 1u;
            frameBufEntry.depth = depth;
            frameBufEntry.colorCubeFace = faceIndex;
            framebuffers_[frameBuffer.id] = frameBufEntry;
            return frameBuffer;
        }

        void DestroyFramebuffer(FrameBufferHandle frameBuffer) noexcept override
        {
            if (frameBuffer.id == 0)
            {
                return;
            }
            framebuffers_.erase(frameBuffer.id);
        }

        // ---------------- Buffers ----------------
