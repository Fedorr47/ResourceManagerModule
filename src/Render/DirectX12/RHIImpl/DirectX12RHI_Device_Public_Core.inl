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

                const D3D12_RESOURCE_DESC resourceDesc = newRes->GetDesc();
                it->second.extent = Extent2D{
                    static_cast<std::uint32_t>(resourceDesc.Width),
                    static_cast<std::uint32_t>(resourceDesc.Height) };
                it->second.resourceFormat = resourceDesc.Format;
                it->second.srvFormat = fmt;
                it->second.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

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

            const D3D12_RESOURCE_DESC resourceDesc = res->GetDesc();
            textureEntry.extent = Extent2D{
                static_cast<std::uint32_t>(resourceDesc.Width),
                static_cast<std::uint32_t>(resourceDesc.Height) };
            textureEntry.format = rhi::Format::RGBA8_UNORM;

            textureEntry.resourceFormat = resourceDesc.Format;
            textureEntry.srvFormat = fmt;
            textureEntry.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

            textureEntry.resource = res;

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

            textureEntry.resourceFormat = resourceDesc.Format;
            textureEntry.srvFormat = fmt;
            textureEntry.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

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
