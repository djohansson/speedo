#include "capi.h"

#include <assert.h>
#include <signal.h> 
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>
#if __WINDOWS__
#	define GLFW_EXPOSE_NATIVE_WIN32
#	include <GLFW/glfw3native.h>
#endif

#include <mimalloc.h>

static MouseState g_mouse = {-1.0, -1.0, 0, 0, 0, false};
static KeyboardState g_keyboard = {0, 0, 0, 0};
static WindowState g_window = {NULL, NULL, 0, 0, 1920, 1080, 0, 0, 0, false};

static volatile bool g_isInterrupted = false;

void onExit(void) 
{
	glfwDestroyWindow(g_window.handle);

	client_destroy();

	glfwTerminate();
}

void onSignal(int signal)
{
	if (signal == SIGINT || signal == SIGTERM)
		g_isInterrupted = true;
}

static void onError(int error, const char* description)
{
	assert(description != NULL);

	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

static void onMouseEnter(GLFWwindow* window, int entered)
{
	assert(window != NULL);

	g_mouse.insideWindow = entered;

	client_mouse(&g_mouse);
}

static void onMouseButton(GLFWwindow* window, int button, int action, int mods)
{
	assert(window != NULL);

	g_mouse.button = button;
	g_mouse.action = action;
	g_mouse.mods = mods;

	client_mouse(&g_mouse);
}

static void onMouseCursorPos(GLFWwindow* window, double xpos, double ypos)
{
	assert(window != NULL);

	g_mouse.xpos = xpos;
	g_mouse.ypos = ypos;

	client_mouse(&g_mouse);
}

static void onKey(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	assert(window != NULL);

	static bool fullscreenChangeTriggered = false;
	if (key == GLFW_KEY_ENTER && mods == GLFW_MOD_ALT)
	{
		if (!fullscreenChangeTriggered)
		{
			fullscreenChangeTriggered = true;

			GLFWmonitor* windowMonitor = glfwGetWindowMonitor(window);

			if (windowMonitor)
			{
				g_window.fullscreenEnabled = false;

				glfwSetWindowMonitor(
					window, NULL, g_window.x, g_window.y, g_window.width, g_window.height, 0);
			}
			else
			{
				GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();

				if (primaryMonitor)
				{
					const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);

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
						primaryMonitor,
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

	client_keyboard(&g_keyboard);
}

static void onFramebufferResize(GLFWwindow* window, int w, int h)
{
	assert(window != NULL);
	assert(w > 0);
	assert(h > 0);

	client_resizeFramebuffer(w, h);
}

static void onWindowResize(GLFWwindow* window, int w, int h)
{
	assert(window != NULL);
	assert(w > 0);
	assert(h > 0);

	if (!g_window.fullscreenEnabled)
	{
		g_window.width = w;
		g_window.height = h;
	}

	client_resizeWindow(&g_window);
}

static void onWindowFocusChanged(GLFWwindow* window, int focused)
{
	assert(window != NULL);
}

static void onWindowRefresh(GLFWwindow* window)
{
	assert(window != NULL);
}

static void onMonitorChanged(GLFWmonitor* monitor, int event)
{
	assert(monitor != NULL);

	if (event == GLFW_CONNECTED)
	{
		// The monitor was connected
	}
	else if (event == GLFW_DISCONNECTED)
	{
		// The monitor was disconnected
	}
}

const char* getCmdOption(char** begin, char** end, const char* option)
{
	assert(begin != NULL);
	assert(end != NULL);
	assert(option != NULL);

	while (begin != end)
	{
		if (strcmp(*begin++, option) == 0)
			return *begin;
	}

	return NULL;
}

int main(int argc, char* argv[], char* env[])
{
	assert(argv != NULL);
	assert(env != NULL);

	atexit(onExit);

	signal(SIGINT, &onSignal);
	signal(SIGTERM, &onSignal);

	printf("mi_version(): %d\n", mi_version());
	
	glfwSetErrorCallback(onError);
	if (!glfwInit())
	{
		fprintf(stderr, "GLFW: Failed to initialize.\n");
		return EXIT_FAILURE;
	}

	int monitorCount;
	glfwGetMonitors(&monitorCount);
	if (monitorCount <= 0)
	{
		fprintf(stderr, "GLFW: No monitor connected?\n");
		return EXIT_FAILURE;
	}

	if (!glfwVulkanSupported())
	{
		fprintf(stderr, "GLFW: Vulkan Not Supported.\n");
		return 1;
	}

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(g_window.width, g_window.height, "", NULL, NULL);

	g_window.handle = window;
#if __WINDOWS__
	HWND nativeHandle = glfwGetWin32Window(window);
	g_window.nativeHandle = &nativeHandle;
#else
	g_window.nativeHandle = window;
#endif

	client_create(
		&g_window,
		"./",
		getCmdOption(argv, argv + argc, "(-r)"),
		getCmdOption(argv, argv + argc, "(-u)"));

	glfwSetCursorEnterCallback(window, onMouseEnter);
	glfwSetMouseButtonCallback(window, onMouseButton);
	glfwSetCursorPosCallback(window, onMouseCursorPos);
	glfwSetKeyCallback(window, onKey);
	glfwSetWindowSizeCallback(window, onWindowResize);
	glfwSetFramebufferSizeCallback(window, onFramebufferResize);
	glfwSetWindowFocusCallback(window, onWindowFocusChanged);
	glfwSetWindowRefreshCallback(window, onWindowRefresh);
	glfwSetMonitorCallback(onMonitorChanged);
	glfwSetWindowTitle(window, client_getAppName());

	do { glfwPollEvents(); } while (!glfwWindowShouldClose(window) && client_tick() && !g_isInterrupted);

	return EXIT_SUCCESS;
}
