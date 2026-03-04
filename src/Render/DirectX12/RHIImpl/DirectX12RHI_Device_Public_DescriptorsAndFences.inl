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
                // 0 = null
                return;
            }
            if (idx >= static_cast<TextureDescIndex>(kSrvHeapNumDescriptors))
            {
                throw std::runtime_error("DX12: UpdateTextureDescriptor: index out of SRV heap range");
            }

            // Keep mapping for transitions / debug.
            descToTex_[idx] = tex;

            D3D12_CPU_DESCRIPTOR_HANDLE dst = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            dst.ptr += static_cast<SIZE_T>(idx) * srvInc_;

            // If tex is null -> write null texture SRV (copy from slot0)
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
            if (!te.hasSRV || te.srvCpu.ptr == 0)
            {
                throw std::runtime_error("DX12: UpdateTextureDescriptor: texture has no SRV");
            }

            if (dst.ptr != te.srvCpu.ptr)
            {
                NativeDevice()->CopyDescriptorsSimple(
                    1,
                    dst,
                    te.srvCpu,
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }

        void FreeTextureDescriptor(TextureDescIndex index) noexcept override
        {
            // Reset mapping; slot is recycled deferred-safe via deferredFreeSrv.
            descToTex_.erase(index);

            if (index != 0 && index < static_cast<TextureDescIndex>(kSrvHeapNumDescriptors))
            {
                // Overwrite with null tex SRV (copy from slot0)
                D3D12_CPU_DESCRIPTOR_HANDLE dst = srvHeap_->GetCPUDescriptorHandleForHeapStart();
                dst.ptr += static_cast<SIZE_T>(index) * srvInc_;
                D3D12_CPU_DESCRIPTOR_HANDLE src = srvHeap_->GetCPUDescriptorHandleForHeapStart(); // slot0
                // ^ if you don't have this helper, use srvHeap_->GetCPUDescriptorHandleForHeapStart() like above.
                src = srvHeap_->GetCPUDescriptorHandleForHeapStart();
                NativeDevice()->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

                // Recycle after fence for safety (same mechanism as textures/buffers).
                CurrentFrame().deferredFreeSrv.push_back(static_cast<UINT>(index));
            }
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
