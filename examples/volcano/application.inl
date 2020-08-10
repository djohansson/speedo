template <GraphicsBackend B>
void Application<B>::resizeWindow(const WindowState& state)
{
    if (state.fullscreenEnabled)
    {
        myWindows[0]->onResizeWindow({state.fullscreenWidth, state.fullscreenHeight});
    }
    else
    {
        myWindows[0]->onResizeWindow({state.width, state.height});
    }
}

template <GraphicsBackend B>
const char* Application<B>::getName() const
{
    return myContext->instance->getConfig().applicationName.c_str();
}
