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
                std::string msg = "DX12: SM6.1 shader requested, but SM6.1/DXC is unavailable (shader='";
                msg += std::string(debugName);
                msg += "')";
                throw std::runtime_error(msg);
            }

            const wchar_t* target = (stage == ShaderStage::Vertex) ? L"vs_6_1" : L"ps_6_1";

            std::string lastErr{};
            auto TryCompile = [&](std::string_view entry) -> ComPtr<ID3DBlob>
                {
                    ComPtr<ID3DBlob> out;
                    std::string err;
                    if (!CompileDXC_(sourceOrBytecode, target, entry, debugName, out, &err))
                    {
                        if (!err.empty())
                        {
                            lastErr = std::move(err);
                        }
                        return {};
                    }
                    lastErr.clear();
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
                std::string msg = "DX12: SM6.1 shader compile failed (shader='";
                msg += std::string(debugName);
                msg += "')";
                if (!lastErr.empty())
                {
                    msg += ": ";
                    msg += lastErr;
                }
                throw std::runtime_error(msg);
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
            std::string msg = "DX12: SM6.1 shader requested, but this build has CORE_DX12_HAS_DXC=0 (shader='";
            msg += std::string(debugName);
            msg += "')";
            throw std::runtime_error(msg);
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

            if (!vertexShader)
            {
                std::string msg = "DX12: CreatePipelineEx: vertex shader is null (pipeline='";
                msg += std::string(debugName);
                msg += "')";
                throw std::runtime_error(msg);
            }
            if (shaders_.find(vertexShader.id) == shaders_.end())
            {
                std::string msg = "DX12: CreatePipelineEx: vertex shader handle not found (pipeline='";
                msg += std::string(debugName);
                msg += "', vs=";
                msg += std::to_string(vertexShader.id);
                msg += ")";
                throw std::runtime_error(msg);
            }
            if (pixelShader && shaders_.find(pixelShader.id) == shaders_.end())
            {
                std::string msg = "DX12: CreatePipelineEx: pixel shader handle not found (pipeline='";
                msg += std::string(debugName);
                msg += "', ps=";
                msg += std::to_string(pixelShader.id);
                msg += ")";
                throw std::runtime_error(msg);
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

