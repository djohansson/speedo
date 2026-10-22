#pragma once

#include <platform/capi.h>

#include <atomic>

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

	// true while the framebuffer has no area (minimized): there is nothing to draw to. set on the main thread, read on
	// the draw thread
	[[nodiscard]] bool IsMinimized() const noexcept { return myMinimized.load(std::memory_order_relaxed); }
	void SetMinimized(bool minimized) noexcept { myMinimized.store(minimized, std::memory_order_relaxed); }

	// where the window was before it went fullscreen, to go back to, and how it went (FULLSCREEN_MODE_*), to leave the same
	// way (the state's mode may have changed since). main thread
	struct WindowedPlacement
	{
		int x = 0;
		int y = 0;
		int width = 0;
		int height = 0;
		uint32_t fullscreenMode = FULLSCREEN_MODE_EXCLUSIVE;
	};
	[[nodiscard]] WindowedPlacement& GetWindowedPlacement() noexcept { return myWindowedPlacement; }

	Window(Window&& other) noexcept
		: myHandle(other.myHandle)
		, myState(other.myState)
		, myMinimized(other.myMinimized.load())
		, myWindowedPlacement(other.myWindowedPlacement)
	{}

private:
	WindowHandle myHandle = kInvalidWindowHandle;
	WindowState myState{};
	std::atomic_bool myMinimized = false;
	WindowedPlacement myWindowedPlacement;
};

} // namespace platform
