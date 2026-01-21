#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

import core;
import std;

static rhi::Extent2D GetDrawableExtent(GLFWwindow* wnd)
{
	int w = 0;
	int h = 0;
	glfwGetFramebufferSize(wnd, &w, &h);
	return rhi::Extent2D{ static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h) };
}

int main()
{
	if (!glfwInit())
	{
		std::cerr << "GLFW init failed\n";
		return 1;
	}

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

	GLFWwindow* wnd = glfwCreateWindow(1280, 720, "RenderGraph + RHI_GL Demo", nullptr, nullptr);
	if (!wnd)
	{
		std::cerr << "Failed to create GLFW window\n";
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

	auto device = rhi::CreateGLDevice(rhi::GLDeviceDesc{ .enableDebug = false });

	rhi::GLSwapChainDesc sc{};
	sc.base.extent = GetDrawableExtent(wnd);
	sc.base.vsync = true;
	sc.base.backbufferFormat = rhi::Format::BGRA8_UNORM;
	sc.hooks.present = [wnd] { glfwSwapBuffers(wnd); };
	sc.hooks.getDrawableExtent = [wnd] { return GetDrawableExtent(wnd); };
	sc.hooks.setVsync = [](bool vsync) { glfwSwapInterval(vsync ? 1 : 0); };

	auto swapchain = rhi::CreateGLSwapChain(*device, std::move(sc));

	std::cout << "Device: " << device->GetName() << "\n";

	StbTextureDecoder decoder;
	rendern::JobSystemImmediate jobs;
	rendern::RenderQueueImmediate renderQueue;
	rendern::GLTextureUploader uploader;

	TextureIO io{ decoder, uploader, jobs, renderQueue };

	ResourceManager rm;

	TextureProperties props{};
	props.srgb = true;
	props.generateMips = true;

	// ВАЖНО: это относительный путь от assets/
	props.filePath = "textures/brick.png";

	auto tex = rm.LoadAsync<TextureResource>("brick", io, props);

	rendern::Renderer renderer(*device);

	while (!glfwWindowShouldClose(wnd))
	{
		glfwPollEvents();

		rm.ProcessUploads<TextureResource>(io, 8, 8);

		rhi::TextureHandle sampled{};
		if (rm.GetState<TextureResource>("brick") == ResourceState::Loaded)
			sampled = rhi::TextureHandle{ tex->GetResource().id };

		renderer.RenderFrame(*swapchain, sampled);
	}

	renderer.Shutdown();

	// Clean up GL resources created by ResourceManager.
	rm.Clear<TextureResource>();
	rm.ProcessUploads<TextureResource>(io, 0, 64);

	glfwDestroyWindow(wnd);
	glfwTerminate();
	return 0;
}
