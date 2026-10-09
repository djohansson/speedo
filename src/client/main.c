#include <core/assert.h>
#include <core/capi.h>

#include <client/capi.h>

#include <platform/capi.h>
#include <rhi/capi.h>

#include <signal.h> 
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cargs.h>
#include <ctrace/ctrace.h>
#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#	define GLFW_EXPOSE_NATIVE_COCOA
#	include <GLFW/glfw3native.h>
#endif
#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc.h>
#endif
#if !defined(__WINDOWS__)
#	include <pthread.h>
#	include <unistd.h>
#endif
#if defined(__APPLE__)
#	include <objc/message.h>
#	include <objc/runtime.h>
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
static atomic_bool gIsInterrupted = false;
static atomic_bool gIsGlfwInitialized = false;

static void OnInterrupt(void)
{
	atomic_store(&gIsInterrupted, true);

	// the main loop sleeps in glfwWaitEvents() until the next window event, so wake it up
	if (atomic_load(&gIsGlfwInitialized))
		glfwPostEmptyEvent();
}

#ifndef __WINDOWS__
// glfwPostEmptyEvent() is not async-signal-safe, so the SIGINT handler only writes to a pipe (which is),
// and this thread forwards the wakeup to glfw from a regular thread context (self-pipe trick).
static int gInterruptPipe[2] = {-1, -1};

static void* InterruptThreadMain(void* arg)
{
	(void)arg;

	char byte;
	while (read(gInterruptPipe[0], &byte, 1) == 1)
		OnInterrupt();

	return NULL;
}

static void StartInterruptThread(void)
{
	ENSURE(pipe(gInterruptPipe) == 0);

	pthread_t thread;
	ENSURE(pthread_create(&thread, NULL, InterruptThreadMain, NULL) == 0);
	ENSURE(pthread_detach(thread) == 0);
}
#endif

static void OnSignal(int sig)
{
	switch (sig)
	{	
	case SIGINT:
#if defined(__WINDOWS__)
		OnInterrupt(); // runs on a separate thread on windows, so calling into glfw is fine
#else
		atomic_store(&gIsInterrupted, true);
		(void)!write(gInterruptPipe[1], "", 1);
#endif
		return;
	case SIGTERM:
		LOG_ERROR("Program terminated.");
		break;
	case SIGABRT:
		LOG_ERROR("Program aborted.");
		break;
	default:
#if defined(__WINDOWS__)
		LOG_ERROR("Unhandled signal\n");
#else
		LOG_ERROR("Unhandled signal: %s\n", strsignal(sig));
#endif
		break;
	}

	// re-raise with the default action, so the process actually terminates. returning from e.g. SIGSEGV would
	// re-execute the faulting instruction and loop in this handler forever.
	signal(sig, SIG_DFL);
	raise(sig);
}

static void OnError(int error, const char* description)
{
	ENSURE(description != NULL);

	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

static void OnMouseEnter(GLFWwindow* window, int entered)
{
	ENSURE(window != NULL);

	if (entered)
		SetCurrentWindow((WindowHandle)window);

	gMouse.insideWindow = entered;
	gMouse.flags = kWindow;

	UpdateMouse(&gMouse);
}

static void OnMouseButton(GLFWwindow* window, int button, int action, int mods)
{
	ENSURE(window != NULL);

	gMouse.button = button;
	gMouse.action = action;
	gMouse.mods = mods;
	gMouse.flags = kButton;

	UpdateMouse(&gMouse);
}

static void OnMouseCursorPos(GLFWwindow* window, double xpos, double ypos)
{
	ENSURE(window != NULL);

	// glfw reports the cursor in screen coordinates, the renderer works in framebuffer pixels. their ratio is not the
	// content (dpi) scale: on windows and x11 the two coordinate spaces are the same, on macos/wayland they differ.
	int windowWidth, windowHeight, framebufferWidth, framebufferHeight;
	glfwGetWindowSize(window, &windowWidth, &windowHeight);
	glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

	gMouse.xpos = windowWidth > 0 ? xpos * framebufferWidth / windowWidth : xpos;
	gMouse.ypos = windowHeight > 0 ? ypos * framebufferHeight / windowHeight : ypos;
	// the enter callback only fires on transitions: a cursor that is already over the window when it opens never
	// "enters" it, which left insideWindow false (and the camera inactive) until the cursor left and came back
	gMouse.insideWindow = glfwGetWindowAttrib(window, GLFW_HOVERED);
	gMouse.flags = kPosition;

	UpdateMouse(&gMouse);
}

static void OnScroll(GLFWwindow* window, double xoffset, double yoffset)
{
	ENSURE(window != NULL);

	gMouse.xoffset = xoffset;
	gMouse.yoffset = yoffset;
	gMouse.flags = kScroll;

	UpdateMouse(&gMouse);
}

#if defined(__APPLE__)
// exclusive fullscreen (glfwSetWindowMonitor) lets macOS scan the swapchain images out directly, which flickers
// (frames shown out of order) with the vulkan driver even though the rendered frames are correct. a borderless window
// covering the monitor, with the menu bar and dock hidden, stays composited and doesn't.
static void SetMacPresentationOptions(unsigned long options)
{
	id app = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
	((void (*)(id, SEL, unsigned long))objc_msgSend)(app, sel_registerName("setPresentationOptions:"), options);
}

enum
{
	kMacPresentationDefault = 0,
	kMacPresentationHideDock = 1UL << 1, // NSApplicationPresentationHideDock
	kMacPresentationHideMenuBar = 1UL << 3, // NSApplicationPresentationHideMenuBar
};

// native fullscreen (the green title bar button) moves the window to its own space and scans it out directly too,
// with the same flicker. opting out turns the button into zoom. glfw resets the collection behavior in places
// (e.g. when leaving glfwSetWindowMonitor), so this is reapplied after those.
static void DisableMacNativeFullscreen(GLFWwindow* window)
{
	enum
	{
		kCollectionBehaviorFullScreenPrimary = 1UL << 7, // NSWindowCollectionBehaviorFullScreenPrimary
		kCollectionBehaviorFullScreenNone = 1UL << 9, // NSWindowCollectionBehaviorFullScreenNone
	};

	id nsWindow = glfwGetCocoaWindow(window);
	unsigned long behavior = ((unsigned long (*)(id, SEL))objc_msgSend)(nsWindow, sel_registerName("collectionBehavior"));
	behavior = (behavior & ~(unsigned long)kCollectionBehaviorFullScreenPrimary) | kCollectionBehaviorFullScreenNone;
	((void (*)(id, SEL, unsigned long))objc_msgSend)(nsWindow, sel_registerName("setCollectionBehavior:"), behavior);
}
#endif

static void OnWindowFullscreenChanged(GLFWwindow* window)
{
	ENSURE(window != NULL);

	struct WindowState* windowState = GetWindowState((WindowHandle)window);

	ENSURE(windowState != NULL);

	// windowed placement to restore when leaving fullscreen (windowState holds the fullscreen size by then)
	static int gWindowedX = 0;
	static int gWindowedY = 0;
	static int gWindowedWidth = 0;
	static int gWindowedHeight = 0;

#if defined(__APPLE__)
	bool isFullscreen = windowState->fullscreenEnabled; // the borderless window has no monitor
#else
	bool isFullscreen = glfwGetWindowMonitor(window) != NULL;
#endif

	if (isFullscreen)
	{
#if defined(__APPLE__)
		SetMacPresentationOptions(kMacPresentationDefault);
		glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
#endif
		glfwSetWindowMonitor(
			window,
			NULL,
			gWindowedX,
			gWindowedY,
			gWindowedWidth,
			gWindowedHeight,
			GLFW_DONT_CARE);
#if defined(__APPLE__)
		DisableMacNativeFullscreen(window);
#endif
		
		windowState->fullscreenRefresh = 0;
		windowState->fullscreenEnabled = false;
	}
	else
	{
		GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();

		if (primaryMonitor)
		{
			const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);

			ENSURE(mode != NULL);

			glfwGetWindowPos(window, &gWindowedX, &gWindowedY);
			glfwGetWindowSize(window, &gWindowedWidth, &gWindowedHeight);

			windowState->x = 0;
			windowState->y = 0;
			windowState->width = mode->width;
			windowState->height = mode->height;

#if defined(__APPLE__)
			int monitorX, monitorY;
			glfwGetMonitorPos(primaryMonitor, &monitorX, &monitorY);

			SetMacPresentationOptions(kMacPresentationHideDock | kMacPresentationHideMenuBar);
			glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
			glfwSetWindowPos(window, monitorX, monitorY);
			glfwSetWindowSize(window, mode->width, mode->height);
#else
			glfwSetWindowMonitor(
				window,
				primaryMonitor,
				(int)windowState->x,
				(int)windowState->y,
				(int)windowState->width,
				(int)windowState->height,
				mode->refreshRate); // not 0: glfw picks the mode closest to the requested rate, i.e. the lowest one
#endif

			windowState->fullscreenRefresh = mode->refreshRate;
			windowState->fullscreenEnabled = true;
		}
	}
}

static void OnKey(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	ENSURE(window != NULL);

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
	ENSURE(monitor != NULL);

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
	ENSURE(window != NULL);
}

static void OnFramebufferResize(GLFWwindow* window, int width, int height)
{
	ENSURE(window != NULL);

	// 0x0 while minimized, which the rhi handles by skipping frames until the window is restored
	ResizeFramebuffer((WindowHandle)window, width, height);
}

static void OnWindowContentScaleChanged(GLFWwindow* window, float xscale, float yscale)
{
	ENSURE(window != NULL);
	ASSERT(xscale > 0);
	ASSERT(yscale > 0);

	struct WindowState* windowState = GetWindowState((WindowHandle)window);

	ENSURE(windowState != NULL);

	windowState->xscale = xscale;
	windowState->yscale = yscale;
}


static void OnWindowFocusChanged(GLFWwindow* window, int focused)
{
	ENSURE(window != NULL);
}

static void OnWindowRefreshChanged(GLFWwindow* window)
{
	ENSURE(window != NULL);
}

static void OnWindowIconifyChanged(GLFWwindow* window, int iconified)
{
	ENSURE(window != NULL);

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
	ENSURE(window != NULL);

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
	ENSURE(window != NULL);
}

static void SetWindowCallbacks(GLFWwindow* window)
{
	ENSURE(window != NULL);

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

static WindowHandle OnCreateWindow(struct WindowState* inOutState)
{
	ENSURE(inOutState != NULL);
	ENSURE(inOutState->width > 0);
	ENSURE(inOutState->height > 0);

	// todo: fullscreen on create

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

	// SPEEDO_BACKGROUND=1 (automated runs, see scripts/assettest.ps1): the window opens behind the others, without taking
	// the focus, so that test runs don't interrupt whoever is working
	const char* backgroundSetting = getenv("SPEEDO_BACKGROUND");
	bool background = backgroundSetting != NULL && strcmp(backgroundSetting, "1") == 0;
	if (background)
	{
		glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
		glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	}

	GLFWwindow* window  = glfwCreateWindow(
		(int)inOutState->width,
		(int)inOutState->height,
		"",
		NULL,
		NULL);

	ENSURE(window != NULL);

#if defined(__APPLE__)
	DisableMacNativeFullscreen(window);
#endif

	if (background)
	{
#if defined(__APPLE__)
		// shown behind every other window (glfw's show orders it to the front)
		id nsWindow = glfwGetCocoaWindow(window);
		((void (*)(id, SEL, id))objc_msgSend)(nsWindow, sel_registerName("orderBack:"), NULL);
#else
		glfwShowWindow(window);
#endif
	}

	glfwGetWindowContentScale(window, &inOutState->xscale, &inOutState->yscale);

	if (glfwRawMouseMotionSupported())
	 	glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);

	// glfwSetInputMode(g_window.handle, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);

	SetWindowCallbacks(window);

	return (WindowHandle)window;
}

static void OnDestroyWindow(WindowHandle window)
{
	ENSURE(window != kInvalidWindowHandle);

	glfwDestroyWindow((GLFWwindow*)window);
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

#if !defined(__WINDOWS__)
	StartInterruptThread();
#endif
	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);
	signal(SIGILL, OnSignal);
	signal(SIGABRT, OnSignal);
	signal(SIGFPE, OnSignal);
	signal(SIGSEGV, OnSignal);

	ENSURE(argv != NULL);
	ENSURE(envp != NULL);

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
	atomic_store(&gIsGlfwInitialized, true);
	ENSUREF(glfwVulkanSupported(), "GLFW: Vulkan not supported.\n");
	
	int monitorCount;
	GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
	ENSUREF(monitors != NULL && monitorCount > 0, "GLFW: No monitor connected?\n");
	for (int monitorIt = 0; monitorIt < monitorCount; ++monitorIt)
	{
		GLFWmonitor* monitor = monitors[monitorIt];
		ENSURE(monitor != NULL);

		const char* name = glfwGetMonitorName(monitor);
		ENSURE(name != NULL);

		int monitorx;
		int monitory;
		glfwGetMonitorPos(monitor, &monitorx, &monitory);
		
		int physicalWidth;
		int physicalHeight;
		glfwGetMonitorPhysicalSize(monitor, &physicalWidth, &physicalHeight);
		
		const GLFWvidmode* mode = glfwGetVideoMode(monitor);
		ENSURE(mode != NULL);

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
	
	glfwSetMonitorCallback(OnMonitorChanged);

	ClientCreate(OnCreateWindow, &gPaths);
	do { glfwWaitEvents(); }
	while (!(bool)glfwWindowShouldClose((GLFWwindow*)GetCurrentWindow()) && ClientMain() && !atomic_load(&gIsInterrupted));//NOLINT(performance-no-int-to-ptr)
	ClientDestroy(OnDestroyWindow);
	
	atomic_store(&gIsGlfwInitialized, false);
	glfwTerminate();

	return EXIT_SUCCESS;
}
