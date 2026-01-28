module;

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <d3d12.h>
  #include <dxgi1_6.h>
  #include <wrl.h>
  #include <stdexcept>
  #include <string>

  #include "d3dx12.h"
#endif

export module core:dx12_core;

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

export void ThrowIfFailed(HRESULT hr, const char* msg)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(msg);
    }
}

#endif

export namespace dx12
{
#if defined(_WIN32)
    struct Core
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> cmdQueue;

        void Init()
        {
            ComPtr<IDXGIFactory6> factory;
            ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DX12: CreateDXGIFactory2 failed");

            // Pick first hardware adapter.
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                {
                    adapter.Reset();
                    continue;
                }
                break;
            }
            if (!adapter)
            {
                ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "DX12: EnumWarpAdapter failed");
            }

            ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
                          "DX12: D3D12CreateDevice failed");

            D3D12_COMMAND_QUEUE_DESC d12queue{};
            d12queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            d12queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            d12queue.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

            ThrowIfFailed(device->CreateCommandQueue(&d12queue, IID_PPV_ARGS(&cmdQueue)), "DX12: CreateCommandQueue failed");
        }
    };
#else
    struct Core
    {
        void Init() {}
    };
#endif
}
