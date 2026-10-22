#include "windowedapplication.h"
#include "glfw.h"

#include <core/assert.h>
#include <core/profiling.h>

#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#	define GLFW_EXPOSE_NATIVE_COCOA
#	include <GLFW/glfw3native.h>
#	include <objc/message.h>
#	include <objc/runtime.h>
#endif

#include <algorithm>
#include <memory>
#include <optional>

namespace platform
{

namespace windowedapplication
{

std::optional<WindowHandle> gCurrentWindow;

#if defined(__APPLE__)
// borderless fullscreen (FULLSCREEN_MODE_BORDERLESS): a window covering the monitor, with the menu bar and dock hidden,
// stays composited. exclusive and native fullscreen let macOS scan the swapchain images out directly, which flickered
// (frames shown out of order) with KosmicKrisp from mesa 26.2; with mesa main (8fc4981) all three work, and exclusive is
// the default
void SetMacPresentationOptions(unsigned long options)
{
	id app = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
	reinterpret_cast<void (*)(id, SEL, unsigned long)>(objc_msgSend)(app, sel_registerName("setPresentationOptions:"), options);
}

enum : unsigned long
{
	kMacPresentationDefault = 0,
	kMacPresentationHideDock = 1UL << 1, // NSApplicationPresentationHideDock
	kMacPresentationHideMenuBar = 1UL << 3, // NSApplicationPresentationHideMenuBar
};
#endif

// the monitor whose area holds the point (screen coordinates), else the primary one
GLFWmonitor* MonitorAt(int x, int y)
{
	int monitorCount = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
	for (int monitorIt = 0; monitors != nullptr && monitorIt < monitorCount; monitorIt++)
	{
		int monitorX = 0;
		int monitorY = 0;
		glfwGetMonitorPos(monitors[monitorIt], &monitorX, &monitorY);
		const GLFWvidmode* mode = glfwGetVideoMode(monitors[monitorIt]);
		if (mode != nullptr && x >= monitorX && x < monitorX + mode->width && y >= monitorY && y < monitorY + mode->height)
			return monitors[monitorIt];
	}
	return glfwGetPrimaryMonitor();
}

} // namespace windowedapplication

WindowedApplication::WindowedApplication(std::string_view name, core::Environment&& env)
	: Application(name, std::forward<core::Environment>(env))
{
	WindowState state{.x = 0, .y = 0, .width = kDefaultWindowWidth, .height = kDefaultWindowHeight, .fullscreenEnabled = 0U, .fullscreenMode = FULLSCREEN_MODE_EXCLUSIVE};
	auto handle = glfw::CreateNativeWindow(state);
	ENSURE(handle != kInvalidWindowHandle);
	myWindows.emplace_back(handle, state);

	// the first window is the one the application draws its user interface in
	SetCurrentWindow(handle);
}

WindowedApplication::~WindowedApplication()
{
	// after the derived application released what it drew in them (their surfaces)
	for (const auto& window : myWindows)
		glfw::DestroyNativeWindow(window.GetHandle());
}

bool WindowedApplication::Main()
{
	ZoneScopedN("WindowedApplication::Main");

	core::TaskHandle mainCall;
	while (mainCalls.try_dequeue(mainCall))
		GetExecutor().Call(mainCall);

	return !IsExitRequested();
}

void WindowedApplication::RequestExit() noexcept
{
	Application::RequestExit();
	WakeMain();
}

void WindowedApplication::WakeMain() noexcept
{
	glfw::WakeMain();
}

void WindowedApplication::ToggleFullscreen(WindowHandle handle)
{
	ZoneScopedN("WindowedApplication::ToggleFullscreen");

	using namespace windowedapplication;

	auto& window = GetWindow(handle);
	auto& state = window.GetState();
	auto& placement = window.GetWindowedPlacement();
	auto* glfwWindow = reinterpret_cast<GLFWwindow*>(handle); //NOLINT(performance-no-int-to-ptr)

	// left the way it was entered
	uint32_t fullscreenMode = state.fullscreenEnabled != 0U ? placement.fullscreenMode : state.fullscreenMode;
#if !defined(__APPLE__)
	if (fullscreenMode == FULLSCREEN_MODE_NATIVE)
		fullscreenMode = FULLSCREEN_MODE_EXCLUSIVE;
#endif
	placement.fullscreenMode = fullscreenMode;

#if defined(__APPLE__)
	if (fullscreenMode == FULLSCREEN_MODE_NATIVE)
	{
		// asynchronous: the window resizes as it moves to its space (or back), which the resize callbacks follow
		reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(glfwGetCocoaWindow(glfwWindow), sel_registerName("toggleFullScreen:"), nullptr);
		state.fullscreenEnabled = state.fullscreenEnabled != 0U ? 0U : 1U;
		return;
	}
#endif

	if (state.fullscreenEnabled != 0U)
	{
		if (fullscreenMode == FULLSCREEN_MODE_BORDERLESS)
		{
#if defined(__APPLE__)
			SetMacPresentationOptions(kMacPresentationDefault);
#endif
			glfwSetWindowAttrib(glfwWindow, GLFW_DECORATED, GLFW_TRUE);
		}
		// for the borderless window (which has no monitor) too: it places the window
		glfwSetWindowMonitor(glfwWindow, nullptr, placement.x, placement.y, placement.width, placement.height, GLFW_DONT_CARE);

		state.fullscreenRefresh = 0;
		state.fullscreenEnabled = 0U;
		return;
	}

	glfwGetWindowPos(glfwWindow, &placement.x, &placement.y);
	glfwGetWindowSize(glfwWindow, &placement.width, &placement.height);

	// the monitor the window is on (its center's), else the primary one: always the primary, a second monitor's window
	// went fullscreen on the first, at the first's refresh rate
	GLFWmonitor* monitor = MonitorAt(placement.x + (placement.width / 2), placement.y + (placement.height / 2));
	if (monitor == nullptr)
		return;
	const GLFWvidmode* mode = glfwGetVideoMode(monitor);
	ENSURE(mode != nullptr);

	state.x = 0;
	state.y = 0;
	state.width = static_cast<uint32_t>(mode->width);
	state.height = static_cast<uint32_t>(mode->height);

	if (fullscreenMode == FULLSCREEN_MODE_BORDERLESS)
	{
		// an undecorated window covering the monitor (and, on windows, its taskbar), in the monitor's mode
		int monitorX = 0;
		int monitorY = 0;
		glfwGetMonitorPos(monitor, &monitorX, &monitorY);

#if defined(__APPLE__)
		SetMacPresentationOptions(kMacPresentationHideDock | kMacPresentationHideMenuBar);
#endif
		glfwSetWindowAttrib(glfwWindow, GLFW_DECORATED, GLFW_FALSE);
		glfwSetWindowPos(glfwWindow, monitorX, monitorY);
		glfwSetWindowSize(glfwWindow, mode->width, mode->height);
	}
	else
		// at the monitor's refresh rate, not GLFW_DONT_CARE: glfw picks the mode closest to the requested rate, i.e. the
		// lowest one
		glfwSetWindowMonitor(glfwWindow, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);

	state.fullscreenRefresh = static_cast<uint32_t>(mode->refreshRate);
	state.fullscreenEnabled = 1U;
}

void WindowedApplication::RequestWindowChange(core::std_extra::move_only_function<void()> change)
{
	CallOnMain([change = std::move(change)]() mutable { change(); });
}

void WindowedApplication::RequestToggleFullscreen(WindowHandle window)
{
	RequestWindowChange([this, window] { ToggleFullscreen(window); });
}

void WindowedApplication::SetWindowSize(WindowHandle handle, int width, int height)
{
	glfwSetWindowSize(reinterpret_cast<GLFWwindow*>(handle), width, height); //NOLINT(performance-no-int-to-ptr)
}

void WindowedApplication::MoveWindowToMonitor(WindowHandle handle, uint32_t monitorIndex)
{
	int monitorCount = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
	if (monitors == nullptr || monitorIndex >= static_cast<uint32_t>(monitorCount))
		return;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	glfwGetMonitorWorkarea(monitors[monitorIndex], &x, &y, &width, &height);
	auto* glfwWindow = reinterpret_cast<GLFWwindow*>(handle); //NOLINT(performance-no-int-to-ptr)
	int windowWidth = 0;
	int windowHeight = 0;
	glfwGetWindowSize(glfwWindow, &windowWidth, &windowHeight);
	glfwSetWindowPos(glfwWindow, x + std::max(0, (width - windowWidth) / 2), y + std::max(0, (height - windowHeight) / 2));
}

void WindowedApplication::OnResizeFramebuffer(WindowHandle window, int width, int height)
{
	// minimizing reports 0x0
	GetWindow(window).SetMinimized(width <= 0 || height <= 0);
}

Window& WindowedApplication::GetWindow(WindowHandle window)
{
	return const_cast<Window&>(std::as_const(*this).GetWindow(window));
}

const Window& WindowedApplication::GetWindow(WindowHandle window) const
{
	auto windowIt = std::ranges::find(myWindows, window, &Window::GetHandle);
	ENSUREF(windowIt != myWindows.end(), "Unknown window {}", window);
	return *windowIt;
}

} // namespace platform

WindowHandle GetCurrentWindow(void)
{
	return platform::windowedapplication::gCurrentWindow.value_or(kInvalidWindowHandle);
}

void SetCurrentWindow(WindowHandle window)
{
	if (!platform::windowedapplication::gCurrentWindow.has_value())
		platform::windowedapplication::gCurrentWindow = window;
}


