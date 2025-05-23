#include "capi.h"
#include "rhiapplication.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace rhi
{

static std::vector<WindowHandle> gWindows{};
static std::optional<WindowHandle> gCurrentWindow{};

std::unique_ptr<RHIBase> CreateRHI(GraphicsApi api, std::string_view name, CreateWindowFunc createWindowFunc)
{	
	switch (api)
	{
	case kVk:
		return CreateRHI<kVk>(name, createWindowFunc);
	default:
		ENSUREF(false, "Invalid Graphics API");
	}

	return {};
}

}

void ResizeFramebuffer(WindowHandle window, int width, int height)
{
	using namespace rhi;

	if (auto app = static_pointer_cast<RHIApplication>(Application::Get().lock()); app)
		app->OnResizeFramebuffer(window, width, height);
}

WindowHandle GetCurrentWindow(void)
{
	using namespace rhi;

	return gCurrentWindow.value_or(nullptr);
}

WindowHandle* GetWindows(size_t* count)
{
	using namespace rhi;

	if (gWindows.empty())
	{
		*count = 0;
		return nullptr;
	}

	*count = gWindows.size();
	return gWindows.data();
}

void SetWindows(WindowHandle* windows, size_t count)
{
	using namespace rhi;

	gWindows.clear();

	for (size_t i = 0; i < count; ++i)
		gWindows.emplace_back(windows[i]);
}

void SetCurrentWindow(WindowHandle window)
{
	using namespace rhi;

	ASSERT(std::find_if(gWindows.begin(), gWindows.end(), [window](WindowHandle handle) { return handle == window; }) != gWindows.end());

	if (!gCurrentWindow.has_value())
		gCurrentWindow = window;
}

WindowState* GetWindowState(WindowHandle window)
{
	using namespace rhi;

	if (auto app = static_pointer_cast<RHIApplication>(Application::Get().lock()); app)
		return app->GetWindowState(window);

	return nullptr;
}
