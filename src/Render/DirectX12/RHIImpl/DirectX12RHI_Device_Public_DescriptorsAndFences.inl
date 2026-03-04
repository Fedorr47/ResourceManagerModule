        TextureDescIndex AllocateTextureDesctiptor(TextureHandle texture) override
        {
            // TextureDescIndex is now a REAL SRV heap slot index.
            // We allocate a slot in the shader-visible heap and write SRV there.
            // 0 is reserved for "null texture".

            const UINT slot = AllocateSrvIndex(); // uses deferred-safe recycling
            const TextureDescIndex idx = static_cast<TextureDescIndex>(slot);
            UpdateTextureDescriptor(idx, texture);
            return idx;
        }

        void UpdateTextureDescriptor(TextureDescIndex idx, TextureHandle tex) override
        {
            if (idx == 0)
            {
                // 0 is always null.
                return;
            }
            if (static_cast<UINT>(idx) >= kSrvHeapNumDescriptors)
            {
                throw std::runtime_error("DX12: UpdateTextureDescriptor: index out of SRV heap range");
            }

            // Keep mapping for transitions / validation.
            descToTex_[idx] = tex;

            D3D12_CPU_DESCRIPTOR_HANDLE dst = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            dst.ptr += static_cast<SIZE_T>(idx) * srvInc_;

            // Null texture -> copy null Texture2D SRV from slot 0.
            if (!tex)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE src = srvHeap_->GetCPUDescriptorHandleForHeapStart(); // slot 0
                NativeDevice()->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                return;
            }

            auto it = textures_.find(tex.id);
            if (it == textures_.end())
            {
                throw std::runtime_error("DX12: UpdateTextureDescriptor: texture not found");
            }

            auto& te = it->second;
            if (!te.resource)
            {
                throw std::runtime_error("DX12: UpdateTextureDescriptor: texture has no resource");
            }
            if (te.srvFormat == DXGI_FORMAT_UNKNOWN)
            {
                throw std::runtime_error("DX12: UpdateTextureDescriptor: texture has no SRV format");
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = te.srvFormat;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

            if (te.type == TextureEntry::Type::Cube)
            {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = 0;
                srvDesc.TextureCube.MipLevels = 1;
                srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
            }
            else
            {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = 0;
                srvDesc.Texture2D.MipLevels = 1;
                srvDesc.Texture2D.PlaneSlice = 0;
                srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            }

            NativeDevice()->CreateShaderResourceView(te.resource.Get(), &srvDesc, dst);
        }

        void FreeTextureDescriptor(TextureDescIndex index) noexcept override
        {
            descToTex_.erase(index);

            if (index == 0 || static_cast<UINT>(index) >= kSrvHeapNumDescriptors)
            {
                return;
            }

            // Overwrite freed slot with null Texture2D SRV (slot 0).
            D3D12_CPU_DESCRIPTOR_HANDLE dst = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            dst.ptr += static_cast<SIZE_T>(index) * srvInc_;

            D3D12_CPU_DESCRIPTOR_HANDLE src = srvHeap_->GetCPUDescriptorHandleForHeapStart(); // slot 0
            NativeDevice()->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            // Recycle after GPU fence for safety.
            CurrentFrame().deferredFreeSrv.push_back(static_cast<UINT>(index));
        }

        // ---------------- Fences (minimal impl) ----------------
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
