#include "windowedapplication.h"

#include <core/assert.h>
#include <core/profiling.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <memory>
#include <optional>

namespace platform
{

namespace windowedapplication
{

std::optional<WindowHandle> gCurrentWindow;

} // namespace windowedapplication

WindowedApplication::WindowedApplication(std::string_view name, core::Environment&& env, CreateWindowFunc createWindowFunc)
	: Application(name, std::forward<core::Environment>(env))
{
	ENSURE(createWindowFunc != nullptr);

	WindowState state{.x = 0, .y = 0, .width = kDefaultWindowWidth, .height = kDefaultWindowHeight, .fullscreenEnabled = 0U};
	auto handle = createWindowFunc(&state);
	ENSURE(handle != kInvalidWindowHandle);
	myWindows.emplace_back(handle, state);

	// the first window is the one the application draws its user interface in
	SetCurrentWindow(handle);
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
	glfwPostEmptyEvent(); // thread safe
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

void ResizeFramebuffer(WindowHandle window, int width, int height)
{
	if (auto app = std::static_pointer_cast<platform::WindowedApplication>(core::Application::Get()); app)
		app->OnResizeFramebuffer(window, width, height);
}

WindowState* GetWindowState(WindowHandle window)
{
	if (auto app = std::static_pointer_cast<platform::WindowedApplication>(core::Application::Get()); app)
		return &app->GetWindow(window).GetState();

	return nullptr;
}
