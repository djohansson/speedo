#include <cereal/cereal.hpp>

template <class Archive, GraphicsBackend B>
void load(Archive& archive, InstanceConfiguration<B>& config)
{
    archive(cereal::make_nvp("applicationName", config.applicationName));
    archive(cereal::make_nvp("engineName", config.engineName));
    archive(cereal::make_nvp("appInfo", config.appInfo));

    if constexpr (B == GraphicsBackend::Vulkan)
    {
        config.appInfo.pApplicationName = config.applicationName.c_str();
        config.appInfo.pEngineName = config.engineName.c_str();
    }
}

template <class Archive, GraphicsBackend B>
void save(Archive& archive, const InstanceConfiguration<B>& config)
{
    archive(cereal::make_nvp("applicationName", config.applicationName));
    archive(cereal::make_nvp("engineName", config.engineName));
    archive(cereal::make_nvp("appInfo", config.appInfo));
}
