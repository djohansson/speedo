#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, SwapchainConfiguration<B>& config)
{
	archive(cereal::make_nvp("extent", config.extent));
	archive(cereal::make_nvp("surfaceFormat", config.surfaceFormat));
	archive(cereal::make_nvp("presentMode", config.presentMode));
	archive(cereal::make_nvp("imageCount", config.imageCount));
}