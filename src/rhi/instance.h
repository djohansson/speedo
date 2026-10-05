#pragma once

#include <core/utils.h>

#include <rhi/capi.h>
#include <rhi/types.h>

#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace rhi
{

template <GraphicsApi G>
struct InstanceCreateDesc
{
	std::string applicationName;
	std::string engineName;
	ApplicationInfo<G> appInfo{};

	InstanceCreateDesc() = default;
	InstanceCreateDesc(std::string&& applicationName, std::string&& engineName) noexcept;
	InstanceCreateDesc(InstanceCreateDesc&& other) noexcept;
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
using PhysicalDeviceFeatureParams = std::variant<
	PhysicalDeviceInlineUniformBlockFeatures<G>,
	PhysicalDeviceDynamicRenderingFeatures<G>,
	PhysicalDeviceSynchronization2Features<G>,
	PhysicalDeviceExtendedDynamicStateFeatures<G>,
	PhysicalDevicePresentIdFeatures<G>,
	PhysicalDevicePresentWaitFeatures<G>,
	PhysicalDeviceMultiviewFeatures<G>,
	PhysicalDeviceSwapchainMaintenance1Features<G>>;

template <GraphicsApi G>
using PhysicalDevicePropertyParams = std::variant<PhysicalDevicePushDescriptorProperties<G>>;

template <GraphicsApi G>
using PhysicalDeviceFeatureParamsSet = core::UnorderedSet<
	PhysicalDeviceFeatureParams<G>,
	core::IntrusiveTypeInfoHash<PhysicalDeviceFeatureParams<G>, StructureType<G>>,
	core::IntrusiveTypeInfoEqualTo<PhysicalDeviceFeatureParams<G>, StructureType<G>>>;

template <GraphicsApi G>
using PhysicalDevicePropertyParamsSet = core::UnorderedSet<
	PhysicalDevicePropertyParams<G>,
	core::IntrusiveTypeInfoHash<PhysicalDevicePropertyParams<G>, StructureType<G>>,
	core::IntrusiveTypeInfoEqualTo<PhysicalDevicePropertyParams<G>, StructureType<G>>>;

template <GraphicsApi G>
struct PhysicalDeviceInfo
{
	PhysicalDeviceFeatures<G> deviceFeatures{};
	PhysicalDeviceFeatures12Ex<G> deviceFeatures12Ex{};
	// PhysicalDeviceFeatures13Ex<G> deviceFeatures13Ex{};
	PhysicalDeviceFeatureParamsSet<G> deviceFeatureParams;
	PhysicalDeviceProperties<G> deviceProperties{};
	PhysicalDeviceProperties12Ex<G> deviceProperties12Ex{};
	// PhysicalDeviceProperties13Ex<G> deviceProperties13Ex{};
	PhysicalDevicePropertyParamsSet<G> devicePropertyParams;
	std::vector<QueueFamilyProperties<G>> queueFamilyProperties;
};

template <GraphicsApi G>
class Instance final
{
public:
	constexpr Instance() = default;
	explicit Instance(InstanceCreateDesc<G>&& desc);
	Instance(const Instance&) = delete;
	Instance(Instance&& other) noexcept = default;
	~Instance();

	[[nodiscard]] Instance& operator=(const Instance&) = delete;
	[[nodiscard]] Instance& operator=(Instance&& other) noexcept = default;

	[[nodiscard]] operator auto() const noexcept { return myInstance; }//NOLINT(google-explicit-constructor)

	[[nodiscard]] const auto& GetConfig() const noexcept { return myConfig; }
	[[nodiscard]] const auto& GetHostAllocationCallbacks() const noexcept {	return myHostAllocationCallbacks; }
	[[nodiscard]] const auto& GetPhysicalDevices() const noexcept { return myPhysicalDevices; }
	[[nodiscard]] const auto& GetPhysicalDeviceInfo(PhysicalDeviceHandle<G> device) const { return *myPhysicalDeviceInfos.at(device);	}

	[[nodiscard]] const SwapchainInfo<G>& UpdateSwapchainInfo(PhysicalDeviceHandle<G> device, SurfaceHandle<G> surface);
	[[nodiscard]] const SwapchainInfo<G>& GetSwapchainInfo(PhysicalDeviceHandle<G> device, SurfaceHandle<G> surface) const;
	void UpdateSurfaceCapabilities(PhysicalDeviceHandle<kVk> device, SurfaceHandle<kVk> surface);

private:
	InstanceCreateDesc<G> myConfig{};
	InstanceHandle<G> myInstance{};
	AllocationCallbacks<G> myHostAllocationCallbacks{};
	std::vector<PhysicalDeviceHandle<G>> myPhysicalDevices;
	core::UnorderedMap<PhysicalDeviceHandle<G>, std::unique_ptr<PhysicalDeviceInfo<G>>> myPhysicalDeviceInfos;
	core::UnorderedMap<std::tuple<PhysicalDeviceHandle<G>, SurfaceHandle<G>>, SwapchainInfo<G>, core::TupleHash>
		myPhysicalDeviceSwapchainInfos;
};

} // namespace rhi
