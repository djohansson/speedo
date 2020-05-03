#pragma once

template <>
template <class Archive>
void InstanceConfiguration<GraphicsBackend::Vulkan>::load(Archive& archive)
{
    archive(
        CEREAL_NVP(applicationName),
        CEREAL_NVP(engineName),
        CEREAL_NVP(appInfo)
    );

    appInfo.pApplicationName = applicationName.c_str();
    appInfo.pEngineName = engineName.c_str();
}

template <>
template <class Archive>
void InstanceConfiguration<GraphicsBackend::Vulkan>::save(Archive& archive) const
{
    archive(
        CEREAL_NVP(applicationName),
        CEREAL_NVP(engineName),
        CEREAL_NVP(appInfo)
    );
}
