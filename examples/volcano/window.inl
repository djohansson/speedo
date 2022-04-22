#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, WindowConfiguration<B>& config)
{
	serialize(archive, static_cast<SwapchainConfiguration<B>&>(config));

	archive(cereal::make_nvp("windowExtent", config.windowExtent));
	archive(cereal::make_nvp("splitScreenGrid", config.splitScreenGrid));
}
