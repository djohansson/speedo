#include "../device.h"
#include "../rhiapplication.h"

#include "utils.h"

#include <xxhash.h>

#include <algorithm>
#include <format>
#include <list>
#include <iostream>
#include <shared_mutex>
#include <utility>

namespace device
{

#if GRAPHICS_VALIDATION_ENABLED
static PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT{};
#endif

}

template <>
uint64_t Device<Vk>::getTimelineSemaphoreValue() const
{
	uint64_t value;
	VK_CHECK(vkGetSemaphoreCounterValue(myDevice, myTimelineSemaphore, &value));

	return value;
}

template <>
void Device<Vk>::wait(uint64_t timelineValue) const
{
	ZoneScopedN("Device::wait");

	VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	waitInfo.flags = {};
	waitInfo.semaphoreCount = 1;
	waitInfo.pSemaphores = &myTimelineSemaphore;
	waitInfo.pValues = &timelineValue;

	VK_CHECK(vkWaitSemaphores(myDevice, &waitInfo, UINT64_MAX));
}

template <>
void Device<Vk>::waitIdle() const
{
	ZoneScopedN("Device::waitIdle");

	VK_CHECK(vkDeviceWaitIdle(myDevice));
}

template <>
uint64_t Device<Vk>::addTimelineCallback(std::function<void(uint64_t)>&& callback)
{
	ZoneScopedN("Device::addTimelineCallback");

	auto timelineValue = myTimelineValue.load(std::memory_order_relaxed);

	myTimelineCallbacks.enqueue(
		std::make_tuple(timelineValue, std::forward<std::function<void(uint64_t)>>(callback)));

	return timelineValue;
}

template <>
uint64_t Device<Vk>::addTimelineCallback(TimelineCallback&& callback)
{
	ZoneScopedN("Device::addTimelineCallback");

	auto timelineValue = std::get<0>(callback);

	myTimelineCallbacks.enqueue(std::forward<TimelineCallback>(callback));

	return timelineValue;
}

template <>
bool Device<Vk>::processTimelineCallbacks(uint64_t timelineValue)
{
	ZoneScopedN("Device::processTimelineCallbacks");

	TimelineCallback callbackTuple;
	while (myTimelineCallbacks.try_dequeue(callbackTuple))
	{
		const auto& [commandBufferTimelineValue, callback] = callbackTuple;

		if (commandBufferTimelineValue > timelineValue)
		{
			myTimelineCallbacks.enqueue(std::move(callbackTuple));
			return false;
		}

		callback(commandBufferTimelineValue);
	}

	return true;
}

#if GRAPHICS_VALIDATION_ENABLED
template <>
void Device<Vk>::addOwnedObjectHandle(
	const uuids::uuid& ownerId,
	ObjectType<Vk> objectType,
	uint64_t objectHandle,
	std::string&& objectName)
{
	ZoneScopedN("Device::addOwnedObjectHandle");

	if (!objectHandle)
		return;

	uint64_t ownerIdHash = 0ull;

	{
		ZoneScopedN("Device::addOwnedObjectHandle::hash");

		ownerIdHash = XXH3_64bits(&ownerId, sizeof(ownerId));
	}

	{
		auto lock = std::lock_guard(myObjectMutex);

		auto& objectInfos = myOwnerToDeviceObjectInfoMap[ownerIdHash];

		auto& objectInfo = objectInfos.emplace_back(ObjectNameInfo{
			{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			 nullptr,
			 objectType,
			 objectHandle}, std::forward<std::string>(objectName)});
		objectInfo.pObjectName = objectInfo.name.c_str();

		{
			ZoneScopedN("Device::addOwnedObjectHandle::vkSetDebugUtilsObjectNameEXT");

			VK_CHECK(device::vkSetDebugUtilsObjectNameEXT(myDevice, &objectInfo));
		}

		myObjectTypeToCountMap[objectType]++;
	}
}

template <>
void Device<Vk>::eraseOwnedObjectHandle(const uuids::uuid& ownerId, uint64_t objectHandle)
{
	ZoneScopedN("Device::eraseOwnedObjectHandle");

	if (!objectHandle)
		return;

	uint64_t ownerIdHash = 0ull;

	{
		ZoneScopedN("Device::eraseOwnedObjectHandle::hash");

		ownerIdHash = XXH3_64bits(&ownerId, sizeof(ownerId));
	}

	{
		ZoneScopedN("Device::addOwnedObjectHandle::erase");

		auto lock = std::lock_guard(myObjectMutex);

		auto& objectInfos = myOwnerToDeviceObjectInfoMap[ownerIdHash];

		for (auto it = objectInfos.begin(); it != objectInfos.end(); it++)
		{
			if (it->objectHandle == objectHandle)
			{
				myObjectTypeToCountMap[it->objectType]--;
				it = objectInfos.erase(it);
				return;
			}
		}
	}
}

template <>
void Device<Vk>::clearOwnedObjectHandles(const uuids::uuid& ownerId)
{
	ZoneScopedN("Device::clearOwnedObjectHandles");

	uint64_t ownerIdHash = 0ull;

	{
		ZoneScopedN("Device::clearOwnedObjectHandles::hash");

		ownerIdHash = XXH3_64bits(&ownerId, sizeof(ownerId));
	}

	{
		ZoneScopedN("Device::clearOwnedObjectHandles::clear");

		auto lock = std::lock_guard(myObjectMutex);

		auto& objectInfos = myOwnerToDeviceObjectInfoMap[ownerIdHash];

		for (auto it = objectInfos.begin(); it != objectInfos.end(); it++)
			myObjectTypeToCountMap[it->objectType]--;

		objectInfos.clear();
	}
}

template <>
uint32_t Device<Vk>::getTypeCount(ObjectType<Vk> type)
{
	auto lock = std::shared_lock(myObjectMutex);

	return myObjectTypeToCountMap[type];
}
#endif // GRAPHICS_VALIDATION_ENABLED

template <>
Device<Vk>::Device(
	const std::shared_ptr<Instance<Vk>>& instance,
	DeviceConfiguration<Vk>&& defaultConfig)
	: myInstance(instance)
	, myConfig{
		std::get<std::filesystem::path>(Application::instance().lock()->environment().variables["UserProfilePath"]) / "device.json",
		std::forward<DeviceConfiguration<Vk>>(defaultConfig)}
	, myPhysicalDeviceIndex(myConfig.physicalDeviceIndex)
{
	ZoneScopedN("Device()");

	const auto& physicalDeviceInfo = getPhysicalDeviceInfo();

	std::cout << "\"" << physicalDeviceInfo.deviceProperties.properties.deviceName
			  << "\" is selected as primary graphics device" << '\n';

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	queueCreateInfos.reserve(physicalDeviceInfo.queueFamilyProperties.size());
	std::list<std::vector<float>> queuePriorityList;
	for (uint32_t queueFamilyIt = 0ul;
		 queueFamilyIt < physicalDeviceInfo.queueFamilyProperties.size();
		 queueFamilyIt++)
	{
		auto& queuePriorities = queuePriorityList.emplace_back();
		const auto& queueFamilyProperty = physicalDeviceInfo.queueFamilyProperties[queueFamilyIt];
		queuePriorities.resize(queueFamilyProperty.queueCount);
		std::fill(queuePriorities.begin(), queuePriorities.end(), 1.0f);

		std::cout << "Queue Family " << queueFamilyIt << 
			", queueFlags: " << queueFamilyProperty.queueFlags <<
			", queueCount: " << queueFamilyProperty.queueCount << '\n';

		queueCreateInfos.emplace_back(VkDeviceQueueCreateInfo{
			VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			nullptr,
			0, //VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT,
			queueFamilyIt,
			static_cast<uint32_t>(queuePriorities.size()),
			queuePriorities.data()});
	}

	uint32_t deviceExtensionCount;
	vkEnumerateDeviceExtensionProperties(
		getPhysicalDevice(), nullptr, &deviceExtensionCount, nullptr);

	std::vector<VkExtensionProperties> availableDeviceExtensions(deviceExtensionCount);
	vkEnumerateDeviceExtensionProperties(
		getPhysicalDevice(), nullptr, &deviceExtensionCount, availableDeviceExtensions.data());

	std::cout << deviceExtensionCount << " vulkan device extension(s) found:" << '\n';
	std::vector<const char*> deviceExtensions;
	deviceExtensions.reserve(deviceExtensionCount);
	for (uint32_t i = 0ul; i < deviceExtensionCount; i++)
	{
		deviceExtensions.push_back(availableDeviceExtensions[i].extensionName);
		std::cout << deviceExtensions.back() << '\n';
	}

	std::sort(
		deviceExtensions.begin(),
		deviceExtensions.end(),
		[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

	std::vector<const char*> requiredDeviceExtensions = {
		// must be sorted lexicographically for std::includes to work!
		VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME,
#if defined(__OSX__)
		VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
#endif
		//VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
		VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_EXTENSION_NAME};

	assert(std::includes(
		deviceExtensions.begin(),
		deviceExtensions.end(),
		requiredDeviceExtensions.begin(),
		requiredDeviceExtensions.end(),
		[](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }));

	// auto& deviceFeaturesEx = physicalDeviceInfo.deviceFeaturesEx;
	// deviceFeaturesEx.shaderFloat16 = myConfig.useShaderFloat16.value();
	// deviceFeaturesEx.descriptorIndexing = myConfig.useDescriptorIndexing.value();
	// deviceFeaturesEx.scalarBlockLayout = myConfig.useScalarBlockLayout.value();
	// deviceFeaturesEx.hostQueryReset = myConfig.useHostQueryReset.value();
	// deviceFeaturesEx.timelineSemaphore = myConfig.useTimelineSemaphores.value();
	// deviceFeaturesEx.bufferDeviceAddress = myConfig.useBufferDeviceAddress.value();

	VkDeviceCreateInfo deviceCreateInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	deviceCreateInfo.pNext = &physicalDeviceInfo.deviceFeatures;
	deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
	deviceCreateInfo.queueCreateInfoCount = queueCreateInfos.size();
	deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(requiredDeviceExtensions.size());
	deviceCreateInfo.ppEnabledExtensionNames = requiredDeviceExtensions.data();

	VK_CHECK(vkCreateDevice(getPhysicalDevice(), &deviceCreateInfo, &myInstance->getHostAllocationCallbacks(), &myDevice));

#if GRAPHICS_VALIDATION_ENABLED
	device::vkSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
		vkGetDeviceProcAddr(myDevice, "vkSetDebugUtilsObjectNameEXT"));
#endif

	// addOwnedObjectHandle(
	//     getUid(),
	//     VK_OBJECT_TYPE_INSTANCE,
	//     reinterpret_cast<uint64_t>(myInstance->getInstance()),
	//     "Instance");

	// addOwnedObjectHandle(
	//     getUid(),
	//     VK_OBJECT_TYPE_SURFACE_KHR,
	//     reinterpret_cast<uint64_t>(myInstance->getSurface()),
	//     "Instance_Surface");

	// char stringBuffer[256];
	// for (uint32_t physicalDeviceIt = 0ul; physicalDeviceIt < myInstance->getPhysicalDevices().size(); physicalDeviceIt++)
	// {
	//     auto physicalDevice = myInstance->getPhysicalDevices()[physicalDeviceIt];

	//     static constexpr std::string_view physicalDeviceStr = "Instance_PhysicalDevice";

	//     std::format_to_n(
	//         stringBuffer,
	//         std::size(stringBuffer),
	//         "{0}_{1}",
	//         physicalDeviceStr.data(),
	//         physicalDeviceIt);

	//     addOwnedObjectHandle(
	//         getUid(),
	//         VK_OBJECT_TYPE_PHYSICAL_DEVICE,
	//         reinterpret_cast<uint64_t>(physicalDevice),
	//         stringBuffer);
	// }

	// addOwnedObjectHandle(
	//     getUid(),
	//     VK_OBJECT_TYPE_DEVICE,
	//     reinterpret_cast<uint64_t>(myDevice),
	//     "Device");

	myQueueFamilyDescs.resize(physicalDeviceInfo.queueFamilyProperties.size());

	static_assert((uint32_t)QueueFamilyFlagBits_Graphics == (uint32_t)VK_QUEUE_GRAPHICS_BIT);
	static_assert((uint32_t)QueueFamilyFlagBits_Compute == (uint32_t)VK_QUEUE_COMPUTE_BIT);
	static_assert((uint32_t)QueueFamilyFlagBits_Transfer == (uint32_t)VK_QUEUE_TRANSFER_BIT);
	static_assert((uint32_t)QueueFamilyFlagBits_Sparse == (uint32_t)VK_QUEUE_SPARSE_BINDING_BIT);

	for (uint32_t queueFamilyIt = 0ul;
		 queueFamilyIt < physicalDeviceInfo.queueFamilyProperties.size();
		 queueFamilyIt++)
	{
		const auto& queueFamilyProperty = physicalDeviceInfo.queueFamilyProperties[queueFamilyIt];

		auto& queueFamilyDesc = myQueueFamilyDescs[queueFamilyIt];
		queueFamilyDesc.queueCount = queueFamilyProperty.queueCount;
		queueFamilyDesc.flags = queueFamilyProperty.queueFlags;
	}

	myAllocator = [this]
	{
		auto vkGetBufferMemoryRequirements2KHR =
			(PFN_vkGetBufferMemoryRequirements2KHR)vkGetInstanceProcAddr(
				*getInstance(), "vkGetBufferMemoryRequirements2KHR");
		assert(vkGetBufferMemoryRequirements2KHR != nullptr);

		auto vkGetImageMemoryRequirements2KHR =
			(PFN_vkGetImageMemoryRequirements2KHR)vkGetInstanceProcAddr(
				*getInstance(), "vkGetImageMemoryRequirements2KHR");
		assert(vkGetImageMemoryRequirements2KHR != nullptr);

		VmaVulkanFunctions functions{};
		functions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
		functions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
		functions.vkAllocateMemory = vkAllocateMemory;
		functions.vkFreeMemory = vkFreeMemory;
		functions.vkMapMemory = vkMapMemory;
		functions.vkUnmapMemory = vkUnmapMemory;
		functions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
		functions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
		functions.vkBindBufferMemory = vkBindBufferMemory;
		functions.vkBindImageMemory = vkBindImageMemory;
		functions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
		functions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
		functions.vkCreateBuffer = vkCreateBuffer;
		functions.vkDestroyBuffer = vkDestroyBuffer;
		functions.vkCreateImage = vkCreateImage;
		functions.vkDestroyImage = vkDestroyImage;
		functions.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2KHR;
		functions.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2KHR;

		VmaAllocator allocator;
		VmaAllocatorCreateInfo allocatorInfo{};
		allocatorInfo.flags = {};
		allocatorInfo.physicalDevice = getPhysicalDevice();
        allocatorInfo.preferredLargeHeapBlockSize = 0; // 0 = default (256Mb)
		allocatorInfo.device = myDevice;
		allocatorInfo.instance = *getInstance();
        allocatorInfo.pAllocationCallbacks = &getInstance()->getHostAllocationCallbacks();
		allocatorInfo.pVulkanFunctions = &functions;
		vmaCreateAllocator(&allocatorInfo, &allocator);

		return allocator;
	}();

	VkSemaphoreTypeCreateInfo timelineCreateInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	timelineCreateInfo.initialValue = 0ull;

	VkSemaphoreCreateInfo semaphoreCreateInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	semaphoreCreateInfo.pNext = &timelineCreateInfo;
	semaphoreCreateInfo.flags = {};

	VK_CHECK(vkCreateSemaphore(myDevice, &semaphoreCreateInfo, &myInstance->getHostAllocationCallbacks(), &myTimelineSemaphore));

	// addOwnedObjectHandle(
	//     getUid(),
	//     VK_OBJECT_TYPE_SEMAPHORE,
	//     reinterpret_cast<uint64_t>(myTimelineSemaphore),
	//     "Device_TimelineSemaphore");
}

template <>
Device<Vk>::~Device()
{
	ZoneScopedN("~Device()");

	// it is the applications responsibility to wait for all queues complete gpu execution before destroying the Device.
	processTimelineCallbacks(~0ull); // call all timline callbacks

	vkDestroySemaphore(myDevice, myTimelineSemaphore, &myInstance->getHostAllocationCallbacks());

#if PROFILING_ENABLED
	{
		char* allocatorStatsJSON = nullptr;
		vmaBuildStatsString(myAllocator, &allocatorStatsJSON, true);
		std::cout << allocatorStatsJSON << std::endl;
		vmaFreeStatsString(myAllocator, allocatorStatsJSON);
	}
#endif

	vmaDestroyAllocator(myAllocator);
	vkDestroyDevice(myDevice, &myInstance->getHostAllocationCallbacks());
}

template <>
DeviceObject<Vk>::DeviceObject(DeviceObject&& other) noexcept
	: myDevice(std::exchange(other.myDevice, {}))
	, myDesc(std::exchange(other.myDesc, {}))
	, myUid(std::exchange(other.myUid, {}))
{}

template <>
DeviceObject<Vk>::DeviceObject(
	const std::shared_ptr<Device<Vk>>& device, DeviceObjectCreateDesc&& desc)
	: myDevice(device)
	, myDesc(std::forward<DeviceObjectCreateDesc>(desc))
	, myUid(uuids::uuid_system_generator{}())
{}

template <>
DeviceObject<Vk>::DeviceObject(
	const std::shared_ptr<Device<Vk>>& device,
	DeviceObjectCreateDesc&& desc,
	uint32_t objectCount,
	ObjectType<Vk> objectType,
	const uint64_t* objectHandles)
	: DeviceObject(device, std::forward<DeviceObjectCreateDesc>(desc))
{
#if GRAPHICS_VALIDATION_ENABLED
		for (uint32_t objectIt = 0; objectIt < objectCount; objectIt++)
			device->addOwnedObjectHandle(
				getUid(), objectType, objectHandles[objectIt], std::format("{0}{1}", getName(), objectIt));
#endif
}

template <>
DeviceObject<Vk>::~DeviceObject()
{
#if GRAPHICS_VALIDATION_ENABLED
	{
		if (myDevice)
			myDevice->clearOwnedObjectHandles(getUid());
	}
#endif
}

template <>
DeviceObject<Vk>& DeviceObject<Vk>::operator=(DeviceObject&& other) noexcept
{
	myDevice = std::exchange(other.myDevice, {});
	myDesc = std::exchange(other.myDesc, {});
	myUid = std::exchange(other.myUid, {});
	return *this;
}

template <>
void DeviceObject<Vk>::swap(DeviceObject& rhs) noexcept
{
	std::swap(myDevice, rhs.myDevice);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myUid, rhs.myUid);
}
