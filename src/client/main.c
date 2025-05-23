#include "capi.h"

#include <core/assert.h>
#include <core/capi.h>
#include <rhi/capi.h>

#include <signal.h> 
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cargs.h>
#include <ctrace/ctrace.h>
#include <GLFW/glfw3.h>
#if __WINDOWS__
#	define GLFW_EXPOSE_NATIVE_WIN32
#	include <GLFW/glfw3native.h>
#endif
#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc.h>
#endif

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
		.access_letters = "h?",
		.access_name = "help",
		.description = "Shows the command help"
	}
};
static struct MouseEvent gMouse;
static struct KeyboardEvent gKeyboard;
static struct PathConfig gPaths;
static volatile bool gIsInterrupted = false;

static void OnSignal(int signal)
{
	switch (signal)
	{	
//	case SIGINT:
	case SIGTERM:
		gIsInterrupted = true;
		return;
	default:
		break;
	}

#if defined(__WINDOWS__)
	LOG_ERROR("Unhandled signal\n");
#else
	LOG_ERROR("Unhandled signal: %s\n", strsignal(signal));
#endif
}

static void OnError(int error, const char* description)
{
	ASSERT(description != NULL);

	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

static void OnMouseEnter(GLFWwindow* window, int entered)
{
	ASSERT(window != NULL);

	if (entered)
		SetCurrentWindow(window);

	gMouse.insideWindow = entered;
	gMouse.flags = kWindow;

	UpdateMouse(&gMouse);
}

static void OnMouseButton(GLFWwindow* window, int button, int action, int mods)
{
	ASSERT(window != NULL);

	gMouse.button = button;
	gMouse.action = action;
	gMouse.mods = mods;
	gMouse.flags = kButton;

	UpdateMouse(&gMouse);
}

static void OnMouseCursorPos(GLFWwindow* window, double xpos, double ypos)
{
	ASSERT(window != NULL);

	gMouse.xpos = xpos;
	gMouse.ypos = ypos;
	gMouse.flags = kPosition;

	UpdateMouse(&gMouse);
}

static void OnScroll(GLFWwindow* window, double xoffset, double yoffset)
{
	ASSERT(window != NULL);

	gMouse.xoffset = xoffset;
	gMouse.yoffset = yoffset;
	gMouse.flags = kScroll;

	UpdateMouse(&gMouse);
}

static void OnWindowFullscreenChanged(GLFWwindow* window)
{
	ASSERT(window != NULL);

	struct WindowState* windowState = GetWindowState(window);

	ASSERT(windowState != NULL);

	GLFWmonitor* windowMonitor = glfwGetWindowMonitor(window);

	if (windowMonitor)
	{
		glfwSetWindowMonitor(
			window,
			NULL,
			(int)windowState->x,
			(int)windowState->y,
			(int)windowState->width,
			(int)windowState->height,
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

			ASSERT(mode != NULL);

			windowState->x = 0;
			windowState->y = 0;
			windowState->width = mode->width;
			windowState->height = mode->height;

			glfwSetWindowMonitor(
				window,
				primaryMonitor,
				(int)windowState->x,
				(int)windowState->y,
				(int)windowState->width,
				(int)windowState->height,
				(int)windowState->fullscreenRefresh);

			windowState->fullscreenRefresh = mode->refreshRate;
			windowState->fullscreenEnabled = true;
		}
	}
}

static void OnKey(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	ASSERT(window != NULL);

	static bool gFullscreenChangeTriggered = false;
	if (key == GLFW_KEY_ENTER && mods == GLFW_MOD_ALT)
	{
		if (!gFullscreenChangeTriggered)
		{
			gFullscreenChangeTriggered = true;

			OnWindowFullscreenChanged(window);
		}
	}
	else
	{
		gFullscreenChangeTriggered = false;
	}

	gKeyboard.key = key;
	gKeyboard.scancode = scancode;
	gKeyboard.action = action;
	gKeyboard.mods = mods;

	UpdateKeyboard(&gKeyboard);
}

static void OnMonitorChanged(GLFWmonitor* monitor, int event)
{
	ASSERT(monitor != NULL);

	/*
	if (event == GLFW_CONNECTED)
	{
		// The monitor was connected
	}
	else if (event == GLFW_DISCONNECTED)
	{
		// The monitor was disconnected
	}
	*/
}

static void OnDrop(GLFWwindow* window, int count, const char** paths)
{
	ASSERT(window != NULL);
}

static void OnFramebufferResize(GLFWwindow* window, int width, int height)
{
	ASSERT(window != NULL);
	ASSERT(width > 0);
	ASSERT(height > 0);

	ResizeFramebuffer(window, width, height);
}

static void OnWindowContentScaleChanged(GLFWwindow* window, float xscale, float yscale)
{
	ASSERT(window != NULL);
	ASSERT(xscale > 0);
	ASSERT(yscale > 0);

	struct WindowState* windowState = GetWindowState(window);

	ASSERT(windowState != NULL);

	windowState->xscale = xscale;
	windowState->yscale = yscale;
}


static void OnWindowFocusChanged(GLFWwindow* window, int focused)
{
	ASSERT(window != NULL);
}

static void OnWindowRefreshChanged(GLFWwindow* window)
{
	ASSERT(window != NULL);
}

static void OnWindowIconifyChanged(GLFWwindow* window, int iconified)
{
	ASSERT(window != NULL);

	/*
	if (iconified)
	{
		// The window was iconified
	}
	else
	{
		// The window was restored
	}
	*/
}

static void OnWindowMaximizeChanged(GLFWwindow* window, int maximized)
{
	ASSERT(window != NULL);

	/*
	if (maximized)
	{
		// The window was maximized
	}
	else
	{
		// The window was restored
	}
	*/
}

static void OnWindowSizeChanged(GLFWwindow* window, int width, int height)
{
	ASSERT(window != NULL);
}

static void SetWindowCallbacks(GLFWwindow* window)
{
	ASSERT(window != NULL);

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

static WindowHandle OnCreateWindow(struct WindowState* state)
{
	ASSERT(state);

	// todo: fullscreen on create

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

	GLFWwindow* window  = glfwCreateWindow(
		(int)state->width,
		(int)state->height,
		"",
		NULL,
		NULL);

	ASSERT(window);

	float xscale;
	float yscale;
	glfwGetWindowContentScale(window, &xscale, &yscale);

	state->xscale = xscale;
	state->yscale = yscale;

	if (glfwRawMouseMotionSupported())
	 	glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);

	// glfwSetInputMode(g_window.handle, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);

	SetWindowCallbacks(window);

	return window;
}

static void* GlfwAllocate(size_t size, void* user)
{
#if defined(SPEEDO_USE_MIMALLOC)
	return mi_malloc(size);
#else
	return malloc(size);
#endif
}

static void GlfwDeallocate(void* block, void* user)
{
#if defined(SPEEDO_USE_MIMALLOC)
	mi_free(block);
#else
	free(block);
#endif
}

static void* GlfwReallocate(void* block, size_t size, void* user)
{
#if defined(SPEEDO_USE_MIMALLOC)
	return mi_realloc(block, size);
#else
	return realloc(block, size);
#endif
}

int main(int argc, char* argv[], char* envp[])
{
#if defined(SPEEDO_USE_MIMALLOC)
	mi_version(); // if not called first thing in main(), malloc will not be redirected correctly on windows
#endif

	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);
	signal(SIGILL, OnSignal);
	signal(SIGABRT, OnSignal);
	signal(SIGFPE, OnSignal);
	signal(SIGSEGV, OnSignal);

	ASSERT(argv != NULL);
	ASSERT(envp != NULL);

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

	glfwSetErrorCallback(OnError);
	
	GLFWallocator allocator = { .allocate = GlfwAllocate, .reallocate = GlfwReallocate, .deallocate = GlfwDeallocate };
	glfwInitAllocator(&allocator);
	
	ENSUREF(glfwInit(), "GLFW: Failed to initialize.\n");
	ENSUREF(glfwVulkanSupported(), "GLFW: Vulkan not supported.\n");
	
	int monitorCount;
	GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
	ENSUREF(monitors != NULL && monitorCount > 0, "GLFW: No monitor connected?\n");
	for (int monitorIt = 0; monitorIt < monitorCount; ++monitorIt)
	{
		GLFWmonitor* monitor = monitors[monitorIt];
		ASSERT(monitor != NULL);

		const char* name = glfwGetMonitorName(monitor);
		ASSERT(name != NULL);

		int monitorx;
		int monitory;
		glfwGetMonitorPos(monitor, &monitorx, &monitory);
		
		int physicalWidth;
		int physicalHeight;
		glfwGetMonitorPhysicalSize(monitor, &physicalWidth, &physicalHeight);
		
		const GLFWvidmode* mode = glfwGetVideoMode(monitor);
		ASSERT(mode != NULL);

		float xscale;
		float yscale;
		glfwGetMonitorContentScale(monitor, &xscale, &yscale);

		printf("GLFW: Connected Monitor %i: %s, Position: %ix%i, Physical Size: %ix%i, Video Mode: %ix%i@%i[%i:%i:%i], Content Scale: %fx%f\n",
			monitorIt, name,
			monitorx, monitory,
			physicalWidth, physicalHeight,
			mode->width, mode->height, mode->refreshRate, mode->redBits, mode->greenBits, mode->blueBits,
			xscale, yscale);
	}

	ClientCreate(OnCreateWindow, &gPaths);

	glfwSetMonitorCallback(OnMonitorChanged);

	do { glfwWaitEvents();
	} while (!(bool)glfwWindowShouldClose(GetCurrentWindow()) && ClientMain() && !gIsInterrupted);

	size_t windowCount;
	WindowHandle* windows = GetWindows(&windowCount);
	for (size_t i = 0; i < windowCount; ++i)
		glfwDestroyWindow(windows[i]);

	ClientDestroy();

	glfwTerminate();

	return EXIT_SUCCESS;
}
