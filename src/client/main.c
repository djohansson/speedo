#include "capi.h"

#include <assert.h>
#include <signal.h> 
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cargs.h>

#include <GLFW/glfw3.h>
#if __WINDOWS__
#	define GLFW_EXPOSE_NATIVE_WIN32
#	include <GLFW/glfw3native.h>
#endif

#include <mimalloc.h>

static struct cag_option g_cmdArgs[] =
{
	{
		.identifier = 'r',
		.access_letters = "r",
		.access_name = "resourcePath",
		.value_name = "VALUE",
		.description = "Path to resource directory"
	},
	{
		.identifier = 'u',
		.access_letters = "u",
		.access_name = "userProfilePath",
		.value_name = "VALUE",
		.description = "Path to user profile directory"
	},
	{
		.identifier = 'h',
		.access_letters = "h",
		.access_name = "help",
		.description = "Shows the command help"
	}
};

static MouseState g_mouse = {-1.0, -1.0, 0, 0, 0, false};
static KeyboardState g_keyboard = {0, 0, 0, 0};
static WindowState g_window = {NULL, NULL, 0, 0, 1920, 1080, 0, 0, 0, false};
static PathConfig g_paths = { NULL, NULL };

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

int main(int argc, char* argv[], char* envp[])
{
	assert(argv != NULL);
	assert(envp != NULL);

	atexit(onExit);

	signal(SIGINT, &onSignal);
	signal(SIGTERM, &onSignal);

	printf("mi_version(): %d\n", mi_version());

	for (char** env = envp; *env != NULL; ++env)
		printf("%s\n", *env);

	cag_option_context cagContext;
	cag_option_prepare(&cagContext, g_cmdArgs, CAG_ARRAY_SIZE(g_cmdArgs), argc, argv);
	
	while (cag_option_fetch(&cagContext))
	{
		switch (cag_option_get(&cagContext))
		{
		case 'u':
			g_paths.userProfilePath = cag_option_get_value(&cagContext);
			break;
		case 'r':
			g_paths.resourcePath = cag_option_get_value(&cagContext);
			break;
		case 'h':
			printf("Usage: client [OPTION]...\n");
			cag_option_print(g_cmdArgs, CAG_ARRAY_SIZE(g_cmdArgs), stdout);
			break;
		default:
			break;
		}
	}

	glfwSetErrorCallback(onError);
	
	if (!glfwInit())
	{
		fprintf(stderr, "GLFW: Failed to initialize.\n");
		return EXIT_FAILURE;
	}

	if (!glfwVulkanSupported())
	{
		fprintf(stderr, "GLFW: Vulkan Not Supported.\n");
		return 1;
	}

	int monitorCount;
	GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
	assert(monitors != NULL);
	if (monitorCount <= 0)
	{
		fprintf(stderr, "GLFW: No monitor connected?\n");
		return EXIT_FAILURE;
	}

	for (int monitorIt = 0; monitorIt < monitorCount; ++monitorIt)
	{
		GLFWmonitor* monitor = monitors[monitorIt];
		assert(monitor != NULL);

		const char* name = glfwGetMonitorName(monitor);
		assert(name != NULL);

		int x, y;
		glfwGetMonitorPos(monitor, &x, &y);
		
		int width, height;
		glfwGetMonitorPhysicalSize(monitor, &width, &height);
		
		const GLFWvidmode* mode = glfwGetVideoMode(monitor);
		assert(mode != NULL);
		
		float xscale, yscale;
		glfwGetMonitorContentScale(monitor, &xscale, &yscale);

		printf("GLFW: Connected Monitor %i: %s, Position: %ix%i, Physical Size: %ix%i, Video Mode: %ix%i@%i[%i:%i:%i], Content Scale: %fx%f\n",
			monitorIt, name,
			x, y,
			width, height,
			mode->width, mode->height, mode->refreshRate, mode->redBits, mode->greenBits, mode->blueBits,
			xscale, yscale);
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

	client_create(&g_window, &g_paths);

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
