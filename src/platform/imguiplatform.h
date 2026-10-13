#pragma once

#include <platform/capi.h>

#include <mutex>

// imgui's platform side: input, the cursor and the display size from the window system (imgui's glfw backend). the
// imgui context is its user's to create first, and to destroy after Shutdown (see client::WindowedApplication); the
// renderer side is rhi's (rhi::ImGuiRenderer).
namespace platform::imgui
{

// binds the current imgui context to window, installing the backend's window callbacks (after the window's own, which
// they call first, and locked: see NewFrame)
void Init(WindowHandle window);

// starts an imgui frame: polls the window's input into imgui's io and updates its display size and time. returns the
// lock on imgui's input queue, which the window's callbacks fill on the main thread: hold it until ImGui::NewFrame has
// consumed the queue (ui thread)
[[nodiscard]] std::unique_lock<std::mutex> NewFrame();

void Shutdown();

} // namespace platform::imgui
