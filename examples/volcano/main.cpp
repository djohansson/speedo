#include <stdio.h>
#include <stdlib.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <examples/imgui_impl_glfw.cpp>

#include "volcano.h"

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

static void glfw_resize_callback(GLFWwindow*, int w, int h)
{
	vkapp_resize(w, h);
}

int main(int, char**)
{
	// todo: parse commandline
	static constexpr uint32_t windowWidth = 1280;
	static constexpr uint32_t windowHeight = 720;

	// Setup window
	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit())
		return 1;

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight, "volcano", NULL, NULL);

	// Setup Vulkan
	if (!glfwVulkanSupported())
	{
		printf("GLFW: Vulkan Not Supported\n");
		return 1;
	}

	int framebufferWidth, framebufferHeight;
	glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

	vkapp_create(window, windowWidth, windowHeight, framebufferWidth, framebufferHeight, "./resources/", true);

	// Create Framebuffer resize callback
	glfwSetFramebufferSizeCallback(window, glfw_resize_callback);

	// Setup GLFW binding
	ImGui_ImplGlfw_InitForVulkan(window, true);

	while (!glfwWindowShouldClose(window))
	{
		// Poll and handle events (inputs, window resize, etc.)
		// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui
		// wants to use your inputs.
		// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main
		// application.
		// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main
		// application. Generally you may always pass all inputs to dear imgui, and hide them from
		// your application based on those two flags.
		glfwPollEvents();

		ImGui_ImplGlfw_NewFrame();

		vkapp_draw();
	}

	ImGui_ImplGlfw_Shutdown();

	vkapp_destroy();

	glfwDestroyWindow(window);
	glfwTerminate();

	return 0;
}
