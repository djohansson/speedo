#include "capi.h"

#include <core/capi.h>
#include <rhi/capi.h>

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

static MouseEvent g_mouse;
static KeyboardEvent g_keyboard;
static PathConfig g_paths;

static volatile bool g_isInterrupted = false;

static void onExit(void) 
{
	size_t windowCount;
	WindowHandle* windows = rhi_getWindows(&windowCount);
	for (size_t i = 0; i < windowCount; ++i)
		glfwDestroyWindow(windows[i]);

	client_destroy();
	glfwTerminate();
}

static void onSignal(int signal)
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

	if (entered)
		rhi_setCurrentWindow(window);

	g_mouse.insideWindow = entered;
	g_mouse.flags = Window;

	core_mouse(&g_mouse);
}

static void onMouseButton(GLFWwindow* window, int button, int action, int mods)
{
	assert(window != NULL);

	g_mouse.button = button;
	g_mouse.action = action;
	g_mouse.mods = mods;
	g_mouse.flags = Button;

	core_mouse(&g_mouse);
}

static void onMouseCursorPos(GLFWwindow* window, double xpos, double ypos)
{
	assert(window != NULL);

	g_mouse.xpos = xpos;
	g_mouse.ypos = ypos;
	g_mouse.flags = Position;

	core_mouse(&g_mouse);
}

static void onScroll(GLFWwindow* window, double xoffset, double yoffset)
{
	assert(window != NULL);

	g_mouse.xoffset = xoffset;
	g_mouse.yoffset = yoffset;
	g_mouse.flags = Scroll;

	core_mouse(&g_mouse);
}


static void onWindowFullscreenChanged(GLFWwindow* window)
{
	assert(window != NULL);

	WindowState* windowState = rhi_getWindowState(window);

	assert(windowState != NULL);

	GLFWmonitor* windowMonitor = glfwGetWindowMonitor(window);

	if (windowMonitor)
	{
		glfwSetWindowMonitor(
			window,
			NULL,
			windowState->x,
			windowState->y,
			windowState->width,
			windowState->height,
			0);
		
		windowState->fullscreenRefresh = 0;
		windowState->fullscreenEnabled = false;
	}
	else
	{
		GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();

		if (primaryMonitor)
		{
			const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);

			assert(mode != NULL);

			windowState->x = 0;
			windowState->y = 0;
			windowState->width = mode->width;
			windowState->height = mode->height;

			glfwSetWindowMonitor(
				window,
				primaryMonitor,
				windowState->x,
				windowState->y,
				windowState->width,
				windowState->height,
				windowState->fullscreenRefresh);

			windowState->fullscreenRefresh = mode->refreshRate;
			windowState->fullscreenEnabled = true;
		}
	}
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

			onWindowFullscreenChanged(window);
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

	core_keyboard(&g_keyboard);
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

static void onDrop(GLFWwindow* window, int count, const char** paths)
{
	assert(window != NULL);
}

static void onFramebufferResize(GLFWwindow* window, int w, int h)
{
	assert(window != NULL);
	assert(w > 0);
	assert(h > 0);

	rhi_resizeFramebuffer(window, w, h);
}

static void onWindowContentScaleChanged(GLFWwindow* window, float xscale, float yscale)
{
	assert(window != NULL);
	assert(xscale > 0);
	assert(yscale > 0);

	WindowState* windowState = rhi_getWindowState(window);

	assert(windowState != NULL);

	windowState->xscale = xscale;
	windowState->yscale = yscale;
}


static void onWindowFocusChanged(GLFWwindow* window, int focused)
{
	assert(window != NULL);
}

static void onWindowRefreshChanged(GLFWwindow* window)
{
	assert(window != NULL);
}

static void onWindowIconifyChanged(GLFWwindow* window, int iconified)
{
	assert(window != NULL);

	if (iconified)
	{
		// The window was iconified
	}
	else
	{
		// The window was restored
	}
}

static void onWindowMaximizeChanged(GLFWwindow* window, int maximized)
{
	assert(window != NULL);

	if (maximized)
	{
		// The window was maximized
	}
	else
	{
		// The window was restored
	}
}

static void onWindowSizeChanged(GLFWwindow* window, int width, int height)
{
	assert(window != NULL);
}

static WindowHandle onCreateWindow(WindowState* state)
{
	assert(state);

	// todo: fullscreen on create

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

	GLFWwindow* window  = glfwCreateWindow(state->width, state->height, "", NULL, NULL);

	assert(window);

	float xscale, yscale;
	glfwGetWindowContentScale(window, &xscale, &yscale);

	state->xscale = xscale;
	state->yscale = yscale;

	return window;
}

static void setWindowCallbacks(GLFWwindow* window)
{
	assert(window != NULL);

	glfwSetCursorEnterCallback(window, onMouseEnter);
	glfwSetMouseButtonCallback(window, onMouseButton);
	glfwSetCursorPosCallback(window, onMouseCursorPos);
	glfwSetScrollCallback(window, onScroll);
	glfwSetKeyCallback(window, onKey);
	glfwSetDropCallback(window, onDrop);
	glfwSetFramebufferSizeCallback(window, onFramebufferResize);
	glfwSetWindowFocusCallback(window, onWindowFocusChanged);
	glfwSetWindowRefreshCallback(window, onWindowRefreshChanged);
	glfwSetWindowContentScaleCallback(window, onWindowContentScaleChanged);
	glfwSetWindowIconifyCallback(window, onWindowIconifyChanged);
	glfwSetWindowMaximizeCallback(window, onWindowMaximizeChanged);
	glfwSetWindowSizeCallback(window, onWindowSizeChanged);
	glfwSetWindowTitle(window, core_getAppName());
}

int main(int argc, char* argv[], char* envp[])
{
	assert(argv != NULL);
	assert(envp != NULL);

	signal(SIGINT, &onSignal);
	signal(SIGTERM, &onSignal);

	cag_option_context cagContext;
	cag_option_init(&cagContext, g_cmdArgs, CAG_ARRAY_SIZE(g_cmdArgs), argc, argv);
	
	while (cag_option_fetch(&cagContext))
	{
		switch (cag_option_get_identifier(&cagContext))
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
			return EXIT_SUCCESS;
		default:
			break;
		}
	}

	(void)mi_version();

	glfwSetErrorCallback(onError);
	
	if (!glfwInit())
	{
		fprintf(stderr, "GLFW: Failed to initialize.\n");
		return EXIT_FAILURE;
	}

	if (!glfwVulkanSupported())
	{
		fprintf(stderr, "GLFW: Vulkan not supported.\n");
		return 1;
	}

	int monitorCount;
	GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
	assert(monitors != NULL); (void)monitors;
	if (monitorCount <= 0)
	{
		fprintf(stderr, "GLFW: No monitor connected?\n");
		return EXIT_FAILURE;
	}

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	for (int monitorIt = 0; monitorIt < monitorCount; ++monitorIt)
	{
		GLFWmonitor* monitor = monitors[monitorIt];
		assert(monitor != NULL);

		const char* name = glfwGetMonitorName(monitor);
		assert(name != NULL);

		int x, y;
		glfwGetMonitorPos(monitor, &x, &y);
		
		int physicalWidth, physicalHeight;
		glfwGetMonitorPhysicalSize(monitor, &physicalWidth, &physicalHeight);
		
		const GLFWvidmode* mode = glfwGetVideoMode(monitor);
		assert(mode != NULL);
		
		float xscale, yscale;
		glfwGetMonitorContentScale(monitor, &xscale, &yscale);

		printf("GLFW: Connected Monitor %i: %s, Position: %ix%i, Physical Size: %ix%i, Video Mode: %ix%i@%i[%i:%i:%i], Content Scale: %fx%f\n",
			monitorIt, name,
			x, y,
			physicalWidth, physicalHeight,
			mode->width, mode->height, mode->refreshRate, mode->redBits, mode->greenBits, mode->blueBits,
			xscale, yscale);
	}
#endif

	// todo: enable raw mouse input
	// if (!glfwRawMouseMotionSupported())
	// {
	// 	fprintf(stderr, "GLFW: Raw mouse motion not supported.\n");
	// 	return 1;
	// }
	// glfwSetInputMode(g_window.handle, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
	// glfwSetInputMode(g_window.handle, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);

	client_create(onCreateWindow, &g_paths);

	atexit(onExit);

	glfwSetMonitorCallback(onMonitorChanged);

	WindowHandle window = rhi_getCurrentWindow();
	assert(window != NullWindowHandle);

	setWindowCallbacks(window);

	do { glfwWaitEvents(); } while (!glfwWindowShouldClose(window) && client_tick() && !g_isInterrupted);

	return EXIT_SUCCESS;
}
