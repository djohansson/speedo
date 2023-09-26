template <GraphicsBackend B>
void GraphicsApplication<B>::resizeWindow(const WindowState& state)
{
	if (state.fullscreenEnabled)
	{
		gfx().myMainWindow->onResizeWindow({state.fullscreenWidth, state.fullscreenHeight});
	}
	else
	{
		gfx().myMainWindow->onResizeWindow({state.width, state.height});
	}
}

template <GraphicsBackend B>
std::string_view GraphicsApplication<B>::getName() const
{
	return gfx().myInstance->getConfig().applicationName;
}

template <GraphicsBackend B>
GraphicsApplication<B>::GraphicsApplication()
: ApplicationBase()
, myGraphicsContext{std::make_shared<Instance<B>>()}
{
}
