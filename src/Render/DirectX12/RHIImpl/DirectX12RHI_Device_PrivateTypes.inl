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
            static constexpr std::uint32_t kMaxColorAttachments = 8u;
            std::array<TextureHandle, kMaxColorAttachments> colors{};
            std::uint32_t colorCount{ 0u };
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
