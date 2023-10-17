#pragma once

#include "types.h"

#include <core/utils.h>

#include <any>
#include <string>
#include <tuple>
#include <vector>

template <GraphicsApi G>
struct InstanceConfiguration
{
	std::string applicationName;
	std::string engineName;
	ApplicationInfo<G> appInfo{};
};

template <GraphicsApi G>
struct SwapchainInfo
{
	SurfaceCapabilities<G> capabilities{};
	std::vector<SurfaceFormat<G>> formats;
	std::vector<PresentMode<G>> presentModes;
	std::vector<uint32_t> queueFamilyPresentSupport;
};

template <GraphicsApi G>
struct PhysicalDeviceInfo
{
	PhysicalDeviceProperties<G> deviceProperties{};
	PhysicalDevicePropertiesEx<G> devicePropertiesEx{};
	PhysicalDeviceFeatures<G> deviceFeatures{};
	PhysicalDeviceFeaturesEx<G> deviceFeaturesEx{};
	PhysicalDeviceRobustnessFeatures<G> deviceRobustnessFeatures{};
	std::vector<QueueFamilyProperties<G>> queueFamilyProperties;
};

template <GraphicsApi G>
class Instance final : public Noncopyable, public Nonmovable
{
public:
	Instance(InstanceConfiguration<G>&& defaultConfig = {});
	~Instance();

	operator auto() const noexcept { return myInstance; }

	const auto& getConfig() const noexcept { return myConfig; }
	const auto& getHostAllocationCallbacks() const noexcept { return myHostAllocationCallbacks; }
	const auto& getPhysicalDevices() const noexcept { return myPhysicalDevices; }
	const auto& getPhysicalDeviceInfo(PhysicalDeviceHandle<G> device) const { return myPhysicalDeviceInfos.at(device);	}

	const SwapchainInfo<G>&
	getSwapchainInfo(PhysicalDeviceHandle<G> device, SurfaceHandle<G> surface);

	void updateSurfaceCapabilities(PhysicalDeviceHandle<Vk> device, SurfaceHandle<Vk> surface);

private:
	InstanceConfiguration<G> myConfig{};
	InstanceHandle<G> myInstance{};
	AllocationCallbacks<G> myHostAllocationCallbacks{};
	std::vector<PhysicalDeviceHandle<G>> myPhysicalDevices;
	UnorderedMap<PhysicalDeviceHandle<G>, PhysicalDeviceInfo<G>> myPhysicalDeviceInfos;
	UnorderedMap<std::tuple<PhysicalDeviceHandle<G>, SurfaceHandle<G>>, SwapchainInfo<G>, TupleHash>
		myPhysicalDeviceSwapchainInfos;
	std::any myUserData;
};
