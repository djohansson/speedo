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

static MouseEvent g_mouse;
static KeyboardEvent g_keyboard;
static WindowState g_window;
static PathConfig g_paths;

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
	g_mouse.flags = Window;

	client_mouse(&g_mouse);
}

static void onMouseButton(GLFWwindow* window, int button, int action, int mods)
{
	assert(window != NULL);

	g_mouse.button = button;
	g_mouse.action = action;
	g_mouse.mods = mods;
	g_mouse.flags = Button;

	client_mouse(&g_mouse);
}

static void onMouseCursorPos(GLFWwindow* window, double xpos, double ypos)
{
	assert(window != NULL);

	g_mouse.xpos = xpos;
	g_mouse.ypos = ypos;
	g_mouse.flags = Position;

	client_mouse(&g_mouse);
}

void onScroll(GLFWwindow* window, double xoffset, double yoffset)
{
	assert(window != NULL);

	g_mouse.xoffset = xoffset;
	g_mouse.yoffset = yoffset;
	g_mouse.flags = Scroll;

	client_mouse(&g_mouse);
}


static void onFullscreenChanged(GLFWwindow* window)
{
	GLFWmonitor* windowMonitor = glfwGetWindowMonitor(window);

	if (windowMonitor)
	{
		glfwSetWindowMonitor(
			window,
			NULL,
			g_window.x,
			g_window.y,
			g_window.width,
			g_window.height,
			0);
		
		g_window.fullscreenRefresh = 0;
		g_window.fullscreenEnabled = false;
	}
	else
	{
		GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();

		if (primaryMonitor)
		{
			const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);

			assert(mode != NULL);

			g_window.x = 0;
			g_window.y = 0;
			g_window.width = mode->width;
			g_window.height = mode->height;

			glfwSetWindowMonitor(
				window,
				primaryMonitor,
				g_window.x,
				g_window.y,
				g_window.width,
				g_window.height,
				g_window.fullscreenRefresh);

			g_window.fullscreenRefresh = mode->refreshRate;
			g_window.fullscreenEnabled = true;
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

			onFullscreenChanged(window);
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
	assert(g_window.xscale > 0);
	assert(g_window.yscale > 0);

	g_window.width = w / g_window.xscale;
	g_window.height = h / g_window.yscale;

	client_resizeFramebuffer(&g_window);
}

void onContentScaleChanged(GLFWwindow* window, float xscale, float yscale)
{
	assert(window != NULL);
	assert(xscale > 0);
	assert(yscale > 0);

	g_window.xscale = xscale;
	g_window.yscale = yscale;
}


static void onWindowFocusChanged(GLFWwindow* window, int focused)
{
	assert(window != NULL);
}

static void onWindowRefreshChanged(GLFWwindow* window)
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

void onDrop(GLFWwindow* window, int count, const char** paths)
{
	assert(window != NULL);
}

void* onCreateWindow(WindowState* state)
{
	assert(state);

	// todo: fullscreen on create

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

	GLFWwindow* window  = glfwCreateWindow(state->width, state->height, "", NULL, NULL);

	assert(window);

	state->handle = window;

	float xscale, yscale;
	glfwGetWindowContentScale(window, &xscale, &yscale);

	state->xscale = xscale;
	state->yscale = yscale;

	return window;
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

	if (!glfwRawMouseMotionSupported())
	{
		fprintf(stderr, "GLFW: Raw mouse motion not supported.\n");
		return 1;
	}

	// glfwSetInputMode(g_window.handle, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
	// glfwSetInputMode(g_window.handle, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);

	client_create(onCreateWindow, &g_window, &g_paths);

	atexit(onExit);

	glfwSetCursorEnterCallback(g_window.handle, onMouseEnter);
	glfwSetMouseButtonCallback(g_window.handle, onMouseButton);
	glfwSetCursorPosCallback(g_window.handle, onMouseCursorPos);
	glfwSetScrollCallback(g_window.handle, onScroll);
	glfwSetKeyCallback(g_window.handle, onKey);
	glfwSetFramebufferSizeCallback(g_window.handle, onFramebufferResize);
	glfwSetWindowFocusCallback(g_window.handle, onWindowFocusChanged);
	glfwSetWindowRefreshCallback(g_window.handle, onWindowRefreshChanged);
	glfwSetWindowContentScaleCallback(g_window.handle, onContentScaleChanged);
	glfwSetMonitorCallback(onMonitorChanged);
	glfwSetDropCallback(g_window.handle, onDrop);
	glfwSetWindowTitle(g_window.handle, client_getAppName());

	do { glfwWaitEvents(); } while (!glfwWindowShouldClose(g_window.handle) && client_tick() && !g_isInterrupted);

	return EXIT_SUCCESS;
}
