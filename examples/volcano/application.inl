#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, ApplicationConfiguration<B>& config)
{
    archive(
        cereal::make_nvp("mainWindowSplitScreenGrid.width", config.mainWindowSplitScreenGrid.width),
        cereal::make_nvp("mainWindowSplitScreenGrid.height", config.mainWindowSplitScreenGrid.height),
        cereal::make_nvp("maxCommandContextPerFrameCount", config.maxCommandContextPerFrameCount)
    );
}

template <GraphicsBackend B>
void Application<B>::resizeWindow(const WindowState& state)
{
    if (state.fullscreenEnabled)
    {
        myWindow->onResizeWindow({state.fullscreenWidth, state.fullscreenHeight});
    }
    else
    {
        myWindow->onResizeWindow({state.width, state.height});
    }
}

template <GraphicsBackend B>
const char* Application<B>::getName() const
{
    return myInstance->getConfig().applicationName.c_str();
}
