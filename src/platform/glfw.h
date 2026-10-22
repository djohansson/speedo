#pragma once

#include <platform/capi.h>

// platform's window system backend (glfw.cpp): for platform's own code, the rest of the code base goes through
// WindowedApplication and capi.h
namespace platform::glfw
{

// glfw, and a list of the monitors printed. main thread, before anything else of platform's, and after
void Init();
void Terminate();

// a window, its state filled in (content scale, fullscreen mode) and its callbacks installed. main thread
[[nodiscard]] WindowHandle CreateNativeWindow(WindowState& state);
void DestroyNativeWindow(WindowHandle window);

// waits for the window system's events (up to timeoutSeconds) and handles them, calling the callbacks. false once the
// current window was asked to close. main thread
[[nodiscard]] bool WaitEvents(double timeoutSeconds);
// wakes WaitEvents up. any thread
void WakeMain() noexcept;

} // namespace platform::glfw
