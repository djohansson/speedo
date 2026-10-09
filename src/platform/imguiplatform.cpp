#include "imguiplatform.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <GLFW/glfw3.h>

namespace platform::imgui
{

void Init(WindowHandle window)
{
	ImGui_ImplGlfw_InitForOther(reinterpret_cast<GLFWwindow*>(window), true); //NOLINT(performance-no-int-to-ptr)
}

void NewFrame()
{
	ImGui_ImplGlfw_NewFrame();
}

void Shutdown()
{
	ImGui_ImplGlfw_Shutdown();
}

} // namespace platform::imgui
