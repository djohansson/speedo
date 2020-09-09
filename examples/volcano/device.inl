#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/types/optional.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, SwapchainConfiguration<B>& config)
{
    archive(cereal::make_nvp("format", config.surfaceFormat.format));
    archive(cereal::make_nvp("colorSpace", config.surfaceFormat.colorSpace));
    archive(cereal::make_nvp("presentMode", config.presentMode));
    archive(cereal::make_nvp("imageCount", config.imageCount));
}

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, DeviceConfiguration<B>& config)
{
    archive(cereal::make_nvp("swapchainConfig", config.swapchainConfig));
    //archive(cereal::make_nvp("useShaderFloat16", config.useShaderFloat16));
    //archive(cereal::make_nvp("useDescriptorIndexing", config.useDescriptorIndexing));
    //archive(cereal::make_nvp("useScalarBlockLayout", config.useScalarBlockLayout));
    //archive(cereal::make_nvp("useHostQueryReset", config.useHostQueryReset));
    //archive(cereal::make_nvp("useTimelineSemaphores", config.useTimelineSemaphores));
    //archive(cereal::make_nvp("useBufferDeviceAddress", config.useBufferDeviceAddress));
}
