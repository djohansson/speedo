#include <locale>

template <GraphicsBackend B>
void Application<B>::resizeWindow(const WindowState& state)
{
	if (state.fullscreenEnabled)
	{
		myMainWindow->onResizeWindow({state.fullscreenWidth, state.fullscreenHeight});
	}
	else
	{
		myMainWindow->onResizeWindow({state.width, state.height});
	}
}

template <GraphicsBackend B>
const char* Application<B>::getName() const
{
	return myInstance->getConfig().applicationName.c_str();
}

template <GraphicsBackend B>
Application<B>::Application()
	: myInstance(std::make_shared<InstanceContext<B>>())
	, myExecutor(std::max(1u, std::thread::hardware_concurrency() - 1))
{
	std::locale utf8Locale(".UTF8");  	
	std::locale::global(utf8Locale);
}
