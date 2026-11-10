#include "imguiplatform.h"
#include "glfw.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <GLFW/glfw3.h>

#include <algorithm>

namespace platform::imgui
{

namespace detail
{

// imgui's input queue (ImGuiIO::Add*Event) is filled by the window's callbacks on the main thread (glfwWaitEvents) and
// consumed by ImGui::NewFrame on the ui thread: an ImVector, without a lock of its own. its callbacks (the backend's)
// take this lock, as does NewFrame until the caller's ImGui::NewFrame is done (an index out of range in
// FindLatestInputEvent was the race)
std::mutex gInputMutex;

// the window's own callbacks (the client's), which ours chain to, and restore at Shutdown
GLFWwindow* gWindow = nullptr;
GLFWwindowfocusfun gWindowFocus = nullptr;
GLFWcursorenterfun gCursorEnter = nullptr;
GLFWcursorposfun gCursorPos = nullptr;
GLFWmousebuttonfun gMouseButton = nullptr;
GLFWscrollfun gScroll = nullptr;
GLFWkeyfun gKey = nullptr;
GLFWcharfun gChar = nullptr;
GLFWmonitorfun gMonitor = nullptr;

// headless (glfw::IsHeadless): not imgui's glfw backend, which has nothing to do without input or a cursor, asks for the
// native window (an error on the null platform), and on Windows hooks the window's procedure, asserting on the null
// platform's window, which has no HWND. NewFrame gives imgui the display's size and the time instead
bool gHeadless = false;
double gHeadlessTime = 0.0;

// the window's own callback first, then the backend's, locked (the window's may take a while: a resize, a fullscreen
// switch, which nothing else waits for behind this lock)
template <typename Own, typename Backend, typename... Args>
void Chain(Own own, Backend backend, Args... args)
{
	if (own != nullptr)
		own(args...);
	std::scoped_lock lock(gInputMutex);
	backend(args...);
}

void WindowFocus(GLFWwindow* window, int focused) { Chain(gWindowFocus, ImGui_ImplGlfw_WindowFocusCallback, window, focused); }
void CursorEnter(GLFWwindow* window, int entered) { Chain(gCursorEnter, ImGui_ImplGlfw_CursorEnterCallback, window, entered); }
void CursorPos(GLFWwindow* window, double xpos, double ypos) { Chain(gCursorPos, ImGui_ImplGlfw_CursorPosCallback, window, xpos, ypos); }
void MouseButton(GLFWwindow* window, int button, int action, int mods)
{
	Chain(gMouseButton, ImGui_ImplGlfw_MouseButtonCallback, window, button, action, mods);
}
void Scroll(GLFWwindow* window, double xoffset, double yoffset) { Chain(gScroll, ImGui_ImplGlfw_ScrollCallback, window, xoffset, yoffset); }
void Key(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	Chain(gKey, ImGui_ImplGlfw_KeyCallback, window, key, scancode, action, mods);
}
void Char(GLFWwindow* window, unsigned int codepoint) { Chain(gChar, ImGui_ImplGlfw_CharCallback, window, codepoint); }
void Monitor(GLFWmonitor* monitor, int event) { Chain(gMonitor, ImGui_ImplGlfw_MonitorCallback, monitor, event); }

} // namespace detail

void Init(WindowHandle window)
{
	using namespace detail;

	auto* glfwWindow = reinterpret_cast<GLFWwindow*>(window); //NOLINT(performance-no-int-to-ptr)
	gWindow = glfwWindow;
	if (glfw::IsHeadless())
	{
		gHeadless = true;
		gHeadlessTime = 0.0;
		ImGui::GetIO().BackendPlatformName = "speedo_headless";
		return;
	}

	// the backend's callbacks installed by us rather than by it, to lock them (see gInputMutex)
	ImGui_ImplGlfw_InitForOther(glfwWindow, false);
	gWindowFocus = glfwSetWindowFocusCallback(glfwWindow, WindowFocus);
	gCursorEnter = glfwSetCursorEnterCallback(glfwWindow, CursorEnter);
	gCursorPos = glfwSetCursorPosCallback(glfwWindow, CursorPos);
	gMouseButton = glfwSetMouseButtonCallback(glfwWindow, MouseButton);
	gScroll = glfwSetScrollCallback(glfwWindow, Scroll);
	gKey = glfwSetKeyCallback(glfwWindow, Key);
	gChar = glfwSetCharCallback(glfwWindow, Char);
	gMonitor = glfwSetMonitorCallback(Monitor);
}

std::unique_lock<std::mutex> NewFrame()
{
	using namespace detail;

	std::unique_lock lock(gInputMutex);
	if (gHeadless)
	{
		// what the backend's NewFrame gives imgui of the window, the rest (input, the cursor) there is none of
		auto& imguiIO = ImGui::GetIO();
		int width = 0;
		int height = 0;
		int framebufferWidth = 0;
		int framebufferHeight = 0;
		glfwGetWindowSize(gWindow, &width, &height);
		glfwGetFramebufferSize(gWindow, &framebufferWidth, &framebufferHeight);
		imguiIO.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
		if (width > 0 && height > 0)
			imguiIO.DisplayFramebufferScale = ImVec2(
				static_cast<float>(framebufferWidth) / static_cast<float>(width),
				static_cast<float>(framebufferHeight) / static_cast<float>(height));
		double now = glfwGetTime();
		constexpr float kFirstDeltaSeconds = 1.0F / 60.0F;
		constexpr float kMinDeltaSeconds = 1e-6F; // imgui asserts on a delta that isn't positive
		imguiIO.DeltaTime =
			gHeadlessTime > 0.0 ? std::max(static_cast<float>(now - gHeadlessTime), kMinDeltaSeconds) : kFirstDeltaSeconds;
		gHeadlessTime = now;
	}
	else
	{
		ImGui_ImplGlfw_NewFrame();
	}
	return lock;
}

void Shutdown()
{
	using namespace detail;

	if (gHeadless)
	{
		gHeadless = false;
		gWindow = nullptr;
		ImGui::GetIO().BackendPlatformName = nullptr;
		return;
	}

	if (gWindow != nullptr)
	{
		glfwSetWindowFocusCallback(gWindow, gWindowFocus);
		glfwSetCursorEnterCallback(gWindow, gCursorEnter);
		glfwSetCursorPosCallback(gWindow, gCursorPos);
		glfwSetMouseButtonCallback(gWindow, gMouseButton);
		glfwSetScrollCallback(gWindow, gScroll);
		glfwSetKeyCallback(gWindow, gKey);
		glfwSetCharCallback(gWindow, gChar);
		glfwSetMonitorCallback(gMonitor);
		gWindow = nullptr;
	}
	ImGui_ImplGlfw_Shutdown();
}

} // namespace platform::imgui
