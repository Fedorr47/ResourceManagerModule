        void EnsureRTVHeap()
        {
            if (rtvHeap_)
            {
                return;
            }
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = 1024;
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
            heapDesc.NumDescriptors = 1024;
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

            if (idx >= 1024u)
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

            if (idx >= 1024u)
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

            if (idx >= 1024u)
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
