// the Metal backend's RHI (docs/backends-plan.md): the instance, the device and its queues and pipeline; headless only so
// far (the swapchain is step 6)
#include <rhi/rhi.h>
#include <rhi/swapchain.h>

#include <core/assert.h>
#include <core/uuids_extra.h>

#include <atomic>
#include <format>
#include <vector>

namespace rhi
{

namespace detail
{

static std::atomic<RHI<kMetal>*> gMetalRHI = nullptr;

template <>
RHIRegistration<kMetal>::RHIRegistration(RHI<kMetal>* rhi) noexcept
{
	[[maybe_unused]] RHI<kMetal>* expected = nullptr;
	ENSUREF(gMetalRHI.compare_exchange_strong(expected, rhi), "only one RHI<kMetal> may exist at a time");
}

template <>
RHIRegistration<kMetal>::~RHIRegistration()
{
	gMetalRHI = nullptr;
}

// the device everything draws on: the instance's first (the system's default device, see Instance)
static std::vector<Device<kMetal>> DetectAndCreateDevices(Instance<kMetal>& instance)
{
	const auto& physicalDevices = instance.GetPhysicalDevices();
	ENSUREF(!physicalDevices.empty(), "Failed to find a Metal device");

	std::vector<Device<kMetal>> devices;
	devices.emplace_back(
		DeviceCreateDesc<kMetal>{
			ObjectCreateDesc<kMetal>{
				.uuid = uuids::NewUuid(),
				.instance = instance,
				.name = "Device 0",
			},
			physicalDevices.front(),
		},
		instance);
	return devices;
}

} // namespace detail

template <>
RHI<kMetal>* GetRHI<kMetal>() noexcept
{
	return detail::gMetalRHI;
}

template <>
RHI<kMetal>::RHI(RHIInitializationData&& initData)
	: myInstance(InstanceCreateDesc<kMetal>{std::string(initData.name), "speedo", initData.window != 0})
	, myDevices{detail::DetectAndCreateDevices(myInstance)}
{
	ENSUREF(initData.window == 0, "the Metal backend presents nothing yet (docs/backends-plan.md, step 6)");

	// device objects find their Device through GetRHI<kMetal>(), so these come once the devices are registered
	for (auto& device : myDevices)
	{
		device.InternalCreateQueues();
		device.InternalCreatePipeline(std::vector(initData.descriptorPoolSizes));
	}
}

} // namespace rhi
