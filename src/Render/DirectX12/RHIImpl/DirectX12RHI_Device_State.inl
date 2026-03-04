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
std::unordered_map<std::uint32_t, bool> fences_;


std::vector<PendingBufferUpdate> pendingBufferUpdates_;

std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> psoCache_;