#include "profiling.h"
#include "utils.h"
#include "volcano.h"

#include <cstdio>
#include <cstdlib>


#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if __WINDOWS__
#	define GLFW_EXPOSE_NATIVE_WIN32
#	include <GLFW/glfw3native.h>
#endif

#include <backends/imgui_impl_glfw.h>
#include <imgui.h>

static MouseState g_mouse = {-1.0, -1.0, 0, 0, 0, false};
static KeyboardState g_keyboard = {0, 0, 0, 0};
static WindowState g_window = {0, 0, 1920, 1080, 0, 0, 0, false};

static void onError(int error, const char* description)
{
	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

static void onMouseEnter(GLFWwindow* window, int entered)
{
	g_mouse.insideWindow = entered;

	volcano_mouse(&g_mouse);
}

static void onMouseButton(GLFWwindow* window, int button, int action, int mods)
{
	g_mouse.button = button;
	g_mouse.action = action;
	g_mouse.mods = mods;

	volcano_mouse(&g_mouse);
}

static void onMouseCursorPos(GLFWwindow* window, double xpos, double ypos)
{
	g_mouse.xpos = xpos;
	g_mouse.ypos = ypos;

	volcano_mouse(&g_mouse);
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
				g_window.fullscreenEnabled = false;

				glfwSetWindowMonitor(
					window, nullptr, g_window.x, g_window.y, g_window.width, g_window.height, 0);
			}
			else
			{
				if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
				{
					const GLFWvidmode* mode = glfwGetVideoMode(monitor);

					g_window.fullscreenWidth = mode->width;
					g_window.fullscreenHeight = mode->height;
					g_window.fullscreenRefresh = mode->refreshRate;
					g_window.fullscreenEnabled = true;

					int x, y, width, height;
					glfwGetWindowPos(window, &x, &y);
					glfwGetWindowSize(window, &width, &height);

					g_window.x = x;
					g_window.y = y;
					g_window.width = width;
					g_window.height = height;

					glfwSetWindowMonitor(
						window,
						monitor,
						0,
						0,
						g_window.fullscreenWidth,
						g_window.fullscreenHeight,
						g_window.fullscreenRefresh);
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

	volcano_keyboard(&g_keyboard);
}

static void onFramebufferResize(GLFWwindow*, int w, int h)
{
	if (w == 0 || h == 0)
		return;

	volcano_resizeFramebuffer(w, h);
}

static void onWindowResize(GLFWwindow*, int w, int h)
{
	if (!g_window.fullscreenEnabled)
	{
		g_window.width = w;
		g_window.height = h;
	}

	volcano_resizeWindow(&g_window);
}

static void onWindowFocusChanged(GLFWwindow* window, int focused) {}

static void onWindowRefresh(GLFWwindow* window) {}

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

const char* getCmdOption(const char* const* begin, const char* const* end, const char* option)
{
	while (begin != end)
	{
		if (strcmp(*begin++, option) == 0)
			return *begin;
	}

	return nullptr;
}

int main(int argc, char** argv)
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

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(g_window.width, g_window.height, "", NULL, NULL);

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

#if __WINDOWS__
	auto windowHandle = glfwGetWin32Window(window);
	volcano_create(
		reinterpret_cast<void*>(&windowHandle),
#else
	volcano_create(
		reinterpret_cast<void*>(window),
#endif
		g_window.width,
		g_window.height,
		"./",
		getCmdOption(argv, argv + argc, R"(-r)"),
		getCmdOption(argv, argv + argc, R"(-u)"));

	glfwSetWindowTitle(window, volcano_getAppName());

	ImGui_ImplGlfw_InitForVulkan(window, true);

#if defined(_DEBUG) && defined(__WINDOWS__)
	uint64_t frameIndex = 0;
#endif
	do
	{
		FrameMark;
		ZoneScopedN("main::gameLoop");

		{
			ZoneScopedN("main::glfwPollEvents");
			glfwPollEvents();
		}

		{
			ZoneScopedN("main::ImGui_ImplGlfw_NewFrame");
			ImGui_ImplGlfw_NewFrame();
		}

	} while (!glfwWindowShouldClose(window) && !volcano_draw());

	ImGui_ImplGlfw_Shutdown();

	glfwDestroyWindow(window);
	glfwTerminate();

	volcano_destroy();

	return 0;
}
