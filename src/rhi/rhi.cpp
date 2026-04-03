#include "capi.h"
#include "rhiapplication.h"

#include <optional>

namespace rhi
{

static std::optional<WindowHandle> gCurrentWindow{};

}

void ResizeFramebuffer(WindowHandle window, int width, int height)
{
	using namespace rhi;

	if (auto app = static_pointer_cast<RHIApplication>(gApplication.lock()); app)
		app->OnResizeFramebuffer(window, width, height);
}

WindowHandle GetCurrentWindow(void)
{
	using namespace rhi;

	return gCurrentWindow.value_or(kInvalidWindowHandle);
}

void SetCurrentWindow(WindowHandle window)
{
	using namespace rhi;

	if (!gCurrentWindow.has_value())
		gCurrentWindow = window;
}

WindowState* GetWindowState(WindowHandle window)
{
	using namespace rhi;

	if (auto app = static_pointer_cast<RHIApplication>(gApplication.lock()); app)
		return app->GetWindowState(window);

	return nullptr;
}
