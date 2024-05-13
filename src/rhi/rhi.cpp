#include "capi.h"
#include "rhiapplication.h"

namespace rhi
{

static FlatSet<WindowHandle> s_windows{};
static std::optional<WindowHandle> s_currentWindow{};

}

void ResizeFramebuffer(WindowHandle window, int w, int h)
{
	using namespace rhi;

	if (auto app = static_pointer_cast<RhiApplication>(Application::Instance().lock()); app)
		app->OnResizeFramebuffer(window, w, h);
}

WindowHandle GetCurrentWindow(void)
{
	using namespace rhi;

	return s_currentWindow.value_or(nullptr);
}

WindowHandle* GetWindows(size_t* count)
{
	using namespace rhi;

	if (s_windows.empty())
	{
		*count = 0;
		return nullptr;
	}

	*count = s_windows.size();
	return s_windows.data();
}

void SetWindows(WindowHandle* windows, size_t count)
{
	using namespace rhi;

	s_windows.clear();

	for (size_t i = 0; i < count; ++i)
		s_windows.emplace(windows[i]);
}

void SetCurrentWindow(WindowHandle window)
{
	using namespace rhi;

	assert(s_windows.find(window) != s_windows.end());

	if (!s_currentWindow.has_value() || s_currentWindow != window)
		s_currentWindow = window;
}

WindowState* GetWindowState(WindowHandle window)
{
	using namespace rhi;

	if (auto app = static_pointer_cast<RhiApplication>(Application::Instance().lock()); app)
		return app->GetWindowState(window);

	return nullptr;
}
