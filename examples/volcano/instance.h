#pragma once

#include "file.h"
#include "types.h"
#include "utils.h"

#include <any>
#include <string>
#include <tuple>
#include <vector>

template <GraphicsBackend B>
struct InstanceConfiguration
{
	InstanceConfiguration();

	std::string applicationName = "";
	std::string engineName = "";
	ApplicationInfo<B> appInfo{};
};

template <GraphicsBackend B>
struct SwapchainInfo
{
	SurfaceCapabilities<B> capabilities{};
	std::vector<SurfaceFormat<B>> formats;
	std::vector<PresentMode<B>> presentModes;
	std::vector<uint32_t> queueFamilyPresentSupport;
};

template <GraphicsBackend B>
struct PhysicalDeviceInfo
{
	PhysicalDeviceProperties<B> deviceProperties{};
	PhysicalDevicePropertiesEx<B> devicePropertiesEx{};
	PhysicalDeviceFeatures<B> deviceFeatures{};
	PhysicalDeviceFeaturesEx<B> deviceFeaturesEx{};
	PhysicalDeviceRobustnessFeatures<B> deviceRobustnessFeatures{};
	std::vector<QueueFamilyProperties<B>> queueFamilyProperties;
};

template <GraphicsBackend B>
class Instance : public Noncopyable
{
public:
	Instance(InstanceConfiguration<B>&& defaultConfig = {});
	~Instance();

	operator auto() const noexcept { return myInstance; }

	const auto& getConfig() const noexcept { return myConfig; }
	const auto& getHostAllocationCallbacks() const noexcept { return myHostAllocationCallbacks; }
	const auto& getPhysicalDevices() const noexcept { return myPhysicalDevices; }
	const auto& getPhysicalDeviceInfo(PhysicalDeviceHandle<B> device) const { return myPhysicalDeviceInfos.at(device);	}

	const SwapchainInfo<B>&
	getSwapchainInfo(PhysicalDeviceHandle<B> device, SurfaceHandle<B> surface);

	void updateSurfaceCapabilities(PhysicalDeviceHandle<Vk> device, SurfaceHandle<Vk> surface);

private:
	AutoSaveJSONFileObject<InstanceConfiguration<B>> myConfig;
	InstanceHandle<B> myInstance{};
	AllocationCallbacks<B> myHostAllocationCallbacks{};
	std::vector<PhysicalDeviceHandle<B>> myPhysicalDevices;
	UnorderedMap<PhysicalDeviceHandle<B>, PhysicalDeviceInfo<B>> myPhysicalDeviceInfos;
	UnorderedMap<std::tuple<PhysicalDeviceHandle<B>, SurfaceHandle<B>>, SwapchainInfo<B>, TupleHash>
		myPhysicalDeviceSwapchainInfos;
	std::any myUserData;
};

#include "instance-vulkan.inl"
#include "instance.inl"
