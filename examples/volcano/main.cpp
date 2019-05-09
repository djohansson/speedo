#include <stdio.h>
#include <stdlib.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <examples/imgui_impl_glfw.cpp>

#include "volcano.h"

static mouse_state g_mouse = { -1.0, -1.0, 0, 0, 0, false };
static keyboard_state g_keyboard = { 0, 0, 0, 0 };
static window_state g_window = { 0, 0, 1920, 1080, -1, -1, -1, false };

static void onError(int error, const char* description)
{
	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

static void onMouseEnter(GLFWwindow* window, int entered)
{
    g_mouse.inside_window = entered;

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
	g_mouse.xpos = xpos;
	g_mouse.ypos = ypos;

	vkapp_mouse(&g_mouse);
}

static void onKey(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	static bool fullscreenChangeTriggered = false;
	if (key == GLFW_KEY_ENTER && mods == GLFW_MOD_ALT)
    {
		if (!fullscreenChangeTriggered)
		{
			fullscreenChangeTriggered = true;

			if (GLFWmonitor* monitor = glfwGetWindowMonitor(window))
			{
				g_window.fullscreen_enabled = false;

				glfwSetWindowMonitor(window, nullptr,
					g_window.x, g_window.y, g_window.width, g_window.height, 0);
			}
			else
			{
				if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
				{
					const GLFWvidmode* mode = glfwGetVideoMode(monitor);

					g_window.fullscreen_width = mode->width;
					g_window.fullscreen_height = mode->height;
					g_window.fullscreen_refresh = mode->refreshRate;
					g_window.fullscreen_enabled = true;

					glfwGetWindowPos(window, &g_window.x, &g_window.y);
					glfwGetWindowSize(window, &g_window.width, &g_window.height);
					glfwSetWindowMonitor(window, monitor, 0, 0,
						g_window.fullscreen_width, g_window.fullscreen_height, g_window.fullscreen_refresh);
				}
			}
		}
    }
	else
	{
		fullscreenChangeTriggered = false;
	}

	g_keyboard.key = key;
	g_keyboard.scancode = scancode;
	g_keyboard.action = action;
	g_keyboard.mods = mods;

	vkapp_keyboard(&g_keyboard);
}

static void onFramebufferResize(GLFWwindow*, int w, int h)
{
	vkapp_resizeFramebuffer(w, h);
}

static void onWindowResize(GLFWwindow*, int w, int h)
{
	if (!g_window.fullscreen_enabled)
	{
		g_window.width = w;
		g_window.height = h;
	}

	vkapp_resizeWindow(&g_window);
}

static void onWindowFocusChanged(GLFWwindow* window, int focused)
{
}

static void onWindowRefresh(GLFWwindow* window)
{
}

static void onMonitorChanged(GLFWmonitor* monitor, int event)
{
    if (event == GLFW_CONNECTED)
    {
        // The monitor was connected
    }
    else if (event == GLFW_DISCONNECTED)
    {
        // The monitor was disconnected
    }
}

int main(int, char**)
{
	// Setup window
	glfwSetErrorCallback(onError);
	if (!glfwInit())
		return 1;

	int monitorCount;
	GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
	(void)monitors;
	if (monitorCount <= 0)
	{
		printf("GLFW: No monitor connected?\n");
		return 1;
	}

	//GLFWmonitor* selectedMonitor = monitors[0];
	//int videoModeCount;
	//const GLFWvidmode* modes = glfwGetVideoModes(selectedMonitor, &videoModeCount);
	//const GLFWvidmode* selectedVideoMode = glfwGetVideoMode(selectedMonitor);

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(g_window.width, g_window.height, "volcano", NULL, NULL);

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
	glfwSetWindowSizeCallback(window, onWindowResize);
	glfwSetFramebufferSizeCallback(window, onFramebufferResize);
	glfwSetWindowFocusCallback(window, onWindowFocusChanged);
	glfwSetWindowRefreshCallback(window, onWindowRefresh);
	glfwSetMonitorCallback(onMonitorChanged);

	int framebufferWidth, framebufferHeight;
	glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

	vkapp_create(window, g_window.width, g_window.height, framebufferWidth, framebufferHeight, "./resources/", true);

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
