    // NOTE:
    // Pipeline State Stream parsing requires each subobject to be aligned to sizeof(void*)
    // and for the stream layout to be well-formed. A common source of E_INVALIDARG is a
    // custom subobject wrapper that doesn't add trailing padding so the next subobject's
    // Type field starts at a pointer-aligned offset.
    
    template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
    struct alignas(void*) PSOSubobject
    {
        static constexpr std::size_t kAlign = sizeof(void*);
        static constexpr std::size_t kBaseSize = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) + sizeof(T);
        static constexpr std::size_t kPaddedSize = ((kBaseSize + (kAlign - 1)) / kAlign) * kAlign;
        static constexpr std::size_t kPadSize = kPaddedSize - kBaseSize;
    
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ Type };
        T data{};
    };

    class DX12Device final : public IRHIDevice
    {
    public:

        DX12Device()
        {
            core_.Init();

            // Detect optional DX12 capabilities (SM6.1 / View Instancing / DXC).
            DetectCapabilities_();

            // -----------------------------------------------------------------
            // Frame resources (allocator + small persistent CB upload buffer)
            // -----------------------------------------------------------------
            for (std::uint32_t i = 0; i < kFramesInFlight; ++i)
            {
                ThrowIfFailed(NativeDevice()->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&frames_[i].cmdAlloc)),
                    "DX12: CreateCommandAllocator failed");

                // Per-frame constant upload buffer (persistently mapped).
                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

                D3D12_RESOURCE_DESC resourceDesc{};
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                resourceDesc.Width = static_cast<UINT64>(kPerFrameCBUploadBytes);
                resourceDesc.Height = 1;
                resourceDesc.DepthOrArraySize = 1;
                resourceDesc.MipLevels = 1;
                resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
                resourceDesc.SampleDesc.Count = 1;
                resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &resourceDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&frames_[i].cbUpload)),
                    "DX12: Create per-frame constant upload buffer failed");

                void* mapped = nullptr;
                ThrowIfFailed(frames_[i].cbUpload->Map(0, nullptr, &mapped),
                    "DX12: Map per-frame constant upload buffer failed");

                // Per-frame buffer upload ring (persistently mapped).
                D3D12_RESOURCE_DESC bufDesc = resourceDesc;
                bufDesc.Width = static_cast<UINT64>(kPerFrameBufUploadBytes);

                ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &bufDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&frames_[i].bufUpload)),
                    "DX12: Create per-frame buffer upload ring failed");

                void* bufMapped = nullptr;
                ThrowIfFailed(frames_[i].bufUpload->Map(0, nullptr, &bufMapped),
                    "DX12: Map per-frame buffer upload ring failed");

                frames_[i].bufMapped = reinterpret_cast<std::byte*>(bufMapped);
                frames_[i].bufCursor = 0;

                frames_[i].cbMapped = reinterpret_cast<std::byte*>(mapped);
                frames_[i].cbCursor = 0;
                frames_[i].fenceValue = 0;
            }

            // Command list (created once, reset per frame).
            ThrowIfFailed(NativeDevice()->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                frames_[0].cmdAlloc.Get(),
                nullptr,
                IID_PPV_ARGS(&cmdList_)),
                "DX12: CreateCommandList failed");
            cmdList_->Close();

            // Fence
            ThrowIfFailed(NativeDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
                "DX12: CreateFence failed");
            fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!fenceEvent_)
            {
                throw std::runtime_error("DX12: CreateEvent failed");
            }

            // SRV heap (shader visible)
            {
                D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
                heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                heapDesc.NumDescriptors = kSrvHeapNumDescriptors;
                heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                ThrowIfFailed(NativeDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap_)),
                    "DX12: Create SRV heap failed");

                srvInc_ = NativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

                // null SRVs:
                //  slot 0: null Texture2D SRV (for t0/t1 texture slots)
                //  slot 1: null StructuredBuffer SRV (for t2 lights SB)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();

                    D3D12_SHADER_RESOURCE_VIEW_DESC nullTex{};
                    nullTex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    nullTex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    nullTex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    nullTex.Texture2D.MipLevels = 1;
                    NativeDevice()->CreateShaderResourceView(nullptr, &nullTex, cpu);

                    cpu.ptr += static_cast<SIZE_T>(srvInc_);

                    D3D12_SHADER_RESOURCE_VIEW_DESC nullBuf{};
                    nullBuf.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                    nullBuf.Format = DXGI_FORMAT_UNKNOWN;
                    nullBuf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    nullBuf.Buffer.FirstElement = 0;
                    nullBuf.Buffer.NumElements = 1;
                    nullBuf.Buffer.StructureByteStride = 16;
                    nullBuf.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
                    NativeDevice()->CreateShaderResourceView(nullptr, &nullBuf, cpu);
                }

                nextSrvIndex_ = 3; // 0=null tex, 1=null buffer, 2=ImGui font SRV
                freeSrv_.clear();
            }

            CreateRootSignature();
        }

        ~DX12Device() override
        {
            // Make sure GPU is idle before we release resources referenced by the queue.
            try
            {
                if (fence_ && core_.cmdQueue)
                {
                    FlushGPU();
                }
            }
            catch (...)
            {
                // Avoid exceptions from destructors.
            }

            for (auto& fr : frames_)
            {
                if (fr.cbUpload)
                {
                    fr.cbUpload->Unmap(0, nullptr);
                    fr.cbMapped = nullptr;
                }

                if (fr.bufUpload)
                {
                    fr.bufUpload->Unmap(0, nullptr);
                    fr.bufMapped = nullptr;
                }

                fr.deferredResources.clear();
                fr.deferredFreeSrv.clear();
                fr.deferredFreeRtv.clear();
                fr.deferredFreeDsv.clear();
            }

            if (fenceEvent_)
            {
                CloseHandle(fenceEvent_);
                fenceEvent_ = nullptr;
            }

            ShutdownDXC_();
        }

        void ReplaceSampledTextureResource(rhi::TextureHandle textureHandle, ID3D12Resource* newRes, DXGI_FORMAT fmt, UINT mipLevels)
        {
            auto it = textures_.find(textureHandle.id);
            if (it == textures_.end())
            {
                throw std::runtime_error("DX12: ReplaceSampledTextureResource: texture handle not found");
            }

            it->second.resource.Reset();
            it->second.resource.Attach(newRes); // takes ownership (AddRef already implied by Attach contract)

            // Keep the same descriptor slot if we already had an SRV; just rewrite it.
            if (it->second.hasSRV && it->second.srvIndex != 0)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += static_cast<SIZE_T>(it->second.srvIndex) * srvInc_;

                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = fmt;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.ViewDimension = (it->second.type == TextureEntry::Type::Cube)
                    ? D3D12_SRV_DIMENSION_TEXTURECUBE
                    : D3D12_SRV_DIMENSION_TEXTURE2D;

                if (it->second.type == TextureEntry::Type::Cube)
                {
                    srvDesc.TextureCube.MostDetailedMip = 0;
                    srvDesc.TextureCube.MipLevels = mipLevels;
                    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
                }
                else
                {
                    srvDesc.Texture2D.MostDetailedMip = 0;
                    srvDesc.Texture2D.MipLevels = mipLevels;
                    srvDesc.Texture2D.PlaneSlice = 0;
                    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
                }

                NativeDevice()->CreateShaderResourceView(it->second.resource.Get(), &srvDesc, cpu);
            }
            else
            {
                it->second.hasSRV = false;
                AllocateSRV(it->second, fmt, mipLevels);
            }
        } /// DX12Device

        TextureHandle RegisterSampledTexture(ID3D12Resource* res, DXGI_FORMAT fmt, UINT mipLevels)
        {
            if (!res)
            {
                return {};
            }

            TextureHandle textureHandle{ ++nextTexId_ };
            TextureEntry textureEntry{};

            // Fill extent from resource desc
            const D3D12_RESOURCE_DESC resourceDesc = res->GetDesc();
            textureEntry.extent = Extent2D{
                static_cast<std::uint32_t>(resourceDesc.Width),
                static_cast<std::uint32_t>(resourceDesc.Height) };
            textureEntry.format = rhi::Format::RGBA8_UNORM; // internal book-keeping only (engine side)

            // Take ownership (AddRef)
            textureEntry.resource = res;

            // Allocate SRV in our shader-visible heap
            AllocateSRV(textureEntry, fmt, mipLevels);

            textures_[textureHandle.id] = std::move(textureEntry);
            return textureHandle;
        }

        TextureHandle RegisterSampledTextureCube(ID3D12Resource* res, DXGI_FORMAT fmt, UINT mipLevels)
        {
            if (!res)
            {
                return {};
            }

            TextureHandle textureHandle{ ++nextTexId_ };
            TextureEntry textureEntry{};

            const D3D12_RESOURCE_DESC resourceDesc = res->GetDesc();
            textureEntry.extent = Extent2D{
                static_cast<std::uint32_t>(resourceDesc.Width),
                static_cast<std::uint32_t>(resourceDesc.Height) };
            textureEntry.format = rhi::Format::RGBA8_UNORM;
            textureEntry.type = TextureEntry::Type::Cube;

            // Take ownership (AddRef)
            textureEntry.resource = res;

            AllocateSRV(textureEntry, fmt, mipLevels);

            textures_[textureHandle.id] = std::move(textureEntry);
            return textureHandle;
        }

        std::string_view GetName() const override
        {
            return "DirectX12 RHI";
        }

        // ---- Dear ImGui hooks ----
        #include "DirectX12RHI_ImGui_Public.inl"
        void WaitIdle() override { FlushGPU(); }

        Backend GetBackend() const noexcept override
        {
            return Backend::DirectX12;
        }

        bool SupportsShaderModel6() const override
        {
            return supportsSM6_1_;
        }

        bool SupportsViewInstancing() const override
        {
            return supportsViewInstancing_;
        }

        bool SupportsVPAndRTArrayIndexFromAnyShader() const override
        {
            return supportsVPAndRTArrayIndexFromAnyShader_;
        }

        ShaderHandle CreateShaderEx(ShaderStage stage, std::string_view debugName, std::string_view sourceOrBytecode, ShaderModel shaderModel) override
        {
            if (shaderModel == ShaderModel::SM5_1)
            {
                return CreateShader(stage, debugName, sourceOrBytecode);
            }

#if CORE_DX12_HAS_DXC
            // Shader Model 6.1 (DXIL) via DXC.
            if (!supportsSM6_1_ || !EnsureDXC_())
            {
                return {};
            }

            const wchar_t* target = (stage == ShaderStage::Vertex) ? L"vs_6_1" : L"ps_6_1";

            auto TryCompile = [&](std::string_view entry) -> ComPtr<ID3DBlob>
                {
                    ComPtr<ID3DBlob> out;
                    std::string err;
                    if (!CompileDXC_(sourceOrBytecode, target, entry, debugName, out, &err))
                    {
                        return {};
                    }
                    return out;
                };

            ComPtr<ID3DBlob> code = TryCompile(std::string_view(debugName));
            if (!code)
            {
                code = TryCompile("main");
            }
            if (!code)
            {
                const char* fallback = (stage == ShaderStage::Vertex) ? "VSMain" : "PSMain";
                code = TryCompile(fallback);
            }

            if (!code)
            {
                return {};
            }

            ShaderHandle handle{ ++nextShaderId_ };
            ShaderEntry shaderEntry{};
            shaderEntry.stage = stage;
            shaderEntry.name = std::string(debugName);
            shaderEntry.blob = code;

            shaders_[handle.id] = std::move(shaderEntry);
            return handle;
#else
            // Built without dxcapi.h; cannot compile SM6 shaders.
            return {};
#endif
        }

        PipelineHandle CreatePipelineEx(std::string_view debugName, ShaderHandle vertexShader, ShaderHandle pixelShader, PrimitiveTopologyType topologyType, std::uint32_t viewInstanceCount) override
        {
            if (viewInstanceCount > 1)
            {
                // View instancing PSOs require ID3D12Device2 + a supported ViewInstancingTier.
                if (!supportsViewInstancing_ || !device2_)
                {
                    return {};
                }
            }

            PipelineHandle handle{ ++nextPsoId_ };
            PipelineEntry pipelineEntry{};
            pipelineEntry.debugName = std::string(debugName);
            pipelineEntry.vs = vertexShader;
            pipelineEntry.ps = pixelShader;
            pipelineEntry.topologyType = topologyType;
            pipelineEntry.viewInstanceCount = viewInstanceCount;
            pipelines_[handle.id] = std::move(pipelineEntry);
            return handle;
        }

        PipelineHandle CreatePipeline(std::string_view debugName, ShaderHandle vertexShader, ShaderHandle pixelShader, PrimitiveTopologyType topologyType) override
        {
            return CreatePipelineEx(debugName, vertexShader, pixelShader, topologyType, 1);
        }

        // ---------------- Textures (RenderGraph transient) ---------------- //
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
            frameBufEntry.color = color;
            frameBufEntry.depth = depth;
            framebuffers_[frameBuffer.id] = frameBufEntry;
            return frameBuffer;
        }


        FrameBufferHandle CreateFramebufferCube(TextureHandle colorCube, TextureHandle depthCube) override
        {
            FrameBufferHandle frameBuffer{ ++nextFBId_ };
            FramebufferEntry frameBufEntry{};
            frameBufEntry.color = colorCube;
            frameBufEntry.depth = depthCube;
            frameBufEntry.colorCubeAllFaces = true;
            framebuffers_[frameBuffer.id] = frameBufEntry;
            return frameBuffer;
        }

        FrameBufferHandle CreateFramebufferCubeFace(TextureHandle colorCube, std::uint32_t faceIndex, TextureHandle depth) override
        {
            FrameBufferHandle frameBuffer{ ++nextFBId_ };
            FramebufferEntry frameBufEntry{};
            frameBufEntry.color = colorCube;
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
        BufferHandle CreateBuffer(const BufferDesc& desc) override
        {
            BufferHandle handle{ ++nextBufId_ };
            BufferEntry bufferEntry{};
            bufferEntry.desc = desc;

            const UINT64 sz = static_cast<UINT64>(desc.sizeInBytes);

            // GPU-local buffer (DEFAULT heap). Updates happen via per-frame upload ring.
            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resourceDesc.Width = std::max<UINT64>(1, sz);
            resourceDesc.Height = 1;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            const D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON;

            ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                initState,
                nullptr,
                IID_PPV_ARGS(&bufferEntry.resource)),
                "DX12: CreateBuffer failed");

            bufferEntry.state = initState;

            if (desc.bindFlag == BufferBindFlag::StructuredBuffer)
            {
                AllocateStructuredBufferSRV(bufferEntry);
            }

            buffers_[handle.id] = std::move(bufferEntry);
            return handle;
        }

        void UpdateBuffer(BufferHandle buffer, std::span<const std::byte> data, std::size_t offsetBytes = 0) override
        {
            if (!buffer || data.empty())
            {
                return;
            }

            auto it = buffers_.find(buffer.id);
            if (it == buffers_.end())
            {
                return;
            }

            BufferEntry& entry = it->second;

            const std::size_t end = offsetBytes + data.size();
            if (end > entry.desc.sizeInBytes)
                throw std::runtime_error("DX12: UpdateBuffer out of bounds");

            // If we haven't submitted anything yet, it's safe to do a blocking upload.
            if (!hasSubmitted_)
            {
                ImmediateUploadBuffer(entry, data, offsetBytes);
                return;
            }

            PendingBufferUpdate u{};
            u.buffer = buffer;
            u.dstOffsetBytes = offsetBytes;
            u.data.assign(data.begin(), data.end());
            pendingBufferUpdates_.push_back(std::move(u));
        }

        void DestroyBuffer(BufferHandle buffer) noexcept override
        {
            if (buffer.id == 0)
            {
                return;
            }

            auto it = buffers_.find(buffer.id);
            if (it == buffers_.end())
            {
                return;
            }

            BufferEntry entry = std::move(it->second);
            buffers_.erase(it);

            // Remove pending updates for this buffer.
            if (!pendingBufferUpdates_.empty())
            {
                pendingBufferUpdates_.erase(
                    std::remove_if(pendingBufferUpdates_.begin(), pendingBufferUpdates_.end(),
                        [&](const PendingBufferUpdate& u) { return u.buffer.id == buffer.id; }),
                    pendingBufferUpdates_.end());
            }

            if (entry.resource && hasSubmitted_)
            {
                CurrentFrame().deferredResources.push_back(std::move(entry.resource));
            }

            if (entry.hasSRV && entry.srvIndex != 0)
            {
                if (hasSubmitted_)
                {
                    CurrentFrame().deferredFreeSrv.push_back(entry.srvIndex);
                }
                else
                {
                    freeSrv_.push_back(entry.srvIndex);
                }
            }

            if (entry.hasSRVArray && entry.srvIndexArray != 0)
            {
                if (hasSubmitted_)
                {
                    CurrentFrame().deferredFreeSrv.push_back(entry.srvIndexArray);
                }
                else
                {
                    freeSrv_.push_back(entry.srvIndexArray);
                }
            }
        }

        // ---------------- Input layouts ----------------
        InputLayoutHandle CreateInputLayout(const InputLayoutDesc& desc) override
        {
            InputLayoutHandle handle{ ++nextLayoutId_ };
            InputLayoutEntry inputLayoutEntry{};
            inputLayoutEntry.strideBytes = desc.strideBytes;

            inputLayoutEntry.semanticStorage.reserve(desc.attributes.size());
            inputLayoutEntry.elems.reserve(desc.attributes.size());

            for (const auto& attribute : desc.attributes)
            {
                inputLayoutEntry.semanticStorage.emplace_back(SemanticName(attribute.semantic));

                const bool instanced = (attribute.inputSlot != 0);

                D3D12_INPUT_ELEMENT_DESC elemDesc{};
                elemDesc.SemanticName = inputLayoutEntry.semanticStorage.back().c_str();
                elemDesc.SemanticIndex = attribute.semanticIndex;
                elemDesc.Format = ToDXGIVertexFormat(attribute.format);
                elemDesc.InputSlot = attribute.inputSlot;
                elemDesc.AlignedByteOffset = attribute.offsetBytes;
                elemDesc.InputSlotClass = instanced
                    ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                    : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                elemDesc.InstanceDataStepRate = instanced ? 1 : 0;

                inputLayoutEntry.elems.push_back(elemDesc);
            }

            layouts_[handle.id] = std::move(inputLayoutEntry);
            return handle;
        }

        void DestroyInputLayout(InputLayoutHandle layout) noexcept override
        {
            layouts_.erase(layout.id);
        }

        // ---------------- Shaders / Pipelines ----------------
        ShaderHandle CreateShader(ShaderStage stage, std::string_view debugName, std::string_view sourceOrBytecode) override
        {
            ShaderHandle handle{ ++nextShaderId_ };
            ShaderEntry shaderEntry{};
            shaderEntry.stage = stage;
            shaderEntry.name = std::string(debugName);

            const char* target = (stage == ShaderStage::Vertex) ? "vs_5_1" : "ps_5_1";

            ComPtr<ID3DBlob> code;
            ComPtr<ID3DBlob> errors;

            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

            auto TryCompile = [&](const char* entry) -> bool
                {
                    code.Reset();
                    errors.Reset();

                    HRESULT hr = D3DCompile(
                        sourceOrBytecode.data(),
                        sourceOrBytecode.size(),
                        shaderEntry.name.c_str(),
                        nullptr, nullptr,
                        entry, target,
                        flags, 0,
                        &code, &errors);

                    return SUCCEEDED(hr);
                };

            if (!TryCompile(shaderEntry.name.c_str()))
            {
                const char* fallback = (stage == ShaderStage::Vertex) ? "VSMain" : "PSMain";
                if (!TryCompile(fallback))
                {
                    std::string err = "DX12: shader compile failed: ";
                    if (errors)
                    {
                        err += std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
                    }
                    throw std::runtime_error(err);
                }
            }

            shaderEntry.blob = code;
            shaders_[handle.id] = std::move(shaderEntry);
            return handle;
        }

        void DestroyShader(ShaderHandle shader) noexcept override
        {
            shaders_.erase(shader.id);
        }

        void DestroyPipeline(PipelineHandle pso) noexcept override
        {
            pipelines_.erase(pso.id);
            // TODO: PSO cache entries - it can be cleared indpendtly - but right here it is ok
        }

        // ---------------- Submission ----------------
        void SubmitCommandList(CommandList&& commandList) override
        {
            // Begin frame: wait/recycle per-frame stuff + reset allocator/list
            BeginFrame();
            hasSubmitted_ = true;

            hasSubmitted_ = true;

            // Set descriptor heaps (SRV)
            ID3D12DescriptorHeap* heaps[] = { NativeSRVHeap() };
            cmdList_->SetDescriptorHeaps(1, heaps);

            FlushPendingBufferUpdates();

            // State while parsing high-level commands
            GraphicsState curState{};
            PipelineHandle curPipe{};

            InputLayoutHandle curLayout{};
            D3D_PRIMITIVE_TOPOLOGY currentTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            static constexpr std::uint32_t kMaxVBSlots = 2;
            std::array<BufferHandle, kMaxVBSlots> vertexBuffers{};
            std::array<std::uint32_t, kMaxVBSlots> vbStrides{};
            std::array<std::uint32_t, kMaxVBSlots> vbOffsets{};

            BufferHandle indexBuffer{};
            IndexType ibType = IndexType::UINT16;
            std::uint32_t ibOffset = 0;

            // Bound textures by slot (we actuallu use only slot 0)
            std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kMaxSRVSlots> boundTex{};
            for (auto& t : boundTex)
            {
                t = srvHeap_->GetGPUDescriptorHandleForHeapStart(); // null SRV slot0
            }

            // slot 2 (t2) expects StructuredBuffer SRV; point it to null-buffer descriptor (SRV heap index 1).
            if (boundTex.size() > 2)
            {
                boundTex[2].ptr += static_cast<UINT64>(srvInc_);
            }

            // Per-draw constants (raw bytes).
            // The renderer is responsible for packing the layout expected by HLSL.
            static constexpr std::uint32_t kMaxPerDrawConstantsBytes = 512;
            std::array<std::byte, kMaxPerDrawConstantsBytes> perDrawBytes{};
            std::uint32_t perDrawSize = 0;
            std::uint32_t perDrawSlot = 0;

            auto WriteCBAndBind = [&]()
                {
                    FrameResource& fr = CurrentFrame();

                    const std::uint32_t used = (perDrawSize == 0) ? 1u : perDrawSize;
                    const std::uint32_t cbSize = AlignUp(used, 256);

                    if (fr.cbCursor + cbSize > kPerFrameCBUploadBytes)
                    {
                        throw std::runtime_error("DX12: per-frame constant upload ring overflow (increase kPerFrameCBUploadBytes)");
                    }

                    if (perDrawSize != 0)
                    {
                        std::memcpy(fr.cbMapped + fr.cbCursor, perDrawBytes.data(), perDrawSize);
                    }

                    const D3D12_GPU_VIRTUAL_ADDRESS gpuVA = fr.cbUpload->GetGPUVirtualAddress() + fr.cbCursor;
                    cmdList_->SetGraphicsRootConstantBufferView(perDrawSlot, gpuVA);

                    fr.cbCursor += cbSize;
                };

            auto ResolveTextureHandleFromDesc = [&](TextureDescIndex idx) -> TextureHandle
                {
                    if (idx == 0) // 0 = null SRV
                    {
                        return {};
                    }

                    auto it = descToTex_.find(idx);
                    if (it == descToTex_.end())
                    {
                        throw std::runtime_error("DX12: TextureDescIndex not mapped");
                    }
                    return it->second;
                };

            auto GetTextureSRV = [&](TextureHandle textureHandle) -> D3D12_GPU_DESCRIPTOR_HANDLE
                {
                    if (!textureHandle)
                    {
                        return srvHeap_->GetGPUDescriptorHandleForHeapStart();
                    }
                    auto it = textures_.find(textureHandle.id);
                    if (it == textures_.end())
                    {
                        return srvHeap_->GetGPUDescriptorHandleForHeapStart();
                    }
                    if (!it->second.hasSRV)
                    {
                        return srvHeap_->GetGPUDescriptorHandleForHeapStart();
                    }
                    return it->second.srvGpu;
                };

            auto NullBufferSRV = [&]() -> D3D12_GPU_DESCRIPTOR_HANDLE
                {
                    D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap_->GetGPUDescriptorHandleForHeapStart();
                    h.ptr += static_cast<UINT64>(srvInc_); // SRV heap index 1
                    return h;
                };

            auto GetBufferSRV = [&](BufferHandle bufferHandle) -> D3D12_GPU_DESCRIPTOR_HANDLE
                {
                    if (!bufferHandle)
                    {
                        return NullBufferSRV();
                    }

                    auto it = buffers_.find(bufferHandle.id);
                    if (it == buffers_.end())
                    {
                        return NullBufferSRV();
                    }

                    if (!it->second.hasSRV)
                    {
                        return NullBufferSRV();
                    }

                    return it->second.srvGpu;
                };



            UINT curNumRT = 0;
            std::array<DXGI_FORMAT, 8> curRTVFormats{};
            std::fill(curRTVFormats.begin(), curRTVFormats.end(), DXGI_FORMAT_UNKNOWN);
            DXGI_FORMAT curDSVFormat = DXGI_FORMAT_UNKNOWN;
            bool curPassIsSwapChain = false;
            DX12SwapChain* curSwapChain = nullptr;

            auto Barrier = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& curState, D3D12_RESOURCE_STATES desired)
                {
                    if (!res)
                    {
                        return;
                    }
                    if (curState == desired)
                    {
                        return;
                    }

                    D3D12_RESOURCE_BARRIER resBarrier{};
                    resBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    resBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    resBarrier.Transition.pResource = res;
                    resBarrier.Transition.StateBefore = curState;
                    resBarrier.Transition.StateAfter = desired;
                    resBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    cmdList_->ResourceBarrier(1, &resBarrier);
                    curState = desired;
                };

            auto TransitionTexture = [&](TextureHandle tex, D3D12_RESOURCE_STATES desired)
                {
                    if (!tex)
                    {
                        return;
                    }
                    auto it = textures_.find(tex.id);
                    if (it == textures_.end())
                    {
                        return;
                    }
                    Barrier(it->second.resource.Get(), it->second.state, desired);
                };

            auto TransitionBackBuffer = [&](DX12SwapChain& sc, D3D12_RESOURCE_STATES desired)
                {
                    Barrier(sc.CurrentBackBuffer(), sc.CurrentBackBufferState(), desired);
                };

            auto EnsurePSO = [&](PipelineHandle pipelineHandle, InputLayoutHandle layout) -> ID3D12PipelineState*
                {
                    auto PackState = [&](const GraphicsState& s) -> std::uint32_t
                        {
                            std::uint32_t v = 0;
                            v |= (static_cast<std::uint32_t>(s.rasterizer.cullMode) & 0x3u) << 0;
                            v |= (static_cast<std::uint32_t>(s.rasterizer.frontFace) & 0x1u) << 2;
                            v |= (s.depth.testEnable ? 1u : 0u) << 3;
                            v |= (s.depth.writeEnable ? 1u : 0u) << 4;
                            v |= (static_cast<std::uint32_t>(s.depth.depthCompareOp) & 0x7u) << 5;
                            v |= (s.blend.enable ? 1u : 0u) << 8;
                            return v;
                        };

                    auto Fnv1a64 = [](std::uint64_t h, std::uint64_t v) -> std::uint64_t
                        {
                            constexpr std::uint64_t kPrime = 1099511628211ull;
                            for (int i = 0; i < 8; ++i)
                            {
                                const std::uint8_t byte = static_cast<std::uint8_t>((v >> (i * 8)) & 0xffu);
                                h ^= byte;
                                h *= kPrime;
                            }
                            return h;
                        };

                    // PSO cache key MUST include: shaders, state, layout, and render-target formats.
                    std::uint64_t key = 1469598103934665603ull; // FNV-1a offset basis
                    key = Fnv1a64(key, static_cast<std::uint64_t>(pipelineHandle.id));
                    key = Fnv1a64(key, static_cast<std::uint64_t>(layout.id));
                    key = Fnv1a64(key, static_cast<std::uint64_t>(PackState(curState)));
                    key = Fnv1a64(key, static_cast<std::uint64_t>(curNumRT));
                    key = Fnv1a64(key, static_cast<std::uint64_t>(curDSVFormat));
                    for (std::size_t i = 0; i < curRTVFormats.size(); ++i)
                    {
                        key = Fnv1a64(key, static_cast<std::uint64_t>(curRTVFormats[i]));
                    }

                    if (auto it = psoCache_.find(key); it != psoCache_.end())
                    {
                        return it->second.Get();
                    }

                    auto pit = pipelines_.find(pipelineHandle.id);
                    if (pit == pipelines_.end())
                    {
                        throw std::runtime_error("DX12: pipeline handle not found");
                    }

                    auto vsIt = shaders_.find(pit->second.vs.id);
                    auto psIt = shaders_.find(pit->second.ps.id);
                    if (vsIt == shaders_.end() || psIt == shaders_.end())
                    {
                        throw std::runtime_error("DX12: shader handle not found");
                    }

                    auto layIt = layouts_.find(layout.id);
                    if (layIt == layouts_.end())
                    {
                        throw std::runtime_error("DX12: input layout handle not found");
                    }

                    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
                    pipelineDesc.pRootSignature = rootSig_.Get();

                    pipelineDesc.VS = { vsIt->second.blob->GetBufferPointer(), vsIt->second.blob->GetBufferSize() };
                    pipelineDesc.PS = { psIt->second.blob->GetBufferPointer(), psIt->second.blob->GetBufferSize() };

                    pipelineDesc.BlendState = CD3D12_BLEND_DESC(D3D12_DEFAULT);
                    pipelineDesc.SampleMask = UINT_MAX;

                    // Blend
                    if (curState.blend.enable)
                    {
                        D3D12_BLEND_DESC blendDesc = CD3D12_BLEND_DESC(D3D12_DEFAULT);
                        blendDesc.AlphaToCoverageEnable = FALSE;
                        blendDesc.IndependentBlendEnable = FALSE;

                        D3D12_RENDER_TARGET_BLEND_DESC renderTartget{};
                        renderTartget.BlendEnable = TRUE;
                        renderTartget.LogicOpEnable = FALSE;
                        renderTartget.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                        renderTartget.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                        renderTartget.BlendOp = D3D12_BLEND_OP_ADD;
                        renderTartget.SrcBlendAlpha = D3D12_BLEND_ONE;
                        renderTartget.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                        renderTartget.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                        renderTartget.LogicOp = D3D12_LOGIC_OP_NOOP;
                        renderTartget.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

                        for (UINT i = 0; i < 8; ++i)
                        {
                            blendDesc.RenderTarget[i] = renderTartget;
                        }

                        pipelineDesc.BlendState = blendDesc;
                    }

                    // Rasterizer from current state
                    pipelineDesc.RasterizerState = CD3D12_RASTERIZER_DESC(D3D12_DEFAULT);
                    pipelineDesc.RasterizerState.CullMode = ToD3DCull(curState.rasterizer.cullMode);
                    pipelineDesc.RasterizerState.FrontCounterClockwise = (curState.rasterizer.frontFace == FrontFace::CounterClockwise) ? TRUE : FALSE;

                    // Depth
                    pipelineDesc.DepthStencilState = CD3D12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
                    pipelineDesc.DepthStencilState.DepthEnable = curState.depth.testEnable ? TRUE : FALSE;
                    pipelineDesc.DepthStencilState.DepthWriteMask = curState.depth.writeEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
                    pipelineDesc.DepthStencilState.DepthFunc = ToD3DCompare(curState.depth.depthCompareOp);

                    pipelineDesc.InputLayout = { layIt->second.elems.data(), static_cast<UINT>(layIt->second.elems.size()) };
                    pipelineDesc.PrimitiveTopologyType = ToD3DTopologyType(pit->second.topologyType);

                    pipelineDesc.NumRenderTargets = curNumRT;
                    for (UINT i = 0; i < curNumRT; ++i)
                    {
                        pipelineDesc.RTVFormats[i] = curRTVFormats[i];
                    }
                    pipelineDesc.DSVFormat = curDSVFormat;

                    pipelineDesc.SampleDesc.Count = 1;

                    ComPtr<ID3D12PipelineState> pso;
                    
                    if (pit->second.viewInstanceCount > 1)
                    {
                        if (!device2_)
                        {
                            // View instancing is optional; fail softly so the renderer can fallback to 6-pass.
                            return nullptr;
                        }
                        // Build PSO via Pipeline State Stream to enable View Instancing.

                        using SO_RootSig = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>;
                        using SO_VS = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE>;
                        using SO_PS = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>;
                        using SO_Blend = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>;
                        using SO_SampleMask = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT>;
                        using SO_Raster = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>;
                        using SO_Depth = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC>;
                        using SO_Input = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC>;
                        using SO_Topo = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE>;
                        using SO_RTVFmts = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>;
                        using SO_DSVFmt = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>;
                        using SO_SampleDesc = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC>;
                        using SO_ViewInst = PSOSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC>;

                        // Each stream subobject must be pointer-aligned, and its size should be a multiple of sizeof(void*)
                        // so the next Type is correctly aligned in the byte stream.
                        static_assert(sizeof(SO_RootSig) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_VS) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_PS) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_Blend) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_SampleMask) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_Raster) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_Depth) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_Input) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_Topo) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_RTVFmts) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_DSVFmt) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_SampleDesc) % sizeof(void*) == 0);
                        static_assert(sizeof(SO_ViewInst) % sizeof(void*) == 0);

                        const std::uint32_t viewCount = pit->second.viewInstanceCount;
                        std::array<D3D12_VIEW_INSTANCE_LOCATION, 8> locations{};
                        if (viewCount > locations.size())
                        {
                            // View instancing is optional; fail softly so the renderer can fallback to 6-pass.
                            return nullptr;
                        }
                        for (std::uint32_t i = 0; i < viewCount; ++i)
                        {
                            locations[i].RenderTargetArrayIndex = i;
                            locations[i].ViewportArrayIndex = 0;
                        }

                        D3D12_VIEW_INSTANCING_DESC viDesc{};
                        viDesc.ViewInstanceCount = viewCount;
                        viDesc.pViewInstanceLocations = locations.data();
                        viDesc.Flags = D3D12_VIEW_INSTANCING_FLAG_NONE;

                        D3D12_RT_FORMAT_ARRAY rtFmts{};
                        rtFmts.NumRenderTargets = curNumRT;
                        for (UINT i = 0; i < curNumRT; ++i)
                        {
                            rtFmts.RTFormats[i] = curRTVFormats[i];
                        }

                        struct alignas(void*) PSOStream
                        {
                            SO_RootSig    rootSig;
                            SO_VS         vs;
                            SO_PS         ps;
                            SO_Blend      blend;
                            SO_SampleMask sampleMask;
                            SO_Raster     raster;
                            SO_Depth      depth;
                            SO_Input      input;
                            SO_Topo       topo;
                            SO_RTVFmts    rtvFmts;
                            SO_DSVFmt     dsvFmt;
                            SO_SampleDesc sampleDesc;
                            SO_ViewInst   viewInst;
                        } stream{};

                        stream.rootSig.data = rootSig_.Get();
                        stream.vs.data = pipelineDesc.VS;
                        stream.ps.data = pipelineDesc.PS;
                        stream.blend.data = pipelineDesc.BlendState;
                        stream.sampleMask.data = pipelineDesc.SampleMask;
                        stream.raster.data = pipelineDesc.RasterizerState;
                        stream.depth.data = pipelineDesc.DepthStencilState;
                        stream.input.data = pipelineDesc.InputLayout;
                        stream.topo.data = pipelineDesc.PrimitiveTopologyType;
                        stream.rtvFmts.data = rtFmts;
                        stream.dsvFmt.data = pipelineDesc.DSVFormat;
                        stream.sampleDesc.data = pipelineDesc.SampleDesc;
                        stream.viewInst.data = viDesc;

                        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{};
                        streamDesc.SizeInBytes = sizeof(stream);
                        streamDesc.pPipelineStateSubobjectStream = &stream;

                        const HRESULT hr = device2_->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pso));
                        if (FAILED(hr))
                        {
                            // View instancing is optional; fail softly so the renderer can fallback to 6-pass.
                            ThrowIfFailed(NativeDevice()->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pso)),
                                "DX12: CreateGraphicsPipelineState failed");
                        }
                    }
                    else
                    {
                        ThrowIfFailed(NativeDevice()->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pso)),
                            "DX12: CreateGraphicsPipelineState failed");
                    }

                    if (!pso)
                    {
                        return nullptr;
                    }

                    psoCache_[key] = pso;
                    return pso.Get();
                };

            // Parse high-level commands and record native D3D12
            for (auto& command : commandList.commands)
            {
                std::visit([&](auto&& cmd)
                    {
                        using T = std::decay_t<decltype(cmd)>;

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

                                dsv = sc->DSV();
                                hasDSV = (dsv.ptr != 0);
                                curDSVFormat = sc->DepthFormat();

                                const D3D12_CPU_DESCRIPTOR_HANDLE* rtvPtr = rtvs.data();
                                const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = hasDSV ? &dsv : nullptr;
                                cmdList_->OMSetRenderTargets(numRT, rtvPtr, FALSE, dsvPtr);

                                if (c.clearColor)
                                {
                                    const float* col = c.color.data();
                                    cmdList_->ClearRenderTargetView(rtvs[0], col, 0, nullptr);
                                }
                                if (c.clearDepth && hasDSV)
                                {
                                    cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, c.depth, 0, 0, nullptr);
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

                                // Color (0 or 1 RT)
                                if (fb.color)
                                {
                                    auto it = textures_.find(fb.color.id);
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
                                    
                                        Barrier(te.resource.Get(), te.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                                    
                                        rtvs[0] = te.rtvAllFaces;
                                        numRT = 1;
                                        curRTVFormats[0] = te.rtvFormat;
                                    }
                                    else if (fb.colorCubeFace != 0xFFFFFFFFu)
                                    {
                                        if (!te.hasRTVFaces)
                                        {
                                            throw std::runtime_error("DX12: CommandBeginPass: cubemap color texture has no RTV faces");
                                        }
                                        if (fb.colorCubeFace >= 6)
                                        {
                                            throw std::runtime_error("DX12: CommandBeginPass: cubemap face index out of range");
                                        }
                                    
                                        Barrier(te.resource.Get(), te.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                                    
                                        rtvs[0] = te.rtvFaces[fb.colorCubeFace];
                                        numRT = 1;
                                        curRTVFormats[0] = te.rtvFormat;
                                    }
                                    else
                                    {
                                        if (!te.hasRTV)
                                        {
                                            throw std::runtime_error("DX12: CommandBeginPass: color texture has no RTV");
                                        }
                                    
                                        Barrier(te.resource.Get(), te.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                                    
                                        rtvs[0] = te.rtv;
                                        numRT = 1;
                                        curRTVFormats[0] = te.rtvFormat;
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
                                        Barrier(te.resource.Get(), te.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                                    
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
                                    
                                        Barrier(te.resource.Get(), te.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                                    
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

                                if (c.clearDepth && hasDSV)
                                {
                                    cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, c.depth, 0, 0, nullptr);
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
                        else if constexpr (std::is_same_v<T, CommandSetState>)
                        {
                            curState = cmd.state;
                        }
                        else if constexpr (std::is_same_v<T, CommandSetPrimitiveTopology>)
                        {
                            currentTopology = ToD3DTopology(cmd.topology);
                        }
                        else if constexpr (std::is_same_v<T, CommandBindPipeline>)
                        {
                            curPipe = cmd.pso;
                        }
                        else if constexpr (std::is_same_v<T, CommandBindInputLayout>)
                        {
                            curLayout = cmd.layout;
                        }
                        else if constexpr (std::is_same_v<T, CommandBindVertexBuffer>)
                        {
                            if (cmd.slot >= kMaxVBSlots)
                            {
                                throw std::runtime_error("DX12: BindVertexBuffer: slot out of range");
                            }
                            vertexBuffers[cmd.slot] = cmd.buffer;
                            vbStrides[cmd.slot] = cmd.strideBytes;
                            vbOffsets[cmd.slot] = cmd.offsetBytes;
                        }
                        else if constexpr (std::is_same_v<T, CommandBindIndexBuffer>)
                        {
                            indexBuffer = cmd.buffer;
                            ibType = cmd.indexType;
                            ibOffset = cmd.offsetBytes;
                        }
                        else if constexpr (std::is_same_v<T, CommnadBindTexture2D>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                auto it = textures_.find(cmd.texture.id);
                                if (it == textures_.end())
                                {
                                    throw std::runtime_error("DX12: BindTexture2D: texture not found in textures_ map");
                                }

                                if (!it->second.hasSRV)
                                {
                                    throw std::runtime_error("DX12: BindTexture2D: texture has no SRV");
                                }

                                TransitionTexture(cmd.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                boundTex[cmd.slot] = GetTextureSRV(cmd.texture);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandBindTextureCube>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                auto it = textures_.find(cmd.texture.id);
                                if (it == textures_.end())
                                {
                                    throw std::runtime_error("DX12: BindTextureCube: texture not found in textures_ map");
                                }

                                if (!it->second.hasSRV)
                                {
                                    throw std::runtime_error("DX12: BindTextureCube: texture has no SRV");
                                }

                                TransitionTexture(cmd.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                boundTex[cmd.slot] = GetTextureSRV(cmd.texture);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandTextureDesc>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                TextureHandle handle = ResolveTextureHandleFromDesc(cmd.texture);
                                if (!handle)
                                {
                                    // null SRV
                                    boundTex[cmd.slot] = srvHeap_->GetGPUDescriptorHandleForHeapStart();
                                    return;
                                }

                                TransitionTexture(handle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                boundTex[cmd.slot] = GetTextureSRV(handle);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandBindStructuredBufferSRV>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                boundTex[cmd.slot] = GetBufferSRV(cmd.buffer);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandSetUniformInt> ||
                            std::is_same_v<T, CommandUniformFloat4> ||
                            std::is_same_v<T, CommandUniformMat4>)
                        {
                            // DX12 backend does not interpret the name-based uniform commands.
                            // Use CommandSetConstants instead.
                        }
                        else if constexpr (std::is_same_v<T, CommandSetConstants>)
                        {
                            perDrawSlot = cmd.slot;
                            perDrawSize = cmd.size;
                            if (perDrawSize > kMaxPerDrawConstantsBytes)
                            {
                                perDrawSize = kMaxPerDrawConstantsBytes;
                            }
                            if (perDrawSize != 0)
                            {
                                std::memcpy(perDrawBytes.data(), cmd.data.data(), perDrawSize);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandDrawIndexed>)
                        {
                            // PSO + RootSig
                            ID3D12PipelineState* pso = EnsurePSO(curPipe, curLayout);
                            cmdList_->SetPipelineState(pso);
                            cmdList_->SetGraphicsRootSignature(rootSig_.Get());

                            // IA bindings (slot0..slotN based on input layout)
                            auto layIt = layouts_.find(curLayout.id);
                            if (layIt == layouts_.end())
                            {
                                throw std::runtime_error("DX12: input layout handle not found");
                            }

                            std::uint32_t maxSlot = 0;
                            for (const auto& e : layIt->second.elems)
                            {
                                maxSlot = std::max(maxSlot, static_cast<std::uint32_t>(e.InputSlot));
                            }
                            const std::uint32_t numVB = maxSlot + 1;
                            if (numVB > kMaxVBSlots)
                            {
                                throw std::runtime_error("DX12: input layout uses more VB slots than supported");
                            }

                            std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVBSlots> vbv{};
                            for (std::uint32_t s = 0; s < numVB; ++s)
                            {
                                if (!vertexBuffers[s])
                                {
                                    throw std::runtime_error("DX12: missing vertex buffer binding for required slot");
                                }
                                auto vbIt = buffers_.find(vertexBuffers[s].id);
                                if (vbIt == buffers_.end())
                                {
                                    throw std::runtime_error("DX12: vertex buffer not found");
                                }

                                const std::uint32_t off = vbOffsets[s];
                                vbv[s].BufferLocation = vbIt->second.resource->GetGPUVirtualAddress() + off;
                                vbv[s].SizeInBytes = (UINT)(vbIt->second.desc.sizeInBytes - off);
                                vbv[s].StrideInBytes = vbStrides[s];
                            }
                            cmdList_->IASetVertexBuffers(0, numVB, vbv.data());
                            cmdList_->IASetPrimitiveTopology(currentTopology);

                            if (indexBuffer)
                            {
                                auto ibIt = buffers_.find(indexBuffer.id);
                                if (ibIt == buffers_.end())
                                {
                                    throw std::runtime_error("DX12: index buffer not found");
                                }
                                D3D12_INDEX_BUFFER_VIEW ibv{};
                                ibv.BufferLocation = ibIt->second.resource->GetGPUVirtualAddress() + ibOffset
                                    + static_cast<UINT64>(cmd.firstIndex) * static_cast<UINT64>(IndexSizeBytes(cmd.indexType));
                                ibv.SizeInBytes = static_cast<UINT>(ibIt->second.desc.sizeInBytes - ibOffset);
                                ibv.Format = (cmd.indexType == IndexType::UINT16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

                                cmdList_->IASetIndexBuffer(&ibv);
                            }

                            // Root bindings: CBV (0) + SRV table (1)
                            WriteCBAndBind();

                            for (UINT i = 0; i < kMaxSRVSlots; ++i)
                            {
                                cmdList_->SetGraphicsRootDescriptorTable(1 + i, boundTex[i]);
                            }

                            cmdList_->DrawIndexedInstanced(cmd.indexCount, cmd.instanceCount, 0, cmd.baseVertex, cmd.firstInstance);
                        }
                        else if constexpr (std::is_same_v<T, CommandDraw>)
                        {
                            ID3D12PipelineState* pso = EnsurePSO(curPipe, curLayout);
                            cmdList_->SetPipelineState(pso);
                            cmdList_->SetGraphicsRootSignature(rootSig_.Get());

                            // IA bindings (slot0..slotN based on input layout)
                            auto layIt = layouts_.find(curLayout.id);
                            if (layIt == layouts_.end())
                            {
                                throw std::runtime_error("DX12: input layout handle not found");
                            }

                            std::uint32_t maxSlot = 0;
                            for (const auto& e : layIt->second.elems)
                            {
                                maxSlot = std::max(maxSlot, static_cast<std::uint32_t>(e.InputSlot));
                            }
                            const std::uint32_t numVB = maxSlot + 1;
                            if (numVB > kMaxVBSlots)
                            {
                                throw std::runtime_error("DX12: input layout uses more VB slots than supported");
                            }

                            std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVBSlots> vbv{};
                            for (std::uint32_t s = 0; s < numVB; ++s)
                            {
                                if (!vertexBuffers[s])
                                {
                                    throw std::runtime_error("DX12: missing vertex buffer binding for required slot");
                                }
                                auto vbIt = buffers_.find(vertexBuffers[s].id);
                                if (vbIt == buffers_.end())
                                {
                                    throw std::runtime_error("DX12: vertex buffer not found");
                                }

                                const std::uint32_t off = vbOffsets[s];
                                vbv[s].BufferLocation = vbIt->second.resource->GetGPUVirtualAddress() + off;
                                vbv[s].SizeInBytes = (UINT)(vbIt->second.desc.sizeInBytes - off);
                                vbv[s].StrideInBytes = vbStrides[s];
                            }
                            cmdList_->IASetVertexBuffers(0, numVB, vbv.data());
                            cmdList_->IASetPrimitiveTopology(currentTopology);

                            WriteCBAndBind();

                            for (UINT i = 0; i < kMaxSRVSlots; ++i)
                            {
                                cmdList_->SetGraphicsRootDescriptorTable(1 + i, boundTex[i]);
                            }

                            cmdList_->DrawInstanced(cmd.vertexCount, cmd.instanceCount, cmd.firstVertex, cmd.firstInstance);
                        }
                        else if constexpr (std::is_same_v<T, CommandDX12ImGuiRender>)
                        {
                            if (!imguiInitialized_ || !cmd.drawData)
                            {
                                return;
                            }

                            // Ensure ImGui sees the same shader-visible heap.
                            ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
                            cmdList_->SetDescriptorHeaps(1, heaps);

                            ImGui_ImplDX12_RenderDrawData(reinterpret_cast<ImDrawData*>(const_cast<void*>(cmd.drawData)), cmdList_.Get());
                        }
                        else if constexpr (std::is_same_v<T, CommandBindTexture2DArray>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                auto it = textures_.find(cmd.texture.id);
                                if (it == textures_.end())
                                {
                                    throw std::runtime_error("DX12: BindTexture2DArray: texture not found in textures_ map");
                                }

                                // Ensure an Array SRV exists for cube textures.
                                if (!it->second.hasSRVArray)
                                {
                                    const auto desc = it->second.resource->GetDesc();
                                    AllocateSRV_CubeAsArray(it->second, it->second.srvFormat, desc.MipLevels);
                                }

                                TransitionTexture(cmd.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                boundTex[cmd.slot] = it->second.srvGpuArray;
                            }
                        }
                        else
                        {
                            // other commands ignored
                        }

                    }, command);
            }

            // Close + execute + signal fence for the current frame resource
            EndFrame();
        }

        // ---------------- Bindless descriptor indices ----------------
        TextureDescIndex AllocateTextureDesctiptor(TextureHandle texture) override
        {
            // 0 = invalid
            TextureDescIndex idx{};
            if (!freeTexDesc_.empty())
            {
                idx = freeTexDesc_.back();
                freeTexDesc_.pop_back();
            }
            else
            {
                idx = TextureDescIndex{ nextTexDesc_++ };
            }

            UpdateTextureDescriptor(idx, texture);
            return idx;
        }

        void UpdateTextureDescriptor(TextureDescIndex idx, TextureHandle tex) override
        {
            if (!tex)
            {
                descToTex_[idx] = {};
                return;
            }

            descToTex_[idx] = tex;

            auto it = textures_.find(tex.id);
            if (it == textures_.end())
            {
                throw std::runtime_error("DX12: UpdateTextureDescriptor: texture not found");
            }

            auto& te = it->second;
            if (!te.hasSRV)
            {
                if (te.srvFormat == DXGI_FORMAT_UNKNOWN)
                {
                    throw std::runtime_error("DX12: UpdateTextureDescriptor: texture has no SRV format");
                }


                AllocateSRV(te, te.srvFormat, /*mips*/ 1);
            }
        }

        void FreeTextureDescriptor(TextureDescIndex index) noexcept override
        {
            descToTex_.erase(index);
            freeTexDesc_.push_back(index);
        }

        // ---------------- Fences (минимально) ----------------
        FenceHandle CreateFence(bool signaled = false) override
        {
            const auto id = ++nextFenceId_;
            fences_[id] = signaled;
            return FenceHandle{ id };
        }

        void DestroyFence(FenceHandle fence) noexcept override
        {
            fences_.erase(fence.id);
        }

        void SignalFence(FenceHandle fence) override
        {
            fences_[fence.id] = true;
        }

        void WaitFence(FenceHandle) override {}

        bool IsFenceSignaled(FenceHandle fence) override
        {
            auto it = fences_.find(fence.id);
            return it != fences_.end() && it->second;
        }

        ID3D12Device* NativeDevice() const
        {
            return core_.device.Get();
        }
        ID3D12CommandQueue* NativeQueue() const
        {
            return core_.cmdQueue.Get();
        }
        ID3D12DescriptorHeap* NativeSRVHeap() const
        {
            return srvHeap_.Get();
        }
        UINT NativeSRVInc() const
        {
            return srvInc_;
        }

    private:
        friend class DX12SwapChain;

        struct BufferEntry
        {
            BufferDesc desc{};
            ComPtr<ID3D12Resource> resource;

            // Track state for proper COPY_DEST transitions when uploading.
            D3D12_RESOURCE_STATES state{ D3D12_RESOURCE_STATE_COMMON };

            // Optional SRV for StructuredBuffer reads (t2 in the demo).
            bool hasSRV{ false };
            UINT srvIndex{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};

            // Optional SRV view for cube textures exposed as a 2D array (6 slices).
            bool hasSRVArray{ false };
            UINT srvIndexArray{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE srvCpuArray{};
            D3D12_GPU_DESCRIPTOR_HANDLE srvGpuArray{};
        };

        struct InputLayoutEntry
        {
            std::vector<std::string> semanticStorage;
            std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
            std::uint32_t strideBytes{ 0 };
        };

        struct ShaderEntry
        {
            ShaderStage stage{};
            std::string name;
            ComPtr<ID3DBlob> blob;
        };

        struct PipelineEntry
        {
            std::string debugName;
            ShaderHandle vs{};
            ShaderHandle ps{};
			PrimitiveTopologyType topologyType{};
            std::uint32_t viewInstanceCount{ 1 };
        };

        struct TextureEntry
        {
            enum class Type : std::uint8_t
            {
                Tex2D,
                Cube
            };

            TextureHandle handle{};
            Extent2D extent{};
            Format format{ Format::Unknown };
            Type type{ Type::Tex2D };

            ComPtr<ID3D12Resource> resource;

            DXGI_FORMAT resourceFormat{ DXGI_FORMAT_UNKNOWN };
            DXGI_FORMAT srvFormat{ DXGI_FORMAT_UNKNOWN };
            DXGI_FORMAT rtvFormat{ DXGI_FORMAT_UNKNOWN };
            DXGI_FORMAT dsvFormat{ DXGI_FORMAT_UNKNOWN };

            D3D12_RESOURCE_STATES state{ D3D12_RESOURCE_STATE_COMMON };

            bool hasSRV{ false };
            UINT srvIndex{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};

            // For Tex2D render targets
            bool hasRTV{ false };
            UINT rtvIndex{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};

            // For cubemap render targets (one RTV per face)
            bool hasRTVFaces{ false };
            std::array<UINT, 6> rtvIndexFaces{};
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 6> rtvFaces{};


            // For cubemap render targets (RTV for all 6 faces as a Texture2DArray view)
            bool hasRTVAllFaces{ false };
            UINT rtvIndexAllFaces{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE rtvAllFaces{};
            bool hasDSV{ false };
            UINT dsvIndex{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE dsv{};

            // For cubemap depth targets (DSV for all 6 faces as a Texture2DArray view)
            bool hasDSVAllFaces{ false };
            UINT dsvIndexAllFaces{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE dsvAllFaces{};

            // Optional SRV view for cube textures exposed as a 2D array (6 slices).
            bool hasSRVArray{ false };
            UINT srvIndexArray{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE srvCpuArray{};
            D3D12_GPU_DESCRIPTOR_HANDLE srvGpuArray{};
        };


        struct FramebufferEntry
        {
            TextureHandle color{};
            TextureHandle depth{};

            // UINT32_MAX means "regular 2D color attachment".
            std::uint32_t colorCubeFace{ 0xFFFFFFFFu };

            // If true, bind the cubemap RTV/DSV as a 2D array view with ArraySize=6 (View-Instancing path).
            bool colorCubeAllFaces{ false };
        };

        static constexpr std::uint32_t kFramesInFlight = 3;
        static constexpr UINT kPerFrameCBUploadBytes = 512u * 1024u;
        static constexpr UINT kPerFrameBufUploadBytes = 8u * 1024u * 1024u; // 8 MB per frame buffer upload ring
        static constexpr UINT kMaxSRVSlots = 20; // t0..t19 (room for PBR maps + env)
        static constexpr UINT kSrvHeapNumDescriptors = 16384u; // CBV/SRV/UAV shader-visible heap size

        struct FrameResource
        {
            ComPtr<ID3D12CommandAllocator> cmdAlloc;

            // Small persistent upload buffer for per-draw constants.
            ComPtr<ID3D12Resource> cbUpload;
            std::byte* cbMapped{ nullptr };
            std::uint32_t cbCursor{ 0 };

            // Per-frame upload buffer for dynamic DEFAULT buffers (lights/instances/etc).
            ComPtr<ID3D12Resource> bufUpload;
            std::byte* bufMapped{ nullptr };
            std::uint32_t bufCursor{ 0 };

            // Fence value that marks when GPU finished using this frame resource.
            UINT64 fenceValue{ 0 };

            // Deferred lifetime management:
            //  - keep resources alive until GPU is done with this frame
            //  - recycle descriptor indices only after the same fence is completed
            std::vector<ComPtr<ID3D12Resource>> deferredResources;
            std::vector<UINT> deferredFreeSrv;
            std::vector<UINT> deferredFreeRtv;
            std::vector<UINT> deferredFreeDsv;

            void ResetForRecording() noexcept
            {
                cbCursor = 0;
                bufCursor = 0;
            }

            void ReleaseDeferred(
                std::vector<UINT>& globalFreeSrv,
                std::vector<UINT>& globalFreeRtv,
                std::vector<UINT>& globalFreeDsv)
            {
                deferredResources.clear();

                globalFreeSrv.insert(globalFreeSrv.end(), deferredFreeSrv.begin(), deferredFreeSrv.end());
                globalFreeRtv.insert(globalFreeRtv.end(), deferredFreeRtv.begin(), deferredFreeRtv.end());
                globalFreeDsv.insert(globalFreeDsv.end(), deferredFreeDsv.begin(), deferredFreeDsv.end());

                deferredFreeSrv.clear();
                deferredFreeRtv.clear();
                deferredFreeDsv.clear();
            }
        };

        void WaitForFence(UINT64 v)
        {
            if (v == 0)
                return;

            if (fence_->GetCompletedValue() < v)
            {
                ThrowIfFailed(fence_->SetEventOnCompletion(v, fenceEvent_), "DX12: SetEventOnCompletion failed");
                WaitForSingleObject(fenceEvent_, INFINITE);
            }
        }

        void ImmediateUploadBuffer(BufferEntry& dst, std::span<const std::byte> data, std::size_t dstOffsetBytes)
        {
            if (!dst.resource || data.empty())
                return;

            // Temp upload resource
            ComPtr<ID3D12Resource> upload;

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resourceDesc.Width = std::max<UINT64>(1, static_cast<UINT64>(data.size()));
            resourceDesc.Height = 1;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ThrowIfFailed(NativeDevice()->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&upload)),
                "DX12: ImmediateUploadBuffer - Create upload resource failed");

            void* mapped = nullptr;
            ThrowIfFailed(upload->Map(0, nullptr, &mapped), "DX12: ImmediateUploadBuffer - Map upload failed");
            std::memcpy(mapped, data.data(), data.size());
            upload->Unmap(0, nullptr);

            // Record tiny copy list
            ComPtr<ID3D12CommandAllocator> alloc;
            ThrowIfFailed(NativeDevice()->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&alloc)),
                "DX12: ImmediateUploadBuffer - CreateCommandAllocator failed");

            ComPtr<ID3D12GraphicsCommandList> cl;
            ThrowIfFailed(NativeDevice()->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                alloc.Get(),
                nullptr,
                IID_PPV_ARGS(&cl)),
                "DX12: ImmediateUploadBuffer - CreateCommandList failed");

            auto Transition = [&](D3D12_RESOURCE_STATES desired)
                {
                    if (dst.state == desired) return;
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = dst.resource.Get();
                    b.Transition.StateBefore = dst.state;
                    b.Transition.StateAfter = desired;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    cl->ResourceBarrier(1, &b);
                    dst.state = desired;
                };

            Transition(D3D12_RESOURCE_STATE_COPY_DEST);

            cl->CopyBufferRegion(
                dst.resource.Get(),
                static_cast<UINT64>(dstOffsetBytes),
                upload.Get(),
                0,
                static_cast<UINT64>(data.size()));

            Transition(D3D12_RESOURCE_STATE_GENERIC_READ);

            ThrowIfFailed(cl->Close(), "DX12: ImmediateUploadBuffer - Close failed");

            ID3D12CommandList* lists[] = { cl.Get() };
            NativeQueue()->ExecuteCommandLists(1, lists);

            const UINT64 v = ++fenceValue_;
            ThrowIfFailed(NativeQueue()->Signal(fence_.Get(), v), "DX12: ImmediateUploadBuffer - Signal failed");
            WaitForFence(v);
        }

        void FlushPendingBufferUpdates()
        {
            if (pendingBufferUpdates_.empty())
                return;

            FrameResource& fr = CurrentFrame();

            for (const PendingBufferUpdate& u : pendingBufferUpdates_)
            {
                auto it = buffers_.find(u.buffer.id);
                if (it == buffers_.end()) continue;

                BufferEntry& dst = it->second;
                if (!dst.resource || u.data.empty()) continue;

                const std::uint32_t size = static_cast<std::uint32_t>(u.data.size());
                const std::uint32_t aligned = AlignUp(size, 16u);

                if (fr.bufCursor + aligned > kPerFrameBufUploadBytes)
                {
                    throw std::runtime_error("DX12: per-frame buffer upload ring overflow (increase kPerFrameBufUploadBytes)");
                }

                std::memcpy(fr.bufMapped + fr.bufCursor, u.data.data(), size);

                auto Transition = [&](D3D12_RESOURCE_STATES desired)
                    {
                        if (dst.state == desired) return;
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = dst.resource.Get();
                        b.Transition.StateBefore = dst.state;
                        b.Transition.StateAfter = desired;
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        cmdList_->ResourceBarrier(1, &b);
                        dst.state = desired;
                    };

                Transition(D3D12_RESOURCE_STATE_COPY_DEST);

                cmdList_->CopyBufferRegion(
                    dst.resource.Get(),
                    static_cast<UINT64>(u.dstOffsetBytes),
                    fr.bufUpload.Get(),
                    static_cast<UINT64>(fr.bufCursor),
                    static_cast<UINT64>(size));

                Transition(D3D12_RESOURCE_STATE_GENERIC_READ);

                fr.bufCursor += aligned;
            }

            pendingBufferUpdates_.clear();
        }

        FrameResource& CurrentFrame() noexcept
        {
            return frames_[activeFrameIndex_];
        }

        void BeginFrame()
        {
            activeFrameIndex_ = static_cast<std::uint32_t>(submitIndex_ % kFramesInFlight);
            ++submitIndex_;

            FrameResource& fr = frames_[activeFrameIndex_];

            // Wait until GPU is done with this frame resource, then recycle deferred objects/indices.
            WaitForFence(fr.fenceValue);
            fr.ReleaseDeferred(freeSrv_, freeRTV_, freeDSV_);

            ThrowIfFailed(fr.cmdAlloc->Reset(), "DX12: cmdAlloc reset failed");
            ThrowIfFailed(cmdList_->Reset(fr.cmdAlloc.Get(), nullptr), "DX12: cmdList reset failed");

            fr.ResetForRecording();
        }

        void EndFrame()
        {
            ThrowIfFailed(cmdList_->Close(), "DX12: cmdList close failed");

            ID3D12CommandList* lists[] = { cmdList_.Get() };
            NativeQueue()->ExecuteCommandLists(1, lists);

            const UINT64 v = ++fenceValue_;
            ThrowIfFailed(NativeQueue()->Signal(fence_.Get(), v), "DX12: Signal failed");
            frames_[activeFrameIndex_].fenceValue = v;
        }

        void FlushGPU()
        {
            const UINT64 v = ++fenceValue_;
            ThrowIfFailed(NativeQueue()->Signal(fence_.Get(), v), "DX12: Signal failed");
            WaitForFence(v);
        }

        void CreateRootSignature()
        {
            // Root signature layout:
            //  [0]  CBV(b0)   - per-draw constants
            //  [1+] SRV(t0+)  - individual SRV descriptor tables (1 descriptor each)
            //
            // We deliberately use one-descriptor tables per register to allow binding arbitrary
            // SRV heap entries without requiring contiguous descriptor ranges.
            //
            // SRV registers used by shaders:
            //  t0      - albedo (Texture2D)
            //  t1      - directional shadow map (Texture2D<float>)
            //  t2      - lights (StructuredBuffer<GPULight>)
            //  t3..t6  - spot shadow maps [0..3] (Texture2D<float>)
            //  t7..t10 - point distance cubemaps [0..3] (TextureCube<float>)
            //  t11     - shadow metadata (StructuredBuffer<ShadowDataSB>)
            //
            //  DX12 PBR extras (main forward shader):
            //  t12 normal (Texture2D)
            //  t13 metalness (Texture2D)
            //  t14 roughness (Texture2D)
            //  t15 ao (Texture2D)
            //  t16 emissive (Texture2D)
            //  t17 env cube (TextureCube)
            //
            // Samplers:

            //  s0 - linear wrap
            //  s1 - comparison sampler for shadow maps (clamp)
            //  s2 - point clamp (used by point shadows)
            //  s3 - linear clamp (used by skybox/env cubemaps)
            std::array<D3D12_DESCRIPTOR_RANGE, kMaxSRVSlots> ranges{};
            for (UINT i = 0; i < kMaxSRVSlots; ++i)
            {
                ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                ranges[i].NumDescriptors = 1;
                ranges[i].BaseShaderRegister = i; // ti
                ranges[i].RegisterSpace = 0;
                ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            }

            std::array<D3D12_ROOT_PARAMETER, 1 + kMaxSRVSlots> rootParams{};

            // b0
            rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rootParams[0].Descriptor.ShaderRegister = 0;
            rootParams[0].Descriptor.RegisterSpace = 0;
            rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            // t0..t11
            for (UINT i = 0; i < kMaxSRVSlots; ++i)
            {
                rootParams[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rootParams[1 + i].DescriptorTable.NumDescriptorRanges = 1;
                rootParams[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
                rootParams[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            }

            D3D12_STATIC_SAMPLER_DESC samplers[4]{};

            auto MakeStaticSampler =
                [](UINT reg,
                    D3D12_FILTER filter,
                    D3D12_TEXTURE_ADDRESS_MODE addrU,
                    D3D12_TEXTURE_ADDRESS_MODE addrV,
                    D3D12_TEXTURE_ADDRESS_MODE addrW,
                    D3D12_COMPARISON_FUNC cmp,
                    D3D12_STATIC_BORDER_COLOR borderColor)
                {
                    D3D12_STATIC_SAMPLER_DESC s{};
                    s.ShaderRegister = reg;
                    s.Filter = filter;
                    s.AddressU = addrU;
                    s.AddressV = addrV;
                    s.AddressW = addrW;
                    s.MipLODBias = 0.0f;
                    s.MaxAnisotropy = 1;
                    s.ComparisonFunc = cmp;
                    s.BorderColor = borderColor;
                    s.MinLOD = 0.0f;
                    s.MaxLOD = D3D12_FLOAT32_MAX;
                    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                    return s;
                };

            // s0: linear wrap
            samplers[0] = MakeStaticSampler(
                0,
                D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                D3D12_COMPARISON_FUNC_ALWAYS,
                D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

            // s1: shadow comparison sampler (clamp)
            samplers[1] = MakeStaticSampler(
                1,
		        // Point filter + explicit PCF in shader: keeps contact edges crisp and avoids "mushy" shadows.
		        D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT,
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                D3D12_COMPARISON_FUNC_LESS_EQUAL,
                D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

            // s2: point clamp
            samplers[2] = MakeStaticSampler(
                2,
                D3D12_FILTER_MIN_MAG_MIP_POINT,
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                D3D12_COMPARISON_FUNC_ALWAYS,
                D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

            // s3: linear clamp (cubemaps: skybox / IBL env)
            samplers[3] = MakeStaticSampler(
                3,
                D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                D3D12_COMPARISON_FUNC_ALWAYS,
                D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

            D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
            rootSigDesc.NumParameters = static_cast<UINT>(rootParams.size());
            rootSigDesc.pParameters = rootParams.data();
            rootSigDesc.NumStaticSamplers = 4;
            rootSigDesc.pStaticSamplers = samplers;
            rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

            ComPtr<ID3DBlob> serialized;
            ComPtr<ID3DBlob> error;

            HRESULT hr = D3D12SerializeRootSignature(
                &rootSigDesc,
                D3D_ROOT_SIGNATURE_VERSION_1,
                serialized.GetAddressOf(),
                error.GetAddressOf());

            if (FAILED(hr))
            {
                std::string msg = "DX12: D3D12SerializeRootSignature failed";
                if (error)
                {
                    msg += ": ";
                    msg += static_cast<const char*>(error->GetBufferPointer());
                }
                throw std::runtime_error(msg);
            }

            ThrowIfFailed(NativeDevice()->CreateRootSignature(
                0,
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(rootSig_.ReleaseAndGetAddressOf())),
                "DX12: CreateRootSignature failed");
        }


        void EnsureRTVHeap()
        {
            if (rtvHeap_)
            {
                return;
            }
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = 256;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            ThrowIfFailed(NativeDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_)),
                "DX12: Create RTV heap failed");
            rtvInc_ = NativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            nextRTV_ = 0;
            freeRTV_.clear();
        }

        void EnsureDSVHeap()
        {
            if (dsvHeap_)
            {
                return;
            }
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            heapDesc.NumDescriptors = 256;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            ThrowIfFailed(NativeDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap_)),
                "DX12: Create DSV heap failed");
            dsvInc_ = NativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            nextDSV_ = 0;
            freeDSV_.clear();
        }

        D3D12_CPU_DESCRIPTOR_HANDLE AllocateRTV(ID3D12Resource* res, DXGI_FORMAT fmt, UINT& outIndex)
        {
            UINT idx = 0;
            if (!freeRTV_.empty())
            {
                idx = freeRTV_.back();
                freeRTV_.pop_back();
            }
            else
            {
                idx = nextRTV_++;
            }
            outIndex = idx;

            if (idx >= 256u)
            {
                throw std::runtime_error("DX12: RTV heap exhausted (increase EnsureRTVHeap() NumDescriptors).");
            }

            D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(idx) * rtvInc_;

            D3D12_RENDER_TARGET_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            viewDesc.Format = fmt;
            viewDesc.Texture2D.MipSlice = 0;
            viewDesc.Texture2D.PlaneSlice = 0;
            NativeDevice()->CreateRenderTargetView(res, &viewDesc, handle);
            return handle;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE AllocateRTVTexture2DArraySlice(ID3D12Resource* res, DXGI_FORMAT fmt, UINT arraySlice, UINT& outIndex)
        {
            EnsureRTVHeap();

            UINT idx = 0;
            if (!freeRTV_.empty())
            {
                idx = freeRTV_.back();
                freeRTV_.pop_back();
            }
            else
            {
                idx = nextRTV_++;
            }

            D3D12_RENDER_TARGET_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Format = fmt;
            viewDesc.Texture2DArray.MipSlice = 0;
            viewDesc.Texture2DArray.FirstArraySlice = arraySlice;
            viewDesc.Texture2DArray.ArraySize = 1;
            viewDesc.Texture2DArray.PlaneSlice = 0;

            const D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            {
                rtvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(idx) * static_cast<SIZE_T>(rtvInc_)
            };

            NativeDevice()->CreateRenderTargetView(res, &viewDesc, cpu);
            outIndex = idx;
            return cpu;
        }


        D3D12_CPU_DESCRIPTOR_HANDLE AllocateRTVTexture2DArray(ID3D12Resource* res, DXGI_FORMAT fmt, UINT firstSlice, UINT arraySize, UINT& outIndex)
        {
            EnsureRTVHeap();

            UINT idx = 0;
            if (!freeRTV_.empty())
            {
                idx = freeRTV_.back();
                freeRTV_.pop_back();
            }
            else
            {
                idx = nextRTV_++;
            }

            D3D12_RENDER_TARGET_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Format = fmt;
            viewDesc.Texture2DArray.MipSlice = 0;
            viewDesc.Texture2DArray.FirstArraySlice = firstSlice;
            viewDesc.Texture2DArray.ArraySize = arraySize;
            viewDesc.Texture2DArray.PlaneSlice = 0;

            const D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            {
                rtvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(idx) * static_cast<SIZE_T>(rtvInc_)
            };

            NativeDevice()->CreateRenderTargetView(res, &viewDesc, cpu);
            outIndex = idx;
            return cpu;
        }


        D3D12_CPU_DESCRIPTOR_HANDLE AllocateDSV(ID3D12Resource* res, DXGI_FORMAT fmt, UINT& outIndex)
        {
            UINT idx = 0;
            if (!freeDSV_.empty())
            {
                idx = freeDSV_.back();
                freeDSV_.pop_back();
            }
            else
            {
                idx = nextDSV_++;
            }
            outIndex = idx;

            if (idx >= 256u)
            {
                throw std::runtime_error("DX12: DSV heap exhausted (increase EnsureDSVHeap() NumDescriptors).");
            }

            D3D12_CPU_DESCRIPTOR_HANDLE handle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(idx) * dsvInc_;

            D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            viewDesc.Format = fmt;
            viewDesc.Flags = D3D12_DSV_FLAG_NONE;
            viewDesc.Texture2D.MipSlice = 0;
            NativeDevice()->CreateDepthStencilView(res, &viewDesc, handle);
            return handle;
        }



        D3D12_CPU_DESCRIPTOR_HANDLE AllocateDSVTexture2DArray(ID3D12Resource* res, DXGI_FORMAT fmt, UINT firstSlice, UINT arraySize, UINT& outIndex)
        {
            EnsureDSVHeap();

            UINT idx = 0;
            if (!freeDSV_.empty())
            {
                idx = freeDSV_.back();
                freeDSV_.pop_back();
            }
            else
            {
                idx = nextDSV_++;
            }
            outIndex = idx;

            if (idx >= 256u)
            {
                throw std::runtime_error("DX12: DSV heap exhausted (increase EnsureDSVHeap() NumDescriptors).");
            }

            D3D12_CPU_DESCRIPTOR_HANDLE handle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(idx) * dsvInc_;

            D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc{};
            viewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Format = fmt;
            viewDesc.Flags = D3D12_DSV_FLAG_NONE;
            viewDesc.Texture2DArray.MipSlice = 0;
            viewDesc.Texture2DArray.FirstArraySlice = firstSlice;
            viewDesc.Texture2DArray.ArraySize = arraySize;

            NativeDevice()->CreateDepthStencilView(res, &viewDesc, handle);
            return handle;
        }
        UINT AllocateSrvIndex()
        {
            UINT idx = 0;

            if (!freeSrv_.empty())
            {
                idx = freeSrv_.back();
                freeSrv_.pop_back();
            }
            else
            {
                idx = nextSrvIndex_++;
            }

            if (idx >= kSrvHeapNumDescriptors)
            {
                throw std::runtime_error("DX12: SRV heap exhausted (increase SRV heap NumDescriptors).");
            }

            return idx;
        }

        void AllocateStructuredBufferSRV(BufferEntry& entry)
        {
            if (entry.hasSRV)
            {
                return;
            }

            const auto stride = entry.desc.structuredStrideBytes;
            if (stride == 0)
            {
                throw std::runtime_error("DX12: StructuredBuffer SRV requested but structuredStrideBytes == 0");
            }

            const UINT64 totalBytes = static_cast<UINT64>(entry.desc.sizeInBytes);
            const UINT numElems = static_cast<UINT>(totalBytes / static_cast<UINT64>(stride));
            if (numElems == 0)
            {
                throw std::runtime_error("DX12: StructuredBuffer SRV requested but NumElements == 0");
            }

            const UINT idx = AllocateSrvIndex();

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(idx) * srvInc_;

            D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(idx) * static_cast<UINT64>(srvInc_);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN; // structured
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = numElems;
            srvDesc.Buffer.StructureByteStride = stride;
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

            NativeDevice()->CreateShaderResourceView(entry.resource.Get(), &srvDesc, cpu);

            entry.hasSRV = true;
            entry.srvIndex = idx;
            entry.srvCpu = cpu;
            entry.srvGpu = gpu;
        }

        void AllocateSRV(TextureEntry& entry, DXGI_FORMAT fmt, UINT mipLevels)
        {
            if (entry.hasSRV)
            {
                return;
            }

            const UINT idx = AllocateSrvIndex();

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(idx) * srvInc_;

            D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(idx) * static_cast<UINT64>(srvInc_);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = fmt;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            if (entry.type == TextureEntry::Type::Cube)
            {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = 0;
                srvDesc.TextureCube.MipLevels = mipLevels;
                srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
            }
            else
            {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = 0;
                srvDesc.Texture2D.MipLevels = mipLevels;
                srvDesc.Texture2D.PlaneSlice = 0;
                srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            }

            NativeDevice()->CreateShaderResourceView(entry.resource.Get(), &srvDesc, cpu);

            entry.hasSRV = true;
            entry.srvIndex = idx;
            entry.srvCpu = cpu;
            entry.srvGpu = gpu;
        }

        void AllocateSRV_CubeAsArray(TextureEntry& entry, DXGI_FORMAT fmt, UINT mipLevels)
        {
            if (entry.hasSRVArray)
                return;

            if (entry.type != TextureEntry::Type::Cube)
                throw std::runtime_error("DX12: AllocateSRV_CubeAsArray: texture is not a cube");

            const UINT idx = AllocateSrvIndex();

            D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(idx) * srvInc_;

            D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(idx) * static_cast<UINT64>(srvInc_);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = fmt;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.MipLevels = mipLevels;
            srvDesc.Texture2DArray.FirstArraySlice = 0;
            srvDesc.Texture2DArray.ArraySize = 6;
            srvDesc.Texture2DArray.PlaneSlice = 0;
            srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;

            NativeDevice()->CreateShaderResourceView(entry.resource.Get(), &srvDesc, cpu);

            entry.hasSRVArray = true;
            entry.srvIndexArray = idx;
            entry.srvCpuArray = cpu;
            entry.srvGpuArray = gpu;
        }

#if CORE_DX12_HAS_DXC
        using DxcCreateInstanceProc = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
#endif

        template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
        struct alignas(void*) PSOSubobject
        {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ Type };
            T data{};
        };

        static std::wstring ToWide_(std::string_view s)
        {
            if (s.empty())
            {
                return {};
            }

            int required = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            if (required <= 0)
            {
                required = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
                if (required <= 0)
                {
                    return {};
                }

                std::wstring w(static_cast<size_t>(required), L'\0');
                MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), required);
                return w;
            }

            std::wstring w(static_cast<size_t>(required), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), required);
            return w;
        }

        void DetectCapabilities_()
        {
            device2_.Reset();
            core_.device.As(&device2_);

            // D3D12 options3: View Instancing tier lives here.
            D3D12_FEATURE_DATA_D3D12_OPTIONS3 opt3{};
            if (SUCCEEDED(NativeDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &opt3, sizeof(opt3))))
            {
                supportsViewInstancing_ = (opt3.ViewInstancingTier != D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED);
            }
            else
            {
                supportsViewInstancing_ = false;
            }

            // Layered rendering (SV_RenderTargetArrayIndex / SV_ViewportArrayIndex) capability.
            // NOTE: Field name differs across Windows SDK versions.
            // Some SDKs expose:
            //   VPAndRTArrayIndexFromAnyShaderFeedingRasterizer
            // Others expose:
            //   VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation
            //
            // We only enable layered point-shadow if it's supported without relying on GS emulation.
            const auto ReadVPAndRTArrayIndexSupport = [](const D3D12_FEATURE_DATA_D3D12_OPTIONS& opt) -> bool
                {
                    if constexpr (requires { opt.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer; })
                    {
                        return opt.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer ? true : false;
                    }
                    else if constexpr (requires { opt.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation; })
                    {
                        return opt.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation ? true : false;
                    }
                    else
                    {
                        return false;
                    }
                };

            // Layered rendering (SV_RenderTargetArrayIndex / SV_ViewportArrayIndex) capability is exposed in
            // D3D12_FEATURE_D3D12_OPTIONS (NOT OPTIONS3). Some Windows SDK versions do not have this field in OPTIONS3.
            D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
            if (SUCCEEDED(NativeDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt))))
            {
                supportsVPAndRTArrayIndexFromAnyShader_ = ReadVPAndRTArrayIndexSupport(opt);
            }
            else
            {
                supportsVPAndRTArrayIndexFromAnyShader_ = false;
            }

            supportsViewInstancing_ = (device2_ != nullptr) && (viewInstancingTier_ != D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED);

            // Shader Model support.
            D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{};
            shaderModel.HighestShaderModel = D3D_SHADER_MODEL_6_6;
            if (FAILED(NativeDevice()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))))
            {
                shaderModel.HighestShaderModel = D3D_SHADER_MODEL_5_1;
            }
            highestShaderModel_ = shaderModel.HighestShaderModel;

#if CORE_DX12_HAS_DXC
            // We only claim SM6.1 support if both hardware and dxcompiler.dll are available.
            supportsSM6_1_ = (highestShaderModel_ >= D3D_SHADER_MODEL_6_1) && EnsureDXC_();
#else
            supportsSM6_1_ = false;
#endif
        }

#if CORE_DX12_HAS_DXC
        bool EnsureDXC_() noexcept
        {
            if (dxcInitTried_)
            {
                return (dxcCompiler_ != nullptr) && (dxcUtils_ != nullptr);
            }

            dxcInitTried_ = true;

            dxcModule_ = LoadLibraryA("dxcompiler.dll");
            if (!dxcModule_)
            {
                return false;
            }

            auto proc = GetProcAddress(dxcModule_, "DxcCreateInstance");
            if (!proc)
            {
                return false;
            }
            dxcCreateInstance_ = reinterpret_cast<DxcCreateInstanceProc>(proc);

            if (FAILED(dxcCreateInstance_(CLSID_DxcUtils, IID_PPV_ARGS(&dxcUtils_))))
            {
                dxcUtils_.Reset();
                return false;
            }

            if (FAILED(dxcCreateInstance_(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler_))))
            {
                dxcCompiler_.Reset();
                dxcUtils_.Reset();
                return false;
            }

            if (FAILED(dxcUtils_->CreateDefaultIncludeHandler(&dxcIncludeHandler_)))
            {
                dxcIncludeHandler_.Reset();
                dxcCompiler_.Reset();
                dxcUtils_.Reset();
                return false;
            }

            return true;
        }

        void ShutdownDXC_() noexcept
        {
            dxcIncludeHandler_.Reset();
            dxcCompiler_.Reset();
            dxcUtils_.Reset();
            dxcCreateInstance_ = nullptr;

            if (dxcModule_)
            {
                FreeLibrary(dxcModule_);
                dxcModule_ = nullptr;
            }

            dxcInitTried_ = false;
        }

        bool CompileDXC_(
            std::string_view source,
            const wchar_t* targetProfile,
            std::string_view entryPoint,
            [[maybe_unused]] std::string_view debugName,
            ComPtr<ID3DBlob>& outCode,
            std::string* outErrors) noexcept
        {
            outCode.Reset();

            if (!dxcUtils_ || !dxcCompiler_)
            {
                return false;
            }

            const std::wstring wEntry = ToWide_(entryPoint);

            std::vector<const wchar_t*> args;
            args.reserve(16);

            args.push_back(L"-E");
            args.push_back(wEntry.c_str());
            args.push_back(L"-T");
            args.push_back(targetProfile);

#if defined(_DEBUG)
            args.push_back(L"-Zi");
            args.push_back(L"-Od");
#else
            args.push_back(L"-O3");
#endif

            DxcBuffer buffer{};
            buffer.Ptr = source.data();
            buffer.Size = source.size();
            buffer.Encoding = DXC_CP_UTF8;

            ComPtr<IDxcResult> result;
            HRESULT hr = dxcCompiler_->Compile(
                &buffer,
                args.data(),
                static_cast<uint32_t>(args.size()),
                dxcIncludeHandler_.Get(),
                IID_PPV_ARGS(&result));

            if (FAILED(hr) || !result)
            {
                if (outErrors)
                {
                    *outErrors = "DXC: Compile() call failed";
                }
                return false;
            }

            HRESULT status = S_OK;
            result->GetStatus(&status);
            if (FAILED(status))
            {
                if (outErrors)
                {
                    ComPtr<IDxcBlobUtf8> errs;
                    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr)) && errs && errs->GetStringLength() > 0)
                    {
                        *outErrors = std::string(errs->GetStringPointer(), errs->GetStringLength());
                    }
                    else
                    {
                        *outErrors = "DXC: compilation failed";
                    }
                }
                return false;
            }

            ComPtr<IDxcBlob> obj;
            hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr);
            if (FAILED(hr) || !obj)
            {
                if (outErrors)
                {
                    *outErrors = "DXC: missing DXIL output";
                }
                return false;
            }

            ID3DBlob* blob = nullptr;
            hr = D3DCreateBlob(obj->GetBufferSize(), &blob);
            if (FAILED(hr) || !blob)
            {
                if (outErrors)
                {
                    *outErrors = "DXC: D3DCreateBlob failed";
                }
                return false;
            }

            std::memcpy(blob->GetBufferPointer(), obj->GetBufferPointer(), obj->GetBufferSize());
            outCode.Attach(blob);
            return true;
        }
#else
        void ShutdownDXC_() noexcept {}
#endif

    private:
        dx12::Core core_{};

        // Optional DX12 capabilities (SM6.1 / View Instancing).
        ComPtr<ID3D12Device2> device2_;
        D3D12_VIEW_INSTANCING_TIER viewInstancingTier_{ D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED };
        D3D_SHADER_MODEL highestShaderModel_{ D3D_SHADER_MODEL_5_1 };
        bool supportsViewInstancing_{ false };
        bool supportsVPAndRTArrayIndexFromAnyShader_{ false };
        bool supportsSM6_1_{ false };

#if CORE_DX12_HAS_DXC
        HMODULE dxcModule_{ nullptr };
        DxcCreateInstanceProc dxcCreateInstance_{ nullptr };
        bool dxcInitTried_{ false };
        ComPtr<IDxcUtils> dxcUtils_;
        ComPtr<IDxcCompiler3> dxcCompiler_;
        ComPtr<IDxcIncludeHandler> dxcIncludeHandler_;
#endif

        // Frame resources (allocator + per-frame constant upload ring)
        std::array<FrameResource, kFramesInFlight> frames_{};
        std::uint32_t activeFrameIndex_{ 0 };

        // Submission tracking (decoupled from any particular swapchain)
        std::uint64_t submitIndex_{ 0 };
        bool hasSubmitted_{ false };

        ComPtr<ID3D12GraphicsCommandList> cmdList_;

        ComPtr<ID3D12Fence> fence_;
        HANDLE fenceEvent_{ nullptr };
        UINT64 fenceValue_{ 0 };

        // Shared root signature
        ComPtr<ID3D12RootSignature> rootSig_;

        // SRV heap (shader visible)
        ComPtr<ID3D12DescriptorHeap> srvHeap_;
        UINT srvInc_{ 0 };

        #include "DirectX12RHI_ImGui_Private.inl"

        UINT nextSrvIndex_{ 1 };
        std::vector<UINT> freeSrv_;

        // RTV/DSV heaps for transient textures (swapchain has its own RTV/DSV)
        ComPtr<ID3D12DescriptorHeap> rtvHeap_;
        ComPtr<ID3D12DescriptorHeap> dsvHeap_;
        UINT rtvInc_{ 0 };
        UINT dsvInc_{ 0 };
        UINT nextRTV_{ 0 };
        UINT nextDSV_{ 0 };
        std::vector<UINT> freeRTV_;
        std::vector<UINT> freeDSV_;



        // Resource tables
        std::uint32_t nextBufId_{ 1 };
        std::uint32_t nextTexId_{ 1 };
        std::uint32_t nextShaderId_{ 1 };
        std::uint32_t nextPsoId_{ 1 };
        std::uint32_t nextLayoutId_{ 1 };
        std::uint32_t nextFBId_{ 1 };
        std::uint32_t nextDescId_{ 0 };
        std::uint32_t nextFenceId_{ 1 };

        std::unordered_map<std::uint32_t, BufferEntry> buffers_;
        std::unordered_map<std::uint32_t, TextureEntry> textures_;
        std::unordered_map<std::uint32_t, ShaderEntry> shaders_;
        std::unordered_map<std::uint32_t, PipelineEntry> pipelines_;
        std::unordered_map<std::uint32_t, InputLayoutEntry> layouts_;
        std::unordered_map<std::uint32_t, FramebufferEntry> framebuffers_;

        std::unordered_map<TextureDescIndex, TextureHandle> descToTex_;
        std::vector<TextureDescIndex> freeTexDesc_{};
        uint32_t nextTexDesc_ = 1;
        std::unordered_map<std::uint32_t, bool> fences_;


        std::vector<PendingBufferUpdate> pendingBufferUpdates_;

        std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> psoCache_;
    };
