import core;
import std;

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// ------------------------------------------------------------
// Win32 window (no GLFW)
// ------------------------------------------------------------
namespace
{
struct Win32Window
{
    HWND hwnd{};
    int width{};
    int height{};
    bool running{ true };
};

Win32Window* g_window = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_window) g_window->running = false;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

Win32Window CreateWindowWin32(int width, int height, const std::wstring& title)
{
    Win32Window w{};
    w.width = width;
    w.height = height;

    const HINSTANCE hInst = GetModuleHandleW(nullptr);
    const wchar_t* kClassName = L"CoreEngineModuleWindowClass";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;

    // If already registered, RegisterClassExW fails with ERROR_CLASS_ALREADY_EXISTS – that's fine.
    if (!RegisterClassExW(&wc))
    {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS)
        {
            throw std::runtime_error("RegisterClassExW failed");
        }
    }

    // Disable resizing for now (swapchain resize handling isn't wired here).
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    RECT rc{ 0, 0, width, height };
    AdjustWindowRect(&rc, style, FALSE);

    w.hwnd = CreateWindowExW(
        0,
        kClassName,
        title.c_str(),
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rc.right - rc.left,
        rc.bottom - rc.top,
        nullptr,
        nullptr,
        hInst,
        nullptr);

    if (!w.hwnd)
    {
        throw std::runtime_error("CreateWindowExW failed");
    }

    ShowWindow(w.hwnd, SW_SHOW);
    UpdateWindow(w.hwnd);
    return w;
}

void PumpMessages(Win32Window& w)
{
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            w.running = false;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void TinySleep()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

rhi::Backend ParseBackendFromArgs(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        if (a == "--null")
        {
            return rhi::Backend::Null;
        }
    }
    return rhi::Backend::DirectX12;
}

void CreateDeviceAndSwapChain(
    rhi::Backend backend,
    HWND hwnd,
    int initialW,
    int initialH,
    std::unique_ptr<rhi::IRHIDevice>& outDevice,
    std::unique_ptr<rhi::IRHISwapChain>& outSwapChain)
{
    if (backend == rhi::Backend::DirectX12)
    {
#if defined(CORE_USE_DX12)
        outDevice = rhi::CreateDX12Device();

        rhi::DX12SwapChainDesc sc{};
        sc.hwnd = hwnd;
        sc.bufferCount = 2;
        sc.base.extent = rhi::Extent2D{ static_cast<std::uint32_t>(initialW), static_cast<std::uint32_t>(initialH) };
        sc.base.backbufferFormat = rhi::Format::BGRA8_UNORM;
        sc.base.vsync = false;

        outSwapChain = rhi::CreateDX12SwapChain(*outDevice, sc);
#else
        outDevice = rhi::CreateNullDevice();
        rhi::SwapChainDesc sc{};
        sc.extent = rhi::Extent2D{ static_cast<std::uint32_t>(initialW), static_cast<std::uint32_t>(initialH) };
        outSwapChain = rhi::CreateNullSwapChain(*outDevice, sc);
#endif
        return;
    }

    // Null backend
    outDevice = rhi::CreateNullDevice();
    rhi::SwapChainDesc sc{};
    sc.extent = rhi::Extent2D{ static_cast<std::uint32_t>(initialW), static_cast<std::uint32_t>(initialH) };
    outSwapChain = rhi::CreateNullSwapChain(*outDevice, sc);
}

static std::unique_ptr<ITextureUploader> CreateTextureUploader(rhi::Backend backend, rhi::IRHIDevice& device)
{
    switch (backend)
    {
    case rhi::Backend::DirectX12:
#if defined(CORE_USE_DX12)
        return std::make_unique<rendern::DX12TextureUploader>(device);
#else
        return std::make_unique<rendern::NullTextureUploader>(device);
#endif

    default:
        return std::make_unique<rendern::NullTextureUploader>(device);
    }
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const rhi::Backend requestedBackend = ParseBackendFromArgs(argc, argv);

        const int W = 1280;
        const int H = 720;
        Win32Window wnd = CreateWindowWin32(W, H, L"CoreEngineModule (DX12)");

        g_window = &wnd;

        std::unique_ptr<rhi::IRHIDevice> device;
        std::unique_ptr<rhi::IRHISwapChain> swapChain;
        CreateDeviceAndSwapChain(requestedBackend, wnd.hwnd, W, H, device, swapChain);

        // Asset/Resource system: CPU decode on job system, GPU upload on render queue.
        StbTextureDecoder decoder{};
        rendern::JobSystemThreadPool jobs{ 1 };
        rendern::RenderQueueImmediate rq{};
        auto uploader = CreateTextureUploader(device->GetBackend(), *device);

        TextureIO texIO{ decoder, *uploader, jobs, rq };
        rendern::MeshIO meshIO{ *device, jobs, rq };

        AssetManager assets{ texIO, meshIO };
        ResourceManager& rm = assets.GetResourceManager();

        // Load brick texture
        TextureProperties brickProps{};
        brickProps.filePath = (std::filesystem::path("textures") / "brick.png").string();
        brickProps.generateMips = true;
        brickProps.srgb = true;
        (void)assets.LoadTextureAsync("brick", brickProps);

        // Renderer (facade) - Stage1 expects Scene
        rendern::RendererSettings rs{};
        rendern::Renderer renderer{ *device, rs };

        // Request meshes asynchronously (Scene stores handles; renderer skips pending resources).
        auto cubeMeshH = assets.LoadMeshAsync((std::filesystem::path("models") / "cube.obj").string());
        auto groundMeshH = assets.LoadMeshAsync((std::filesystem::path("models") / "quad.obj").string());

        // Scene setup
        rendern::Scene scene;
        scene.Clear();

        // Camera
        scene.camera.position = { 5.0f, 10.0f, 10.0f };
        scene.camera.target = { 0.0f, 0.0f, 0.0f };
        scene.camera.up = { 0.0f, 1.0f, 0.0f };
        scene.camera.fovYDeg = 60.0f;
        scene.camera.nearZ = 0.01f;
        scene.camera.farZ = 200.0f;

        // Lights: Dir + Point + Spot
        {
            rendern::Light l{};
            l.type = rendern::LightType::Directional;
            l.direction = mathUtils::Normalize(mathUtils::Vec3(-0.4f, -1.0f, -0.3f)); // FROM light
            l.color = { 1.0f, 0.2f, 1.0f };
            l.intensity = 0.2f;
            scene.AddLight(l);
        }
        {
            rendern::Light l{};
            l.type = rendern::LightType::Point;
            l.position = { -1.35f, 5.0f, -1.35f };
            l.color = { 1.0f, 1.0f, 1.0f };
            l.range = 120.0f;
            l.intensity = 1.0f;
            l.attConstant = 1.0f;
            l.attLinear = 0.02f;
            l.attQuadratic = 0.004f;
            //scene.AddLight(l);
        }
        {
            rendern::Light l{};
            l.type = rendern::LightType::Spot;
            l.position = { 2.0f, 4.0f, 2.0f };
            l.direction = mathUtils::Normalize(mathUtils::Vec3(-2.0f, -5.0f, 0.0f)); // FROM light
            l.color = { 1.0f, 0.2f, 0.2f };
            l.range = 100.0f;
            l.intensity = 5.0f;
            l.innerHalfAngleDeg = 22.0f;
            l.outerHalfAngleDeg = 35.0f;
            l.attLinear = 0.09f;
            l.attQuadratic = 0.032f;
            scene.AddLight(l);
        }

        // Materials
        rendern::Material groundMat{};
        groundMat.params.baseColor = { 0.8f, 0.8f, 0.8f, 1.0f };
        groundMat.params.albedoDescIndex = 0;
        groundMat.params.shininess = 16.0f;
        groundMat.params.specStrength = 0.5f;
        groundMat.params.shadowBias = 0.0f;
        groundMat.permFlags = rendern::MaterialPerm::UseShadow; // no tex

        rendern::Material cubeMat{};
        cubeMat.params.baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        cubeMat.params.albedoDescIndex = 0; // will be set once descriptor is allocated
        cubeMat.params.shininess = 64.0f;
        cubeMat.params.specStrength = 0.5f;
        cubeMat.params.shadowBias = 0.0015f;
        cubeMat.permFlags = rendern::MaterialPerm::UseShadow; // tex inferred automatically from descriptor

        rendern::MaterialHandle groundMatH = scene.CreateMaterial(groundMat);
        rendern::MaterialHandle cubeMatH = scene.CreateMaterial(cubeMat);

        // Ground
        {
            rendern::DrawItem ground{};
            ground.mesh = groundMeshH;
            ground.transform.position = { 0.0f, -0.6f, 0.0f };
            ground.transform.rotationDegrees = { -90.0f, 0.0f, 0.0f }; // quad XY -> XZ
            ground.transform.scale = { 8.0f, 8.0f, 8.0f };
            ground.material = groundMatH;
            scene.AddDraw(ground);
        }

        rhi::TextureDescIndex brickDesc = 0;

        // Add kDim*kDim cubes (same mesh + same material) -> should become 1 draw call in MainPass (DX12)
        {
            constexpr int kDim = 4;
            constexpr float kStep = 1.35f;

            for (int z = 0; z < kDim; ++z)
            {
                for (int x = 0; x < kDim; ++x)
                {
                    rendern::DrawItem cube{};
                    cube.mesh = cubeMeshH;

                    const float fx = (x - (kDim / 2)) * kStep;
                    const float fz = (z - (kDim / 2)) * kStep;

                    cube.transform.position = { fx, 2.3f, fz };
                    cube.transform.rotationDegrees = { 0.0f, 0.0f, 0.0f };
                    cube.transform.scale = { 1.0f, 1.0f, 1.0f };

                    cube.material = cubeMatH;
                    scene.AddDraw(cube);
                }
            }
        }

        // Animation timer
        using clock = std::chrono::steady_clock;
        auto last = clock::now();
        float t = 0.0f;

        while (wnd.running)
        {
            PumpMessages(wnd);
            if (!wnd.running) break;

            // Drive uploads/destruction
            assets.ProcessUploads(8, 32, 2, 32);

            // Get brick GPU texture from RM
            rhi::TextureHandle brick{};
            if (auto tex = rm.Get<TextureResource>("brick"))
            {
                const auto& gpu = tex->GetResource();
                if (gpu.id != 0)
                    brick = rhi::TextureHandle{ static_cast<std::uint32_t>(gpu.id) };
            }

            // Allocate descriptor once
            if (brick && brickDesc == 0)
                brickDesc = device->AllocateTextureDesctiptor(brick);

            // Update cube material once descriptor is ready
            if (brickDesc != 0)
            {
                scene.GetMaterial(cubeMatH).params.albedoDescIndex = brickDesc;
            }

            // Animate cube rotation
            const auto now = clock::now();
            std::chrono::duration<float> dt = now - last;
            last = now;

            const float delta = std::min(dt.count(), 0.05f);
            t += delta;

            if (scene.drawItems.size() >= 2)
            {
                for (std::size_t i = 1; i < scene.drawItems.size(); ++i)
                {
                    scene.drawItems[i].transform.rotationDegrees.y = t * 45.0f + static_cast<float>(i) * 3.6f;
                }
            }

            // Keep spot light attached to camera (lights[2])
            if (scene.lights.size() >= 3 && scene.lights[2].type == rendern::LightType::Spot)
            {
                scene.lights[2].position = scene.camera.position;
                scene.lights[2].direction = mathUtils::Normalize(mathUtils::Sub(scene.camera.target, scene.camera.position));
            }

            renderer.RenderFrame(*swapChain, scene);

            TinySleep();
        }

        renderer.Shutdown();

        if (brickDesc != 0)
            device->FreeTextureDescriptor(brickDesc);

        // Cleanup resources (destroy queues are driven by ProcessUploads).
        jobs.WaitIdle();
        assets.ClearAll();
        assets.ProcessUploads(64, 256, 64, 256);

        if (wnd.hwnd)
        {
            DestroyWindow(wnd.hwnd);
            wnd.hwnd = nullptr;
        }
        g_window = nullptr;

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 2;
    }
}
