auto PackBits = [](std::uint64_t& packedValue, std::uint32_t& bitOffset, std::uint64_t value, std::uint32_t width)
    {
        const std::uint64_t mask = (width >= 64u) ? ~0ull : ((1ull << width) - 1ull);
        packedValue |= (value & mask) << bitOffset;
        bitOffset += width;
    };

auto PackStencilFaceState = [&](std::uint64_t& packedValue, std::uint32_t& bitOffset, const StencilFaceState& face)
    {
        PackBits(packedValue, bitOffset, static_cast<std::uint32_t>(face.failOp), 3);
        PackBits(packedValue, bitOffset, static_cast<std::uint32_t>(face.depthFailOp), 3);
        PackBits(packedValue, bitOffset, static_cast<std::uint32_t>(face.passOp), 3);
        PackBits(packedValue, bitOffset, static_cast<std::uint32_t>(face.compareOp), 3);
    };

auto PackGraphicsStateKey = [&](const GraphicsState& state) -> std::uint64_t
    {
        std::uint64_t packedValue = 0;
        std::uint32_t bitOffset = 0;

        PackBits(packedValue, bitOffset, static_cast<std::uint32_t>(state.rasterizer.cullMode), 2);
        PackBits(packedValue, bitOffset, static_cast<std::uint32_t>(state.rasterizer.frontFace), 1);
        PackBits(packedValue, bitOffset, state.depth.testEnable ? 1u : 0u, 1);
        PackBits(packedValue, bitOffset, state.depth.writeEnable ? 1u : 0u, 1);
        PackBits(packedValue, bitOffset, static_cast<std::uint32_t>(state.depth.depthCompareOp), 3);
        PackBits(packedValue, bitOffset, state.blend.enable ? 1u : 0u, 1);
        PackBits(packedValue, bitOffset, static_cast<std::uint32_t>(state.blend.mode), 2);
        PackBits(packedValue, bitOffset, state.depth.stencil.enable ? 1u : 0u, 1);
        PackBits(packedValue, bitOffset, static_cast<std::uint32_t>(state.depth.stencil.readMask), 8);
        PackBits(packedValue, bitOffset, static_cast<std::uint32_t>(state.depth.stencil.writeMask), 8);
        PackStencilFaceState(packedValue, bitOffset, state.depth.stencil.front);
        PackStencilFaceState(packedValue, bitOffset, state.depth.stencil.back);
        return packedValue;
    };

auto HashPsoKeyPart = [](std::uint64_t hash, std::uint64_t value) -> std::uint64_t
    {
        constexpr std::uint64_t kPrime = 1099511628211ull;
        for (int i = 0; i < 8; ++i)
        {
            const std::uint8_t byte = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu);
            hash ^= byte;
            hash *= kPrime;
        }
        return hash;
    };

auto BuildMissingShaderMessage = [](const auto& pipelineRecord, PipelineHandle pipelineHandle, UINT numRenderTargets, const char* shaderStage) -> std::string
    {
        std::string msg = "DX12: shader handle not found (";
        msg += shaderStage;
        msg += ", pipeline='";
        msg += pipelineRecord.debugName;
        msg += "', pipe=";
        msg += std::to_string(pipelineHandle.id);
        msg += ", vs=";
        msg += std::to_string(pipelineRecord.vs.id);
        msg += ", ps=";
        msg += std::to_string(pipelineRecord.ps.id);
        msg += ", numRT=";
        msg += std::to_string(numRenderTargets);
        msg += ")";
        return msg;
    };