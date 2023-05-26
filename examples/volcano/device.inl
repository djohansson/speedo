#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, DeviceConfiguration<B>& config)
{
	archive(cereal::make_nvp("physicalDeviceIndex", config.physicalDeviceIndex));
	//archive(cereal::make_nvp("useShaderFloat16", config.useShaderFloat16));
	//archive(cereal::make_nvp("useDescriptorIndexing", config.useDescriptorIndexing));
	//archive(cereal::make_nvp("useScalarBlockLayout", config.useScalarBlockLayout));
	//archive(cereal::make_nvp("useHostQueryReset", config.useHostQueryReset));
	//archive(cereal::make_nvp("useTimelineSemaphores", config.useTimelineSemaphores));
	//archive(cereal::make_nvp("useBufferDeviceAddress", config.useBufferDeviceAddress));
}
