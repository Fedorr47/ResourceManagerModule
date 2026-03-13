export module core;

export import :resource_manager;
export import :asset_manager;
export import :input;
export import :input_core;
export import :controller_base;
export import :win32_input;
export import :gametimer;
export import :render;
export import :mesh;
export import :skeleton;
export import :animation_clip;
export import :animator;
export import :animation_controller;
export import :skinned_mesh;
export import :obj_loader;
export import :math_utils;
export import :geometry;
export import :EnTTHelpers;
export import :gameplay;
export import :gameplay_runtime;

#if defined(CORE_USE_DX12)
export import :render_dx12;
export import :imgui_debug_ui;
#else
export import :render_gl;
#endif