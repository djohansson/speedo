#pragma once

template <>
template <class Archive>
void DeviceCreateDesc<GraphicsBackend::Vulkan>::serialize(Archive& archive)
{
    archive(
        CEREAL_NVP(physicalDeviceIndex),
        CEREAL_NVP(useCommandPoolReset),
        CEREAL_NVP(useTimelineSemaphores)
    );
}
