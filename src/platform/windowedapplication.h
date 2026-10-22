#pragma once

#include <platform/capi.h>
#include <platform/window.h>

#include <core/application.h>
#include <core/std_extra.h>
#include <core/task.h>

#include <string_view>
#include <vector>

namespace platform
{

inline constexpr uint32_t kDefaultWindowWidth = 1280;
inline constexpr uint32_t kDefaultWindowHeight = 720;

// an application with windows: it makes its window (with the window system, see glfw.h) and runs the main thread's work
// (mainCalls), which the window system and its dialogues need. what is drawn in the windows is the derived class's
// (see client::WindowedApplication).
class WindowedApplication : public core::Application
{
public:
	// destroys the windows
	~WindowedApplication() override;

	// runs the tasks queued for the main thread, and returns whether to go on (until an exit is requested). called by the
	// main loop, between the window system's events
	[[nodiscard]] virtual bool Main();

	// also wakes the main loop, which sleeps in glfwWaitEvents() until the next window event
	void RequestExit() noexcept override;

	// the window's framebuffer has a new size (0x0 when minimized, see Window::IsMinimized). call on the main thread.
	virtual void OnResizeFramebuffer(WindowHandle window, int width, int height);

	// takes the window fullscreen as its state's fullscreenMode says (FULLSCREEN_MODE_*), on the primary monitor, or
	// back to where it was. main thread: the window system's calls are only allowed there, and resize the window before
	// they return (OnResizeFramebuffer)
	void ToggleFullscreen(WindowHandle window);
	// asks for change, which changes how a window is shown (ToggleFullscreen, SetWindowSize, MoveWindowToMonitor), to be
	// called on the main thread: at once here; an application drawing in the window may first need to stop (see
	// client::WindowedApplication::RequestWindowChange). any thread
	virtual void RequestWindowChange(core::std_extra::move_only_function<void()> change);
	// RequestWindowChange of ToggleFullscreen (alt+enter, View > Presentation). any thread
	void RequestToggleFullscreen(WindowHandle window);
	// sets the window's size, in screen coordinates (windowed). main thread
	void SetWindowSize(WindowHandle window, int width, int height);
	// centers the window on a monitor (glfw's order: the primary first), if there is one of that index. main thread
	void MoveWindowToMonitor(WindowHandle window, uint32_t monitorIndex);

	// queues fn for the main thread, and wakes it (it sleeps in glfwWaitEvents until the next window event). any thread
	template <typename Fn>
	void CallOnMain(Fn&& fn)
	{
		auto [task, future] = core::CreateTask(std::forward<Fn>(fn));
		mainCalls.enqueue(task);
		WakeMain();
	}
	static void WakeMain() noexcept;

	[[nodiscard]] const std::vector<Window>& GetWindows() const noexcept { return myWindows; }
	[[nodiscard]] Window& GetWindow(WindowHandle window);
	[[nodiscard]] const Window& GetWindow(WindowHandle window) const;

	// tasks called once on the main thread, by Main (e.g. dialogues, which the window system only allows there)
	mutable core::ConcurrentQueue<core::TaskHandle> mainCalls;

protected:
	// makes the window (see platform::glfw::CreateNativeWindow), the window system initialized (PlatformInit)
	WindowedApplication(std::string_view name, core::Environment&& env);

private:
	std::vector<Window> myWindows;
};

} // namespace platform
