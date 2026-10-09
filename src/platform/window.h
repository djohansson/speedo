#pragma once

#include <platform/capi.h>

namespace platform
{

// a native window (made by the application's CreateWindowFunc, e.g. glfw's): its handle, and its state as the window
// system reports it. what is drawn in it (its surface and swapchain) is rhi's.
class Window final
{
public:
	Window(WindowHandle handle, const WindowState& state) noexcept : myHandle(handle), myState(state) {}

	[[nodiscard]] WindowHandle GetHandle() const noexcept { return myHandle; }
	[[nodiscard]] operator WindowHandle() const noexcept { return myHandle; } //NOLINT(google-explicit-constructor)

	[[nodiscard]] WindowState& GetState() noexcept { return myState; }
	[[nodiscard]] const WindowState& GetState() const noexcept { return myState; }

	// true while the framebuffer has no area (minimized): there is nothing to draw to
	[[nodiscard]] bool IsMinimized() const noexcept { return myMinimized; }
	void SetMinimized(bool minimized) noexcept { myMinimized = minimized; }

private:
	WindowHandle myHandle = kInvalidWindowHandle;
	WindowState myState{};
	bool myMinimized = false;
};

} // namespace platform
