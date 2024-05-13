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

static struct cag_option gCmdArgs[] =
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
static MouseEvent gMouse;
static KeyboardEvent gKeyboard;
static PathConfig gPaths;
static volatile bool gIsInterrupted = false;

static void OnExit(void) 
{
	size_t windowCount;
	WindowHandle* windows = GetWindows(&windowCount);
	for (size_t i = 0; i < windowCount; ++i)
		glfwDestroyWindow(windows[i]);

	DestroyClient();
	glfwTerminate();
}

static void OnSignal(int signal)
{
	if (signal == SIGINT || signal == SIGTERM)
		gIsInterrupted = true;
}

static void OnError(int error, const char* description)
{
	assert(description != NULL);

	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

static void OnMouseEnter(GLFWwindow* window, int entered)
{
	assert(window != NULL);

	if (entered)
		SetCurrentWindow(window);

	gMouse.insideWindow = entered;
	gMouse.flags = Window;

	UpdateMouse(&gMouse);
}

static void OnMouseButton(GLFWwindow* window, int button, int action, int mods)
{
	assert(window != NULL);

	gMouse.button = button;
	gMouse.action = action;
	gMouse.mods = mods;
	gMouse.flags = Button;

	UpdateMouse(&gMouse);
}

static void OnMouseCursorPos(GLFWwindow* window, double xpos, double ypos)
{
	assert(window != NULL);

	gMouse.xpos = xpos;
	gMouse.ypos = ypos;
	gMouse.flags = Position;

	UpdateMouse(&gMouse);
}

static void OnScroll(GLFWwindow* window, double xoffset, double yoffset)
{
	assert(window != NULL);

	gMouse.xoffset = xoffset;
	gMouse.yoffset = yoffset;
	gMouse.flags = Scroll;

	UpdateMouse(&gMouse);
}


static void OnWindowFullscreenChanged(GLFWwindow* window)
{
	assert(window != NULL);

	WindowState* windowState = GetWindowState(window);

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

static void OnKey(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	assert(window != NULL);

	static bool fullscreenChangeTriggered = false;
	if (key == GLFW_KEY_ENTER && mods == GLFW_MOD_ALT)
	{
		if (!fullscreenChangeTriggered)
		{
			fullscreenChangeTriggered = true;

			OnWindowFullscreenChanged(window);
		}
	}
	else
	{
		fullscreenChangeTriggered = false;
	}

	gKeyboard.key = key;
	gKeyboard.scancode = scancode;
	gKeyboard.action = action;
	gKeyboard.mods = mods;

	UpdateKeyboard(&gKeyboard);
}

static void OnMonitorChanged(GLFWmonitor* monitor, int event)
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

static void OnDrop(GLFWwindow* window, int count, const char** paths)
{
	assert(window != NULL);
}

static void OnFramebufferResize(GLFWwindow* window, int w, int h)
{
	assert(window != NULL);
	assert(w > 0);
	assert(h > 0);

	ResizeFramebuffer(window, w, h);
}

static void OnWindowContentScaleChanged(GLFWwindow* window, float xscale, float yscale)
{
	assert(window != NULL);
	assert(xscale > 0);
	assert(yscale > 0);

	WindowState* windowState = GetWindowState(window);

	assert(windowState != NULL);

	windowState->xscale = xscale;
	windowState->yscale = yscale;
}


static void OnWindowFocusChanged(GLFWwindow* window, int focused)
{
	assert(window != NULL);
}

static void OnWindowRefreshChanged(GLFWwindow* window)
{
	assert(window != NULL);
}

static void OnWindowIconifyChanged(GLFWwindow* window, int iconified)
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

static void OnWindowMaximizeChanged(GLFWwindow* window, int maximized)
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

static void OnWindowSizeChanged(GLFWwindow* window, int width, int height)
{
	assert(window != NULL);
}

static WindowHandle OnCreateWindow(WindowState* state)
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

	glfwSetCursorEnterCallback(window, OnMouseEnter);
	glfwSetMouseButtonCallback(window, OnMouseButton);
	glfwSetCursorPosCallback(window, OnMouseCursorPos);
	glfwSetScrollCallback(window, OnScroll);
	glfwSetKeyCallback(window, OnKey);
	glfwSetDropCallback(window, OnDrop);
	glfwSetFramebufferSizeCallback(window, OnFramebufferResize);
	glfwSetWindowFocusCallback(window, OnWindowFocusChanged);
	glfwSetWindowRefreshCallback(window, OnWindowRefreshChanged);
	glfwSetWindowContentScaleCallback(window, OnWindowContentScaleChanged);
	glfwSetWindowIconifyCallback(window, OnWindowIconifyChanged);
	glfwSetWindowMaximizeCallback(window, OnWindowMaximizeChanged);
	glfwSetWindowSizeCallback(window, OnWindowSizeChanged);
	glfwSetWindowTitle(window, GetApplicationName());
}

int main(int argc, char* argv[], char* envp[])
{
	assert(argv != NULL);
	assert(envp != NULL);

	signal(SIGINT, &OnSignal);
	signal(SIGTERM, &OnSignal);

	cag_option_context cagContext;
	cag_option_init(&cagContext, gCmdArgs, CAG_ARRAY_SIZE(gCmdArgs), argc, argv);
	
	while (cag_option_fetch(&cagContext))
	{
		switch (cag_option_get_identifier(&cagContext))
		{
		case 'u':
			gPaths.userProfilePath = cag_option_get_value(&cagContext);
			break;
		case 'r':
			gPaths.resourcePath = cag_option_get_value(&cagContext);
			break;
		case 'h':
			printf("Usage: client [OPTION]...\n");
			cag_option_print(gCmdArgs, CAG_ARRAY_SIZE(gCmdArgs), stdout);
			return EXIT_SUCCESS;
		default:
			break;
		}
	}

	(void)mi_version();

	glfwSetErrorCallback(OnError);
	
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

	CreateClient(OnCreateWindow, &gPaths);

	atexit(OnExit);

	glfwSetMonitorCallback(OnMonitorChanged);

	WindowHandle window = GetCurrentWindow();
	assert(window != NullWindowHandle);

	setWindowCallbacks(window);

	do { glfwWaitEvents(); } while (!glfwWindowShouldClose(window) && TickClient() && !gIsInterrupted);

	return EXIT_SUCCESS;
}
