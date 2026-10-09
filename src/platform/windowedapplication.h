#pragma once

#include <platform/capi.h>
#include <platform/window.h>

#include <core/application.h>
#include <core/task.h>

#include <string_view>
#include <vector>

namespace platform
{

inline constexpr uint32_t kDefaultWindowWidth = 1280;
inline constexpr uint32_t kDefaultWindowHeight = 720;

// an application with windows: it makes its window (createWindowFunc, e.g. with glfw) and runs the main thread's work
// (mainCalls), which the window system and its dialogues need. what is drawn in the windows is the derived class's
// (see gfx::WindowedApplication).
class WindowedApplication : public core::Application
{
public:
	~WindowedApplication() override = default;

	// runs the tasks queued for the main thread, and returns whether to go on (until an exit is requested). called by the
	// main loop, between the window system's events
	[[nodiscard]] virtual bool Main();

	// also wakes the main loop, which sleeps in glfwWaitEvents() until the next window event
	void RequestExit() noexcept override;

	// the window's framebuffer has a new size (0x0 when minimized, see Window::IsMinimized). call on the main thread.
	virtual void OnResizeFramebuffer(WindowHandle window, int width, int height);

	[[nodiscard]] const std::vector<Window>& GetWindows() const noexcept { return myWindows; }
	[[nodiscard]] Window& GetWindow(WindowHandle window);
	[[nodiscard]] const Window& GetWindow(WindowHandle window) const;

	// tasks called once on the main thread, by Main (e.g. dialogues, which the window system only allows there)
	mutable core::ConcurrentQueue<core::TaskHandle> mainCalls;

protected:
	WindowedApplication(std::string_view name, core::Environment&& env, CreateWindowFunc createWindowFunc);

private:
	std::vector<Window> myWindows;
};

} // namespace platform
