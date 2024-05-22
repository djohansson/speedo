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
class Instance final
{
public:
	explicit Instance(InstanceConfiguration<G>&& defaultConfig = {});
	Instance(const Instance&) = delete;
	Instance(Instance&& other) noexcept = delete;
	~Instance();

	[[nodiscard]] Instance& operator=(const Instance&) = delete;
	[[nodiscard]] Instance& operator=(Instance&& other) noexcept = delete;

	[[nodiscard]] operator auto() const noexcept { return myInstance; }//NOLINT(google-explicit-constructor)

	[[nodiscard]] const auto& GetConfig() const noexcept { return myConfig; }
	[[nodiscard]] const auto& GetHostAllocationCallbacks() const noexcept
	{
		return myHostAllocationCallbacks;
	}
	[[nodiscard]] const auto& GetPhysicalDevices() const noexcept { return myPhysicalDevices; }
	[[nodiscard]] const auto& GetPhysicalDeviceInfo(PhysicalDeviceHandle<G> device) const { return *myPhysicalDeviceInfos.at(device);	}

	const SwapchainInfo<G>& UpdateSwapchainInfo(PhysicalDeviceHandle<G> device, SurfaceHandle<G> surface);
	[[nodiscard]] const SwapchainInfo<G>& GetSwapchainInfo(PhysicalDeviceHandle<G> device, SurfaceHandle<G> surface) const;
	void UpdateSurfaceCapabilities(PhysicalDeviceHandle<kVk> device, SurfaceHandle<kVk> surface);

private:
	InstanceConfiguration<G> myConfig{};
	InstanceHandle<G> myInstance{};
	AllocationCallbacks<G> myHostAllocationCallbacks{};
	std::vector<PhysicalDeviceHandle<G>> myPhysicalDevices;
	UnorderedMap<PhysicalDeviceHandle<G>, std::unique_ptr<PhysicalDeviceInfo<G>>> myPhysicalDeviceInfos;
	UnorderedMap<std::tuple<PhysicalDeviceHandle<G>, SurfaceHandle<G>>, SwapchainInfo<G>, TupleHash>
		myPhysicalDeviceSwapchainInfos;
};
