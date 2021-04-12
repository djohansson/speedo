#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, WindowConfiguration<B>& config)
{
    serialize(archive, static_cast<SwapchainConfiguration<B>&>(config));
    
    archive(cereal::make_nvp("windowExtent", config.windowExtent));
    archive(cereal::make_nvp("splitScreenGrid", config.splitScreenGrid));
}
