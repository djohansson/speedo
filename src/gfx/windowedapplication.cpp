#include <gfx/capi.h>
#include <gfx/windowedapplication.h>

namespace gfx
{

std::mutex WindowedApplication::gDrawMutex{};
core::LoadQueue WindowedApplication::gLoads{};
bool WindowedApplication::gShowAbout = false;
bool WindowedApplication::gShowDemoWindow = false;
bool WindowedApplication::gShowFps = false;
bool WindowedApplication::gShowTps = false;

} // namespace gfx

void ResizeFramebuffer(WindowHandle window, int width, int height)
{
	if (auto app = std::static_pointer_cast<gfx::WindowedApplication>(core::Application::Get()); app)
		app->OnResizeFramebuffer(window, width, height);
}

WindowState* GetWindowState(WindowHandle window)
{
	if (auto app = std::static_pointer_cast<gfx::WindowedApplication>(core::Application::Get()); app)
		return app->GetWindowState(window);

	return nullptr;
}
