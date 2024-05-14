#pragma once

#include "types.h"

#include <core/utils.h>

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
	PhysicalDeviceInlineUniformBlockProperties<G> inlineUniformBlockProperties{};
	PhysicalDeviceFeatures<G> deviceFeatures{};
	PhysicalDeviceFeaturesEx<G> deviceFeaturesEx{};
	PhysicalDeviceInlineUniformBlockFeatures<G> inlineUniformBlockFeatures{};
	std::vector<QueueFamilyProperties<G>> queueFamilyProperties;
};

template <GraphicsApi G>
class Instance final : public Noncopyable, public Nonmovable
{
public:
	Instance(InstanceConfiguration<G>&& defaultConfig = {});
	~Instance();

	operator auto() const noexcept { return myInstance; }

	const auto& GetConfig() const noexcept { return myConfig; }
	const auto& GetHostAllocationCallbacks() const noexcept { return myHostAllocationCallbacks; }
	const auto& GetPhysicalDevices() const noexcept { return myPhysicalDevices; }
	const auto& GetPhysicalDeviceInfo(PhysicalDeviceHandle<G> device) const { return *myPhysicalDeviceInfos.at(device);	}

	const SwapchainInfo<G>& UpdateSwapchainInfo(PhysicalDeviceHandle<G> device, SurfaceHandle<G> surface);
	const SwapchainInfo<G>& GetSwapchainInfo(PhysicalDeviceHandle<G> device, SurfaceHandle<G> surface) const;
	void UpdateSurfaceCapabilities(PhysicalDeviceHandle<Vk> device, SurfaceHandle<Vk> surface);

private:
	InstanceConfiguration<G> myConfig{};
	InstanceHandle<G> myInstance{};
	AllocationCallbacks<G> myHostAllocationCallbacks{};
	std::vector<PhysicalDeviceHandle<G>> myPhysicalDevices;
	UnorderedMap<PhysicalDeviceHandle<G>, std::unique_ptr<PhysicalDeviceInfo<G>>> myPhysicalDeviceInfos;
	UnorderedMap<std::tuple<PhysicalDeviceHandle<G>, SurfaceHandle<G>>, SwapchainInfo<G>, TupleHash>
		myPhysicalDeviceSwapchainInfos;
};
