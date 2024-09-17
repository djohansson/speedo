#include "capi.h"
#include "rhiapplication.h"

namespace rhi
{

static FlatSet<WindowHandle> gWindows{};
static std::optional<WindowHandle> gCurrentWindow{};

std::unique_ptr<IRhi> CreateRhi(GraphicsApi api, std::string_view name, CreateWindowFunc createWindowFunc)
{
	using namespace rhi;

	std::unique_ptr<IRhi> rhi;
	
	switch (api)
	{
	case kVk:
		rhi = std::make_unique<Rhi<kVk>>();
		detail::ConstructRhi(static_cast<Rhi<kVk>&>(*rhi), name, createWindowFunc);
		break;
	default:
		CHECK(false);
		break;
	}

	return rhi;
}

}

void ResizeFramebuffer(WindowHandle window, int width, int height)
{
	using namespace rhi;

	if (auto app = static_pointer_cast<RhiApplication>(Application::Instance().lock()); app)
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
		gWindows.emplace(windows[i]);
}

void SetCurrentWindow(WindowHandle window)
{
	using namespace rhi;

	ASSERT(gWindows.find(window) != gWindows.end());

	if (!gCurrentWindow.has_value() || gCurrentWindow != window)
		gCurrentWindow = window;
}

WindowState* GetWindowState(WindowHandle window)
{
	using namespace rhi;

	if (auto app = static_pointer_cast<RhiApplication>(Application::Instance().lock()); app)
		return app->GetWindowState(window);

	return nullptr;
}
