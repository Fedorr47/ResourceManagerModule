        void CreateRootSignature()
        {
            // Root signature layout:
            //  [0]  CBV(b0)   - per-draw constants
            //  [1+] SRV(t0+)  - individual SRV descriptor tables (1 descriptor each)
            // [LAST] SRV(t0, space1) - bindless SRV table for SM6 deferred/bindless materials
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
            // //  t18 env cube alias as Texture2DArray<float4> (same resource, 6 slices)
            //
            // Samplers:

            //  s0 - linear wrap
            //  s1 - comparison sampler for shadow maps (clamp)
            //  s2 - point clamp (used by point shadows)
            //  s3 - linear clamp (used by skybox/env cubemaps)
            
            //
            // NOTE: we move to RS 1.1 so we can define an "unbounded-style" bindless range in space1.
            
            std::array<D3D12_DESCRIPTOR_RANGE1, kMaxSRVSlots> ranges{};
            for (UINT i = 0; i < kMaxSRVSlots; ++i)
            {
                ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                ranges[i].NumDescriptors = 1;
                ranges[i].BaseShaderRegister = i; // ti
                ranges[i].RegisterSpace = 0;
                ranges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
                ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            }
            
            D3D12_DESCRIPTOR_RANGE1 bindlessRange{};
            bindlessRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            bindlessRange.NumDescriptors = UINT_MAX;     // unbounded-style
            bindlessRange.BaseShaderRegister = 0;        // t0
            bindlessRange.RegisterSpace = 1;             // space1
            bindlessRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
            bindlessRange.OffsetInDescriptorsFromTableStart = 0;
            
            std::array<D3D12_ROOT_PARAMETER1, 1 + kMaxSRVSlots + 1> rootParams{};

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

            // bindless SRV table: t0.. (space1)
            {
                const UINT bindlessParam = 1 + kMaxSRVSlots;
                rootParams[bindlessParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rootParams[bindlessParam].DescriptorTable.NumDescriptorRanges = 1;
                rootParams[bindlessParam].DescriptorTable.pDescriptorRanges = &bindlessRange;
                rootParams[bindlessParam].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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

            D3D12_ROOT_SIGNATURE_DESC1 rootSigDesc1{};
            rootSigDesc1.NumParameters = static_cast<UINT>(rootParams.size());
            rootSigDesc1.pParameters = rootParams.data();
            rootSigDesc1.NumStaticSamplers = 4;
            rootSigDesc1.pStaticSamplers = samplers;
            rootSigDesc1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

            D3D12_VERSIONED_ROOT_SIGNATURE_DESC ver{};
            ver.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            ver.Desc_1_1 = rootSigDesc1;

            ComPtr<ID3DBlob> serialized;
            ComPtr<ID3DBlob> error;

            HRESULT hr = D3D12SerializeVersionedRootSignature(
                &ver,
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
