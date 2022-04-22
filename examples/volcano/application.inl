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
