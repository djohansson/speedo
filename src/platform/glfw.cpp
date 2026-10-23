// the window system's backend: glfw, from its initialization to its windows' callbacks, which turn its events into
// core's input events (in core's vocabulary: enum Key, MOUSE_BUTTON_*, INPUT_ACTION_*, INPUT_MOD_*) and the application's
// window handling (platform::WindowedApplication). glfw is platform's alone: here, windowedapplication.cpp (fullscreen,
// sizes, monitors) and imguiplatform.cpp (imgui's backend)

#include "glfw.h"
#include "windowedapplication.h"

#include <core/assert.h>
#include <core/capi.h>
#include <core/profiling.h>

// before glfw: it declares glfwCreateWindowSurface with vulkan's types only when they are
#include <vulkan/vulkan_core.h>

#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#	define GLFW_EXPOSE_NATIVE_COCOA
#	include <GLFW/glfw3native.h>
#	include <objc/message.h>
#	include <objc/runtime.h>
#elif defined(_WIN32)
// windows.h for SetWindowPos (see CreateNativeWindow): only the kernel and user parts, without min and max
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	ifndef NOGDI
#		define NOGDI
#	endif
#	define GLFW_EXPOSE_NATIVE_WIN32
#	include <GLFW/glfw3native.h>
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <string_view>

namespace platform::glfw
{

namespace detail
{

std::atomic_bool gInitialized = false;

// the running application's, if any (callbacks can come before it is published, and after it is gone)
std::shared_ptr<WindowedApplication> App()
{
	return std::static_pointer_cast<WindowedApplication>(core::Application::Get());
}

// glfw's key, as core's (enum Key)
uint16_t ToKey(int key)
{
	if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
		return static_cast<uint16_t>(KEY_0 + (key - GLFW_KEY_0));
	if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
		return static_cast<uint16_t>(KEY_A + (key - GLFW_KEY_A));
	if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12)
		return static_cast<uint16_t>(KEY_F1 + (key - GLFW_KEY_F1));
	switch (key)
	{
	case GLFW_KEY_SPACE: return KEY_SPACE;
	case GLFW_KEY_ESCAPE: return KEY_ESCAPE;
	case GLFW_KEY_ENTER: return KEY_ENTER;
	case GLFW_KEY_TAB: return KEY_TAB;
	case GLFW_KEY_BACKSPACE: return KEY_BACKSPACE;
	case GLFW_KEY_INSERT: return KEY_INSERT;
	case GLFW_KEY_DELETE: return KEY_DELETE;
	case GLFW_KEY_RIGHT: return KEY_RIGHT;
	case GLFW_KEY_LEFT: return KEY_LEFT;
	case GLFW_KEY_DOWN: return KEY_DOWN;
	case GLFW_KEY_UP: return KEY_UP;
	case GLFW_KEY_PAGE_UP: return KEY_PAGE_UP;
	case GLFW_KEY_PAGE_DOWN: return KEY_PAGE_DOWN;
	case GLFW_KEY_HOME: return KEY_HOME;
	case GLFW_KEY_END: return KEY_END;
	case GLFW_KEY_LEFT_SHIFT: return KEY_LEFT_SHIFT;
	case GLFW_KEY_LEFT_CONTROL: return KEY_LEFT_CONTROL;
	case GLFW_KEY_LEFT_ALT: return KEY_LEFT_ALT;
	case GLFW_KEY_LEFT_SUPER: return KEY_LEFT_SUPER;
	case GLFW_KEY_RIGHT_SHIFT: return KEY_RIGHT_SHIFT;
	case GLFW_KEY_RIGHT_CONTROL: return KEY_RIGHT_CONTROL;
	case GLFW_KEY_RIGHT_ALT: return KEY_RIGHT_ALT;
	case GLFW_KEY_RIGHT_SUPER: return KEY_RIGHT_SUPER;
	default: return KEY_UNKNOWN;
	}
}

uint8_t ToAction(int action)
{
	switch (action)
	{
	case GLFW_PRESS: return INPUT_ACTION_PRESS;
	case GLFW_REPEAT: return INPUT_ACTION_REPEAT;
	default: return INPUT_ACTION_RELEASE;
	}
}

uint8_t ToMouseButton(int button)
{
	switch (button)
	{
	case GLFW_MOUSE_BUTTON_LEFT: return MOUSE_BUTTON_LEFT;
	case GLFW_MOUSE_BUTTON_RIGHT: return MOUSE_BUTTON_RIGHT;
	case GLFW_MOUSE_BUTTON_MIDDLE: return MOUSE_BUTTON_MIDDLE;
	default: return MOUSE_BUTTON_OTHER;
	}
}

uint32_t ToMods(int mods)
{
	return ((mods & GLFW_MOD_SHIFT) != 0 ? INPUT_MOD_SHIFT : 0U) | ((mods & GLFW_MOD_CONTROL) != 0 ? INPUT_MOD_CONTROL : 0U) |
		   ((mods & GLFW_MOD_ALT) != 0 ? INPUT_MOD_ALT : 0U) | ((mods & GLFW_MOD_SUPER) != 0 ? INPUT_MOD_SUPER : 0U);
}

// the events so far, each callback updating what it reports (core's event handlers take the whole state)
MouseEvent gMouse{};
KeyboardEvent gKeyboard{};

void OnError(int error, const char* description)
{
	std::println(stderr, "Glfw Error {}: {}", error, description != nullptr ? description : "");
}

void OnMouseEnter(GLFWwindow* window, int entered)
{
	if (entered != 0)
		SetCurrentWindow(reinterpret_cast<WindowHandle>(window));

	gMouse.insideWindow = entered != 0 ? 1U : 0U;
	gMouse.flags = MouseEvent::kWindow;
	UpdateMouse(&gMouse);
}

void OnMouseButton(GLFWwindow* /*window*/, int button, int action, int mods)
{
	gMouse.button = ToMouseButton(button);
	gMouse.action = ToAction(action);
	gMouse.mods = ToMods(mods);
	gMouse.flags = MouseEvent::kButton;
	UpdateMouse(&gMouse);
}

void OnMouseCursorPos(GLFWwindow* window, double xpos, double ypos)
{
	// glfw reports the cursor in screen coordinates, the renderer works in framebuffer pixels. their ratio is not the
	// content (dpi) scale: on windows and x11 the two coordinate spaces are the same, on macos/wayland they differ.
	int windowWidth = 0;
	int windowHeight = 0;
	int framebufferWidth = 0;
	int framebufferHeight = 0;
	glfwGetWindowSize(window, &windowWidth, &windowHeight);
	glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

	gMouse.xpos = windowWidth > 0 ? xpos * framebufferWidth / windowWidth : xpos;
	gMouse.ypos = windowHeight > 0 ? ypos * framebufferHeight / windowHeight : ypos;
	// the enter callback only fires on transitions: a cursor that is already over the window when it opens never
	// "enters" it, which left insideWindow false (and the camera inactive) until the cursor left and came back
	gMouse.insideWindow = glfwGetWindowAttrib(window, GLFW_HOVERED) != 0 ? 1U : 0U;
	gMouse.flags = MouseEvent::kPosition;
	UpdateMouse(&gMouse);
}

void OnScroll(GLFWwindow* /*window*/, double xoffset, double yoffset)
{
	gMouse.xoffset = xoffset;
	gMouse.yoffset = yoffset;
	gMouse.flags = MouseEvent::kScroll;
	UpdateMouse(&gMouse);
}

void OnKey(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	// alt+enter: in or out of fullscreen (see WindowedApplication::RequestToggleFullscreen), once per press
	static bool gFullscreenChangeTriggered = false;
	if (key == GLFW_KEY_ENTER && mods == GLFW_MOD_ALT)
	{
		if (!gFullscreenChangeTriggered)
		{
			gFullscreenChangeTriggered = true;
			if (auto app = App())
				app->RequestToggleFullscreen(reinterpret_cast<WindowHandle>(window));
		}
	}
	else
		gFullscreenChangeTriggered = false;

	gKeyboard.key = ToKey(key);
	gKeyboard.scancode = static_cast<uint32_t>(scancode);
	gKeyboard.action = ToAction(action);
	gKeyboard.mods = ToMods(mods);
	UpdateKeyboard(&gKeyboard);
}

void OnFramebufferResize(GLFWwindow* window, int width, int height)
{
	// 0x0 while minimized, which the application handles by skipping frames until the window is restored
	if (auto app = App())
		app->OnResizeFramebuffer(reinterpret_cast<WindowHandle>(window), width, height);
}

void OnWindowContentScaleChanged(GLFWwindow* window, float xscale, float yscale)
{
	ASSERT(xscale > 0);
	ASSERT(yscale > 0);

	if (auto app = App())
	{
		auto& state = app->GetWindow(reinterpret_cast<WindowHandle>(window)).GetState();
		state.xscale = xscale;
		state.yscale = yscale;
	}
}

} // namespace detail

void Init()
{
	ZoneScopedN("platform::glfw::Init");

	using namespace detail;

	glfwSetErrorCallback(OnError);

	ENSUREF(glfwInit(), "GLFW: Failed to initialize.");
	gInitialized = true;
	ENSUREF(glfwVulkanSupported(), "GLFW: Vulkan not supported.");

	int monitorCount = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
	ENSUREF(monitors != nullptr && monitorCount > 0, "GLFW: No monitor connected?");
	for (int monitorIt = 0; monitorIt < monitorCount; ++monitorIt)
	{
		GLFWmonitor* monitor = monitors[monitorIt];
		const char* name = glfwGetMonitorName(monitor);
		int x = 0;
		int y = 0;
		glfwGetMonitorPos(monitor, &x, &y);
		int physicalWidth = 0;
		int physicalHeight = 0;
		glfwGetMonitorPhysicalSize(monitor, &physicalWidth, &physicalHeight);
		const GLFWvidmode* mode = glfwGetVideoMode(monitor);
		ENSURE(mode != nullptr);
		float xscale = 0.0F;
		float yscale = 0.0F;
		glfwGetMonitorContentScale(monitor, &xscale, &yscale);

		std::println(
			"GLFW: Connected Monitor {}: {}, Position: {}x{}, Physical Size: {}x{}, Video Mode: {}x{}@{}[{}:{}:{}], Content Scale: {:f}x{:f}",
			monitorIt, name != nullptr ? name : "", x, y, physicalWidth, physicalHeight, mode->width, mode->height,
			mode->refreshRate, mode->redBits, mode->greenBits, mode->blueBits, xscale, yscale);
	}
}

void Terminate()
{
	detail::gInitialized = false;
	glfwTerminate();
}

WindowHandle CreateNativeWindow(WindowState& state)
{
	ZoneScopedN("platform::glfw::CreateNativeWindow");

	using namespace detail;

	ENSURE(state.width > 0);
	ENSURE(state.height > 0);

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

	// SPEEDO_BACKGROUND=1 (automated runs, see scripts/assettest.ps1): the window opens behind the others, without taking
	// the focus, so that test runs don't interrupt whoever is working
	const char* backgroundSetting = std::getenv("SPEEDO_BACKGROUND");
	bool background = backgroundSetting != nullptr && std::string_view(backgroundSetting) == "1";
	if (background)
	{
		glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
		glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	}

	// SPEEDO_PRESENTATION_TEST (see the client's PrepareDraw): exclusive fullscreen stays up when the window loses the
	// focus, which an automated run doesn't control (minimized, nothing is drawn, and the run's watchdog took that for a
	// hang)
	if (const char* presentationTest = std::getenv("SPEEDO_PRESENTATION_TEST"); presentationTest != nullptr && *presentationTest != '\0')
		glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);

	GLFWwindow* window = glfwCreateWindow(static_cast<int>(state.width), static_cast<int>(state.height), "", nullptr, nullptr);
	ENSURE(window != nullptr);

	// SPEEDO_FULLSCREEN=exclusive (the default), borderless (see WindowedApplication::ToggleFullscreen: composited, for
	// drivers that flicker scanned out) or native (macOS only, else exclusive): how alt+enter goes fullscreen until View >
	// Presentation picks another
	const char* fullscreenMode = std::getenv("SPEEDO_FULLSCREEN");
	std::string_view fullscreen = fullscreenMode != nullptr ? fullscreenMode : "";
	state.fullscreenMode = FULLSCREEN_MODE_EXCLUSIVE;
	if (fullscreen == "borderless")
		state.fullscreenMode = FULLSCREEN_MODE_BORDERLESS;
#if defined(__APPLE__)
	if (fullscreen == "native")
		state.fullscreenMode = FULLSCREEN_MODE_NATIVE;
#endif

	if (background)
	{
#if defined(__APPLE__)
		// shown behind every other window (glfw's show orders it to the front)
		id nsWindow = glfwGetCocoaWindow(window);
		reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(nsWindow, sel_registerName("orderBack:"), nullptr);
#elif defined(_WIN32)
		// shown at the bottom of the z-order without activating it (glfw's show, SW_SHOWNA, puts it on top)
		SetWindowPos(glfwGetWin32Window(window), HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
#else
		glfwShowWindow(window);
#endif
	}

	glfwGetWindowContentScale(window, &state.xscale, &state.yscale);

	if (glfwRawMouseMotionSupported() != 0)
		glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);

	glfwSetCursorEnterCallback(window, OnMouseEnter);
	glfwSetMouseButtonCallback(window, OnMouseButton);
	glfwSetCursorPosCallback(window, OnMouseCursorPos);
	glfwSetScrollCallback(window, OnScroll);
	glfwSetKeyCallback(window, OnKey);
	glfwSetFramebufferSizeCallback(window, OnFramebufferResize);
	glfwSetWindowContentScaleCallback(window, OnWindowContentScaleChanged);
	glfwSetWindowTitle(window, GetApplicationName());

	return reinterpret_cast<WindowHandle>(window);
}

void DestroyNativeWindow(WindowHandle window)
{
	ENSURE(window != kInvalidWindowHandle);

	glfwDestroyWindow(reinterpret_cast<GLFWwindow*>(window)); //NOLINT(performance-no-int-to-ptr)
}

bool WaitEvents(double timeoutSeconds)
{
	glfwWaitEventsTimeout(timeoutSeconds);

	auto window = GetCurrentWindow();
	return window == kInvalidWindowHandle || glfwWindowShouldClose(reinterpret_cast<GLFWwindow*>(window)) == 0; //NOLINT(performance-no-int-to-ptr)
}

void WakeMain() noexcept
{
	// thread safe, but not before glfw is initialized or after it is terminated (an interrupt can come any time)
	if (detail::gInitialized)
		glfwPostEmptyEvent();
}

} // namespace platform::glfw

void PlatformInit(void)
{
	platform::glfw::Init();
}

void PlatformTerminate(void)
{
	platform::glfw::Terminate();
}

bool PlatformWaitEvents(double timeoutSeconds)
{
	return platform::glfw::WaitEvents(timeoutSeconds);
}

void PlatformWakeMain(void)
{
	platform::glfw::WakeMain();
}

void* PlatformGetWin32Monitor(WindowHandle window)
{
#if defined(_WIN32)
	return MonitorFromWindow(glfwGetWin32Window(reinterpret_cast<GLFWwindow*>(window)), MONITOR_DEFAULTTONEAREST); //NOLINT(performance-no-int-to-ptr)
#else
	(void)window;
	return nullptr;
#endif
}

int32_t PlatformCreateVulkanSurface(WindowHandle window, void* instance, const void* allocator, void* surface)
{
	return static_cast<int32_t>(glfwCreateWindowSurface(
		static_cast<VkInstance>(instance),
		reinterpret_cast<GLFWwindow*>(window), //NOLINT(performance-no-int-to-ptr)
		static_cast<const VkAllocationCallbacks*>(allocator),
		static_cast<VkSurfaceKHR*>(surface)));
}
