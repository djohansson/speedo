#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, SwapchainConfiguration<B>& config)
{
    archive(cereal::make_nvp("extent", config.extent));
    archive(cereal::make_nvp("surfaceFormat", config.surfaceFormat));
    archive(cereal::make_nvp("presentMode", config.presentMode));
    archive(cereal::make_nvp("imageCount", config.imageCount));
}