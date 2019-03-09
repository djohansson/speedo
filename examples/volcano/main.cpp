#include <stdio.h>
#include <stdlib.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <examples/imgui_impl_glfw.cpp>

#include "volcano.h"

mouse_state g_mouse = { -1.0, -1.0, -1.0, -1.0, 0, 0, 0, false };
keyboard_state g_keyboard = { 0, 0, 0, 0 };

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

static void glfw_resize_callback(GLFWwindow*, int w, int h)
{
	vkapp_resize(w, h);
}

void onMouseEnter(GLFWwindow* window, int entered)
{
    g_mouse.inside_window = entered;

	if (!entered)
	{
		g_mouse.xpos_last = -1;
		g_mouse.ypos_last = -1;
	}

	vkapp_mouse(&g_mouse);
}

static void onMouseButton(GLFWwindow* window, int button, int action, int mods)
{
	g_mouse.button = button;
	g_mouse.action = action;
	g_mouse.mods = mods;

	vkapp_mouse(&g_mouse);
}

static void onMouseCursorPos(GLFWwindow* window, double xpos, double ypos)
{
	if (g_mouse.xpos_last < 0)
		g_mouse.xpos_last = xpos;
	else
		g_mouse.xpos_last = g_mouse.xpos;

	if (g_mouse.ypos_last < 0)
		g_mouse.ypos_last = ypos;
	else
		g_mouse.ypos_last = g_mouse.ypos;

	g_mouse.xpos = xpos;
	g_mouse.ypos = ypos;

	vkapp_mouse(&g_mouse);
}

static void onKey(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	g_keyboard.key = key;
	g_keyboard.scancode = scancode;
	g_keyboard.action = action;
	g_keyboard.mods = mods;

	vkapp_keyboard(&g_keyboard);
}

int main(int, char**)
{
	// todo: parse commandline
	static constexpr uint32_t windowWidth = 1920;
	static constexpr uint32_t windowHeight = 1080;

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

	glfwSetCursorEnterCallback(window, onMouseEnter);
	glfwSetMouseButtonCallback(window, onMouseButton);
	glfwSetCursorPosCallback(window, onMouseCursorPos);
	glfwSetKeyCallback(window, onKey);

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
