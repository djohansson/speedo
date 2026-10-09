#pragma once

#include <platform/capi.h>

// imgui's platform side: input, the cursor and the display size from the window system (imgui's glfw backend). the
// imgui context is its user's to create first, and to destroy after Shutdown (see gfx::WindowedApplication); the
// renderer side is rhi's (rhi::ImGuiRenderer).
namespace platform::imgui
{

// binds the current imgui context to window, installing the backend's window callbacks (chained to the window's own)
void Init(WindowHandle window);

// starts an imgui frame: polls the window's input into imgui's io and updates its display size and time
void NewFrame();

void Shutdown();

} // namespace platform::imgui
