// the D3D12 backend's RHI (docs/backends-plan.md): the instance, the device and its queues and pipeline. headless until
// the swapchain (step 5): a window fails hard
#include <rhi/d3d12/backend.h>

#include <rhi/rhi.h>
#include <rhi/swapchain.h>

#include <core/assert.h>
#include <core/uuids_extra.h>

#include <atomic>
#include <utility>
#include <vector>

namespace rhi
{

namespace detail
{

static std::atomic<RHI<kD3D12>*> gD3D12RHI = nullptr;

template <>
RHIRegistration<kD3D12>::RHIRegistration(RHI<kD3D12>* rhi) noexcept
{
	[[maybe_unused]] RHI<kD3D12>* expected = nullptr;
	ENSUREF(gD3D12RHI.compare_exchange_strong(expected, rhi), "only one RHI<kD3D12> may exist at a time");
}

template <>
RHIRegistration<kD3D12>::~RHIRegistration()
{
	gD3D12RHI = nullptr;
}

// the device everything draws on: the instance's first adapter (the highest performance one, see Instance)
static std::vector<Device<kD3D12>> DetectAndCreateDevices(Instance<kD3D12>& instance)
{
	const auto& physicalDevices = instance.GetPhysicalDevices();
	ENSUREF(!physicalDevices.empty(), "Failed to find a D3D12 adapter (feature level 12.1; WARP with SPEEDO_D3D12_WARP=1)");

	std::vector<Device<kD3D12>> devices;
	devices.emplace_back(
		DeviceCreateDesc<kD3D12>{
			ObjectCreateDesc<kD3D12>{
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
RHI<kD3D12>* GetRHI<kD3D12>() noexcept
{
	return detail::gD3D12RHI;
}

template <>
RHI<kD3D12>::RHI(RHIInitializationData&& initData, SurfaceHandle<kD3D12> /*surface*/)
	: myInstance(InstanceCreateDesc<kD3D12>{std::string(initData.name), "speedo", initData.window != 0})
	, myDevices{detail::DetectAndCreateDevices(myInstance)}
{
	// device objects find their Device through GetRHI<kD3D12>(), so these come once the devices are registered
	for (auto& device : myDevices)
	{
		device.InternalCreateQueues();
		device.InternalCreatePipeline(std::vector(initData.descriptorPoolSizes));
	}

	ENSUREF(initData.window == 0, "the D3D12 backend has no swapchain yet (docs/backends-plan.md, step 5): run it headless");
}

template <>
RHI<kD3D12>::RHI(RHIInitializationData&& initData)
	: RHI(std::forward<RHIInitializationData>(initData), nullptr)
{}

} // namespace rhi
