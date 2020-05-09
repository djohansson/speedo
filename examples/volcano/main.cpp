#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <string>
#include <tuple>

#if defined(_DEBUG) && defined(__WINDOWS__)
#include <crtdbg.h>
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <examples/imgui_impl_glfw.h>

#include "utils.h"
#include "volcano.h"

static MouseState g_mouse = { -1.0, -1.0, 0, 0, 0, false };
static KeyboardState g_keyboard = { 0, 0, 0, 0 };
static WindowState g_window = { 0, 0, 1920, 1080, 0, 0, 0, false };

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

				glfwSetWindowMonitor(window, nullptr,
					g_window.x, g_window.y, g_window.width, g_window.height, 0);
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

					glfwSetWindowMonitor(window, monitor, 0, 0,
						g_window.fullscreenWidth, g_window.fullscreenHeight, g_window.fullscreenRefresh);
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

#if defined(_DEBUG) && defined(__WINDOWS__)
int __cdecl CrtReportHook(int nReportType, char* szMsg, int* pnRet)
{
	int nRet = 0;

	printf("CRT report type is \"");
	switch (nReportType)
	{
	case _CRT_ASSERT: {
		printf("_CRT_ASSERT");
		// nRet = TRUE;   // Always stop for this type of report
		break;
	}

	case _CRT_WARN: {
		printf("_CRT_WARN");
		break;
	}

	case _CRT_ERROR: {
		printf("_CRT_ERROR");
		break;
	}

	default: {
		printf("???Unknown???");
		break;
	}
	}

	printf("\".\nCRT report message is:\n\t");
	printf_s(szMsg);

	if (pnRet)
		*pnRet = 0;

	return nRet;
}
#endif

std::tuple<bool, char*> getCmdOption(char** begin, char** end, const char* option)
{
    char** it = std::find(begin, end, option);
	char* next = nullptr;
	bool exist = it != end;
    if (exist && ++it != end)
        next = *it;
    
    return std::make_tuple(exist, next);
}

int main(int argc, char** argv)
{
#if defined(_DEBUG) && defined(__WINDOWS__)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
	int nRet = _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, CrtReportHook);
	printf("_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, CrtReportHook) returned %d\n", nRet);
#endif

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

	volcano_create(
		window,
		g_window.width,
		g_window.height,
		std::get<1>(getCmdOption(argv, argv + argc, "-r")),
		std::get<1>(getCmdOption(argv, argv + argc, "-u")));

	glfwSetWindowTitle(window, volcano_getAppName());

	ImGui_ImplGlfw_InitForVulkan(window, true);

	while (!glfwWindowShouldClose(window))
	{
		FrameMark;
		ZoneScopedN("gameLoop");

		{
			ZoneScopedN("glfw");
			glfwPollEvents();
		}

		{
			ZoneScopedN("imgui");
			ImGui_ImplGlfw_NewFrame();
		}

		volcano_draw();
	}

	ImGui_ImplGlfw_Shutdown();

	volcano_destroy();

	glfwDestroyWindow(window);
	glfwTerminate();

	return 0;
}
