// src/Render/DirectX12/RenderDX12.ixx
export module core:render_dx12;

export import :render;

// DX12 platform core (device/queue/factory)
export import :dx12_core;
// DX12 RHI (device + swapchain + command execution)
export import :rhi_dx12;
// DX12 texture uploader (CPU mipmaps -> upload -> SRV)
export import :render_core_dx12;
