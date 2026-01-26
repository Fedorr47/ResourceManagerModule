#if defined(CORE_USE_DX12)
#include <GLFW/glfw3.h>
#else
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#endif

import core;
import std;

#if !defined(CORE_USE_DX12)
static rhi::Extent2D GetDrawableExtent(GLFWwindow* wnd)
{
    int width = 0, height = 0;
    glfwGetFramebufferSize(wnd, &width, &height);
    return rhi::Extent2D{ static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) };
}
#endif

int main()
{
    if (!glfwInit())
    {
        std::cerr << "GLFW init failed\n";
        return 1;
    }

#if defined(CORE_USE_DX12)
    // DX12 path (skeleton): create a window without OpenGL context and just init the DX12 device.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow* wnd = glfwCreateWindow(1280, 720, "CoreEngineModule DX12 (skeleton)", nullptr, nullptr);
    if (!wnd)
    {
        std::cerr << "Failed to create window\n";
        glfwTerminate();
        return 1;
    }

    auto device = rhi::CreateDX12Device();
    std::cout << "Device: " << device->GetName() << "\n";

    while (!glfwWindowShouldClose(wnd))
    {
        glfwPollEvents();
    }

    glfwDestroyWindow(wnd);
    glfwTerminate();
    return 0;
#else
    // OpenGL context
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // IMPORTANT: request depth buffer for default framebuffer
    glfwWindowHint(GLFW_DEPTH_BITS, 24);

    GLFWwindow* wnd = glfwCreateWindow(1280, 720, "CoreEngineModule v0.4", nullptr, nullptr);
    if (!wnd)
    {
        std::cerr << "Failed to create window\n";
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(wnd);
    glfwSwapInterval(1);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK)
    {
        std::cerr << "GLEW init failed\n";
        glfwDestroyWindow(wnd);
        glfwTerminate();
        return 1;
    }

    // ---- Create RHI device + swapchain ----
    auto device = rhi::CreateGLDevice(rhi::GLDeviceDesc{ .enableDebug = false });

    rhi::GLSwapChainDesc swapChain{};
    swapChain.base.extent = GetDrawableExtent(wnd);
    swapChain.base.vsync = true;
    swapChain.base.backbufferFormat = rhi::Format::BGRA8_UNORM;
    swapChain.hooks.present = [wnd] { glfwSwapBuffers(wnd); };
    swapChain.hooks.getDrawableExtent = [wnd] { return GetDrawableExtent(wnd); };
    swapChain.hooks.setVsync = [](bool vsync) { glfwSwapInterval(vsync ? 1 : 0); };

    auto swapchain = rhi::CreateGLSwapChain(*device, std::move(swapChain));

    std::cout << "Device: " << device->GetName() << "\n";

    StbTextureDecoder decoder;
    rendern::JobSystemImmediate jobs;
    rendern::RenderQueueImmediate renderQueue;
    rendern::GLTextureUploader uploader;

    TextureIO io{ decoder, uploader, jobs, renderQueue };

    ResourceManager resourceManager;

    TextureProperties props{};
    props.filePath = "textures/brick.png";
    props.srgb = true;
    props.generateMips = true;

    auto texRes = resourceManager.LoadAsync<TextureResource>("brick", io, props);

    // ---- Renderer ----
    rendern::Renderer renderer(*device);

    while (!glfwWindowShouldClose(wnd))
    {
        glfwPollEvents();

        resourceManager.ProcessUploads<TextureResource>(io, 8, 16);

        rhi::TextureHandle sampled{};
        if (resourceManager.GetState<TextureResource>("brick") == ResourceState::Loaded)
        {
            sampled = rhi::TextureHandle{ texRes->GetResource().id };
        }

        renderer.RenderFrame(*swapchain, sampled, 0);
    }

    renderer.Shutdown();

    resourceManager.Clear<TextureResource>();
    resourceManager.ProcessUploads<TextureResource>(io, 0, 128);

    glfwDestroyWindow(wnd);
    glfwTerminate();
    return 0;
#endif

}