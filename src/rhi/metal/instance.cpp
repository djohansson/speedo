// the Metal backend's instance: Metal has none, so it holds the devices (each a physical device's handle, an MTL::Device
// it retains), the system's default device first
#include <rhi/instance.h>

#include <Metal/Metal.hpp>

#include <algorithm>
#include <memory>
#include <utility>

namespace rhi
{

template <>
InstanceCreateDesc<kMetal>::InstanceCreateDesc(std::string&& applicationName, std::string&& engineName, bool presentation) noexcept
	: applicationName(std::forward<std::string>(applicationName))
	, engineName(std::forward<std::string>(engineName))
	, presentation(presentation)
{}

template <>
InstanceCreateDesc<kMetal>::InstanceCreateDesc(InstanceCreateDesc&& other) noexcept
	: applicationName(std::move(other.applicationName))
	, engineName(std::move(other.engineName))
	, presentation(other.presentation)
{}

template <>
Instance<kMetal>::Instance(InstanceCreateDesc<kMetal>&& desc)
	: myConfig(std::forward<InstanceCreateDesc<kMetal>>(desc))
{
	MTL::Device* defaultDevice = MTL::CreateSystemDefaultDevice(); // retained (a create)
	NS::Array* devices = MTL::CopyAllDevices();
	auto add = [this](MTL::Device* device)
	{
		auto* handle = reinterpret_cast<metal::PhysicalDeviceObject*>(device);
		myPhysicalDevices.push_back(handle);
		myPhysicalDeviceInfos.emplace(handle, std::make_unique<PhysicalDeviceInfo<kMetal>>());
	};
	if (defaultDevice != nullptr)
		add(defaultDevice);
	for (NS::UInteger deviceIt = 0; devices != nullptr && deviceIt < devices->count(); deviceIt++)
	{
		auto* device = devices->object<MTL::Device>(deviceIt);
		if (device != defaultDevice)
			add(device->retain());
	}
	if (devices != nullptr)
		devices->release();
}

template <>
Instance<kMetal>::~Instance()
{
	for (auto* device : myPhysicalDevices)
		reinterpret_cast<MTL::Device*>(device)->release();
}

} // namespace rhi
