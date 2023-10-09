template <GraphicsBackend B>
void GraphicsApplication<B>::onResizeWindow(const WindowState& state)
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

