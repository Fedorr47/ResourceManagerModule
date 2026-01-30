import core;
import std;

#if defined(_WIN32)
// Prevent Windows headers from defining the `min`/`max` macros which break `std::min`/`std::max`.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#if defined(CORE_USE_GL)
#include <GL/glew.h>
#endif

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

static void GLFWErrorCallback(int error, const char* desc)
{
    std::cerr << "GLFW error " << error << ": " << (desc ? desc : "") << "\n";
}

static void TinySleep()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

static const char* BackendName(rhi::Backend b)
{
    switch (b)
    {
    case rhi::Backend::OpenGL:   return "OpenGL";
    case rhi::Backend::DirectX12:return "DX12";
    default:                    return "Null";
    }
}

static rhi::Backend DefaultBackend()
{
#if defined(CORE_USE_DX12) && defined(CORE_USE_GL)
#if defined(_WIN32)
    return rhi::Backend::DirectX12;
#else
    return rhi::Backend::OpenGL;
#endif
#elif defined(CORE_USE_DX12)
    return rhi::Backend::DirectX12;
#elif defined(CORE_USE_GL)
    return rhi::Backend::OpenGL;
#else
    return rhi::Backend::Null;
#endif
}

static rhi::Backend ParseBackendFromArgs(int argc, char** argv)
{
    rhi::Backend b = DefaultBackend();

#if defined(CORE_USE_DX12) && defined(CORE_USE_GL)
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i] ? std::string_view(argv[i]) : std::string_view{};
        if (a == "--dx12" || a == "-dx12") b = rhi::Backend::DirectX12;
        else if (a == "--gl" || a == "-gl") b = rhi::Backend::OpenGL;
        else if (a == "--null" || a == "-null") b = rhi::Backend::Null;
    }
#else
    (void)argc;
    (void)argv;
#endif

    return b;
}

static void ConfigureWindowHintsForBackend(rhi::Backend backend)
{
    if (backend == rhi::Backend::OpenGL)
    {
#if defined(CORE_USE_GL)
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#else
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#endif
    }
    else
    {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
}

static void InitOpenGLForWindow(GLFWwindow* wnd)
{
#if defined(CORE_USE_GL)
    glfwMakeContextCurrent(wnd);

    // Respect vsync default (can be overridden by swapchain hook later).
    glfwSwapInterval(1);

    // GLEW needs a current context.
    glewExperimental = GL_TRUE;
    const GLenum err = glewInit();
    if (err != GLEW_OK)
    {
        const auto* msg = reinterpret_cast<const char*>(glewGetErrorString(err));
        throw std::runtime_error(std::string("glewInit failed: ") + (msg ? msg : "<unknown>"));
    }

    // GLEW may emit a spurious GL error on init; clear it.
    (void)glGetError();
#else
    (void)wnd;
#endif
}

static void CreateDeviceAndSwapChain(
    rhi::Backend backend,
    GLFWwindow* wnd,
    int initialW,
    int initialH,
    std::unique_ptr<rhi::IRHIDevice>& outDevice,
    std::unique_ptr<rhi::IRHISwapChain>& outSwapChain)
{
    switch (backend)
    {
    case rhi::Backend::DirectX12:
    {
#if defined(CORE_USE_DX12)
        outDevice = rhi::CreateDX12Device();

#if defined(_WIN32)
        const HWND hwnd = glfwGetWin32Window(wnd);
#else
        const void* hwnd = nullptr;
#endif

        rhi::DX12SwapChainDesc sc{};
        sc.hwnd = (HWND)hwnd;
        sc.bufferCount = 2;
        sc.base.extent = rhi::Extent2D{ (std::uint32_t)initialW, (std::uint32_t)initialH };
        sc.base.backbufferFormat = rhi::Format::BGRA8_UNORM;
        sc.base.vsync = true;

        outSwapChain = rhi::CreateDX12SwapChain(*outDevice, sc);
#else
        outDevice = rhi::CreateNullDevice();
        rhi::SwapChainDesc sc{};
        sc.extent = rhi::Extent2D{ (std::uint32_t)initialW, (std::uint32_t)initialH };
        outSwapChain = rhi::CreateNullSwapChain(*outDevice, sc);
#endif
        break;
    }

    case rhi::Backend::OpenGL:
    {
#if defined(CORE_USE_GL)
        outDevice = rhi::CreateGLDevice();

        rhi::GLSwapChainDesc sc{};
        sc.base.extent = rhi::Extent2D{ (std::uint32_t)initialW, (std::uint32_t)initialH };
        sc.base.backbufferFormat = rhi::Format::BGRA8_UNORM;
        sc.base.vsync = true;

        sc.hooks.present = [wnd]() { glfwSwapBuffers(wnd); };
        sc.hooks.getDrawableExtent = [wnd]()
            {
                int w = 0;
                int h = 0;
                glfwGetFramebufferSize(wnd, &w, &h);
                return rhi::Extent2D{
                    (std::uint32_t)std::max(0, w),
                    (std::uint32_t)std::max(0, h)
                };
            };
        sc.hooks.setVsync = [](bool on) { glfwSwapInterval(on ? 1 : 0); };

        outSwapChain = rhi::CreateGLSwapChain(*outDevice, sc);
#else
        outDevice = rhi::CreateNullDevice();
        rhi::SwapChainDesc sc{};
        sc.extent = rhi::Extent2D{ (std::uint32_t)initialW, (std::uint32_t)initialH };
        outSwapChain = rhi::CreateNullSwapChain(*outDevice, sc);
#endif
        break;
    }

    default:
    {
        outDevice = rhi::CreateNullDevice();
        rhi::SwapChainDesc sc{};
        sc.extent = rhi::Extent2D{ (std::uint32_t)initialW, (std::uint32_t)initialH };
        outSwapChain = rhi::CreateNullSwapChain(*outDevice, sc);
        break;
    }
    }
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

    case rhi::Backend::OpenGL:
#if defined(CORE_USE_GL)
        return std::make_unique<rendern::GLTextureUploader>(device);
#else
        return std::make_unique<rendern::NullTextureUploader>(device);
#endif

    default:
        return std::make_unique<rendern::NullTextureUploader>(device);
    }
}

int main(int argc, char** argv)
{
    glfwSetErrorCallback(GLFWErrorCallback);

    if (!glfwInit())
    {
        std::cerr << "glfwInit failed\n";
        return 1;
    }

    const int W = 1280;
    const int H = 720;

    const rhi::Backend requestedBackend = ParseBackendFromArgs(argc, argv);

    ConfigureWindowHintsForBackend(requestedBackend);

    const std::string title = std::string("CoreEngine (") + BackendName(requestedBackend) + ")";
    GLFWwindow* wnd = glfwCreateWindow(W, H, title.c_str(), nullptr, nullptr);
    if (!wnd)
    {
        std::cerr << "glfwCreateWindow failed\n";
        glfwTerminate();
        return 1;
    }

    try
    {
        if (requestedBackend == rhi::Backend::OpenGL)
        {
            InitOpenGLForWindow(wnd);
        }

        std::unique_ptr<rhi::IRHIDevice> device;
        std::unique_ptr<rhi::IRHISwapChain> swapChain;
        CreateDeviceAndSwapChain(requestedBackend, wnd, W, H, device, swapChain);

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
        scene.camera.position = { 2.2f, 10.6f, 10.2f };
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
            l.color = { 1.0f, 1.0f, 1.0f };
            l.intensity = 0.5f;
            scene.AddLight(l);
        }
        {
            rendern::Light l{};
            l.type = rendern::LightType::Point;
            l.position = { 2.5f, 2.0f, 1.5f };
            l.color = { 1.0f, 0.95f, 0.8f };
            l.range = 12.0f;
            l.intensity = 0.0f;
            l.attConstant = 1.0f;
            l.attLinear = 0.12f;
            l.attQuadratic = 0.04f;
            scene.AddLight(l);
        }
        {
            rendern::Light l{};
            l.type = rendern::LightType::Spot;
            l.position = scene.camera.position;
            l.direction = mathUtils::Normalize(mathUtils::Sub(scene.camera.target, scene.camera.position)); // FROM light
            l.color = { 0.2f, 0.1f, 1.0f };
            l.range = 30.0f;
            l.intensity = 3.0f;
            l.innerAngleDeg = 12.0f;
            l.outerAngleDeg = 20.0f;
            l.attLinear = 0.09f;
            l.attQuadratic = 0.032f;
            scene.AddLight(l);
        }

        // Draw items: ground + cube
		// Materials (Stage 3)
		rendern::Material groundMat{};
		groundMat.params.baseColor = { 0.8f, 0.8f, 0.8f, 1.0f };
		groundMat.params.albedoDescIndex = 0;
		groundMat.params.shininess = 16.0f;
		groundMat.params.specStrength = 0.05f;
		groundMat.params.shadowBias = 0.0015f;
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
            constexpr int kDim = 10;
            constexpr float kStep = 1.35f;

            for (int z = 0; z < kDim; ++z)
            {
                for (int x = 0; x < kDim; ++x)
                {
                    rendern::DrawItem cube{};
					cube.mesh = cubeMeshH;

					const float fx = (x - (kDim / 2)) * kStep;
					const float fz = (z - (kDim / 2)) * kStep;

					cube.transform.position = { fx, 0.6f, fz };
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

        while (!glfwWindowShouldClose(wnd))
        {
            glfwPollEvents();

            // Drive uploads/destruction
            assets.ProcessUploads(8, 32, 2, 32);

            // Get brick GPU texture from RM
            rhi::TextureHandle brick{};
            if (auto tex = rm.Get<TextureResource>("brick"))
            {
                const auto& gpu = tex->GetResource();
                if (gpu.id != 0)
                    brick = rhi::TextureHandle{ (std::uint32_t)gpu.id };
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

            // Stage1 render
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

        glfwDestroyWindow(wnd);
        glfwTerminate();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal: " << e.what() << "\n";
        glfwDestroyWindow(wnd);
        glfwTerminate();
        return 2;
    }
}
