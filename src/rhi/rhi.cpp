#include <rhi/capi.h>

#include <optional>

namespace rhi
{

static std::optional<WindowHandle> gCurrentWindow{};

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

