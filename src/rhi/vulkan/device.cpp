#include "../device.h"
#include "../rhiapplication.h"

#include "utils.h"

#include <vector>
#include <xxhash.h>

#include <algorithm>
#include <format>
#include <list>
#include <iostream>
#include <shared_mutex>
#include <utility>


template <>
void Device<kVk>::WaitIdle() const
{
	ZoneScopedN("Device::waitIdle");

	VK_ENSURE(vkDeviceWaitIdle(myDevice));
}

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
template <>
void Device<kVk>::AddOwnedObjectHandle(
	const uuids::uuid& ownerId,
	ObjectType<kVk> objectType,
	uint64_t objectHandle,
	std::string&& objectName)
{
	ZoneScopedN("Device::AddOwnedObjectHandle");

	if (objectHandle == 0U)
		return;

	uint64_t ownerIdHash = 0ULL;

	{
		ZoneScopedN("Device::AddOwnedObjectHandle::hash");

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
			ZoneScopedN("Device::AddOwnedObjectHandle::vkSetDebugUtilsObjectNameEXT");

			VK_ENSURE(gVkSetDebugUtilsObjectNameExt(myDevice, &objectInfo));
		}

		myObjectTypeToCountMap[objectType]++;
	}
}

template <>
void Device<kVk>::EraseOwnedObjectHandle(const uuids::uuid& ownerId, uint64_t objectHandle)
{
	ZoneScopedN("Device::EraseOwnedObjectHandle");

	if (objectHandle == 0U)
		return;

	uint64_t ownerIdHash = 0ULL;

	{
		ZoneScopedN("Device::EraseOwnedObjectHandle::hash");

		ownerIdHash = XXH3_64bits(&ownerId, sizeof(ownerId));
	}

	{
		ZoneScopedN("Device::AddOwnedObjectHandle::erase");

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
void Device<kVk>::ClearOwnedObjectHandles(const uuids::uuid& ownerId)
{
	ZoneScopedN("Device::ClearOwnedObjectHandles");

	uint64_t ownerIdHash = 0ULL;

	{
		ZoneScopedN("Device::ClearOwnedObjectHandles::hash");

		ownerIdHash = XXH3_64bits(&ownerId, sizeof(ownerId));
	}

	{
		ZoneScopedN("Device::ClearOwnedObjectHandles::clear");

		auto lock = std::lock_guard(myObjectMutex);

		auto& objectInfos = myOwnerToDeviceObjectInfoMap[ownerIdHash];

		for (auto& objectInfo : objectInfos)
			myObjectTypeToCountMap[objectInfo.objectType]--;

		objectInfos.clear();
	}
}

template <>
uint32_t Device<kVk>::GetTypeCount(ObjectType<kVk> type)
{
	auto lock = std::shared_lock(myObjectMutex);

	return myObjectTypeToCountMap[type];
}
#endif // SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0

template <>
Device<kVk>::Device(
	const std::shared_ptr<Instance<kVk>>& instance,
	DeviceConfiguration<kVk>&& defaultConfig)
	: myInstance(instance)
	, myConfig{
		std::get<std::filesystem::path>(Application::Get().lock()->GetEnv().variables["UserProfilePath"]) / "device.json",
		std::forward<DeviceConfiguration<kVk>>(defaultConfig)}
	, myPhysicalDeviceIndex(myConfig.physicalDeviceIndex)
{
	ZoneScopedN("Device()");

	const auto& physicalDeviceInfo = GetPhysicalDeviceInfo();
	
	if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		std::cout << "\"" << physicalDeviceInfo.deviceProperties.properties.deviceName
				  << "\" is selected as primary graphics device" << '\n';

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	queueCreateInfos.reserve(physicalDeviceInfo.queueFamilyProperties.size());
	std::list<std::vector<float>> queuePriorityList;
	for (uint32_t queueFamilyIt = 0UL;
		 queueFamilyIt < physicalDeviceInfo.queueFamilyProperties.size();
		 queueFamilyIt++)
	{
		auto& queuePriorities = queuePriorityList.emplace_back();
		const auto& queueFamilyProperty = physicalDeviceInfo.queueFamilyProperties[queueFamilyIt];
		queuePriorities.resize(queueFamilyProperty.queueCount);
		std::fill(queuePriorities.begin(), queuePriorities.end(), 1.0F);

		if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
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

	std::vector<const char*> requiredExtensions = {
		VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME,
		VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
#if defined(__OSX__)
		VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
#endif
		VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME};

	for (const char* extensionName : requiredExtensions)
		ENSUREF(SupportsExtension(extensionName, GetPhysicalDevice()), "Vulkan device extension not supported: {}", extensionName);

	std::vector<const char*> desiredExtensions = requiredExtensions;

	if (SupportsExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, GetPhysicalDevice()))
		desiredExtensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);

	if (SupportsExtension(VK_KHR_PRESENT_ID_EXTENSION_NAME, GetPhysicalDevice()))
		desiredExtensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);

	if (SupportsExtension(VK_KHR_PRESENT_WAIT_EXTENSION_NAME, GetPhysicalDevice()))
		desiredExtensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
	
	VkDeviceCreateInfo deviceCreateInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	deviceCreateInfo.pNext = &physicalDeviceInfo.deviceFeatures;
	deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
	deviceCreateInfo.queueCreateInfoCount = queueCreateInfos.size();
	deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(desiredExtensions.size());
	deviceCreateInfo.ppEnabledExtensionNames = desiredExtensions.data();

	VK_ENSURE(vkCreateDevice(GetPhysicalDevice(), &deviceCreateInfo, &myInstance->GetHostAllocationCallbacks(), &myDevice));

	InitDeviceExtensions(myDevice);

	// AddOwnedObjectHandle(
	//     GetUuid(),
	//     VK_OBJECT_TYPE_INSTANCE,
	//     reinterpret_cast<uint64_t>(myInstance->GetInstance()),
	//     "Instance");

	// AddOwnedObjectHandle(
	//     GetUuid(),
	//     VK_OBJECT_TYPE_SURFACE_KHR,
	//     reinterpret_cast<uint64_t>(myInstance->GetSurface()),
	//     "Instance_Surface");

	// char stringBuffer[256];
	// for (uint32_t physicalDeviceIt = 0ul; physicalDeviceIt < myInstance->GetPhysicalDevices().size(); physicalDeviceIt++)
	// {
	//     auto physicalDevice = myInstance->GetPhysicalDevices()[physicalDeviceIt];

	//     static constexpr std::string_view physicalDeviceStr = "Instance_PhysicalDevice";

	//     std::format_to_n(
	//         stringBuffer,
	//         std::size(stringBuffer),
	//         "{0}_{1}",
	//         physicalDeviceStr.data(),
	//         physicalDeviceIt);

	//     AddOwnedObjectHandle(
	//         GetUuid(),
	//         VK_OBJECT_TYPE_PHYSICAL_DEVICE,
	//         reinterpret_cast<uint64_t>(physicalDevice),
	//         stringBuffer);
	// }

	// AddOwnedObjectHandle(
	//     GetUuid(),
	//     VK_OBJECT_TYPE_DEVICE,
	//     reinterpret_cast<uint64_t>(myDevice),
	//     "Device");

	ENSURE(physicalDeviceInfo.queueFamilyProperties.size() > 0);

	myQueueFamilyDescs.resize(physicalDeviceInfo.queueFamilyProperties.size());

	for (uint32_t queueFamilyIt = 0UL;
		 queueFamilyIt < physicalDeviceInfo.queueFamilyProperties.size();
		 queueFamilyIt++)
	{
		const auto& queueFamilyProperty = physicalDeviceInfo.queueFamilyProperties[queueFamilyIt];

		auto& queueFamilyDesc = myQueueFamilyDescs[queueFamilyIt];
		queueFamilyDesc.queueCount = queueFamilyProperty.queueCount;
		queueFamilyDesc.flags = queueFamilyProperty.queueFlags;
	}

	// // merge queue families if they have the same flags.
	// // only platform qf:s with same flags has been observed on is osx using moltenvk.
	// // unclear what the spec says about this.
	// for (auto prevIt = myQueueFamilyDescs.begin(), qfIt = std::next(prevIt); qfIt != myQueueFamilyDescs.end(); qfIt++)
	// {
	// 	if (prevIt->flags == qfIt->flags)
	// 	{
	// 		qfIt->queueCount += prevIt->queueCount;
	// 		qfIt = myQueueFamilyDescs.erase(prevIt);
	// 	}
	// }

	myAllocator = [this]
	{
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
		functions.vkGetBufferMemoryRequirements2KHR = gVkGetBufferMemoryRequirements2KHR;
		functions.vkGetImageMemoryRequirements2KHR = gVkGetImageMemoryRequirements2KHR;

		VmaAllocator allocator;
		VmaAllocatorCreateInfo allocatorInfo{};
		allocatorInfo.flags = {};
		allocatorInfo.physicalDevice = GetPhysicalDevice();
        allocatorInfo.preferredLargeHeapBlockSize = 0; // 0 = default (256Mb)
		allocatorInfo.device = myDevice;
		allocatorInfo.instance = *GetInstance();
        allocatorInfo.pAllocationCallbacks = &GetInstance()->GetHostAllocationCallbacks();
		allocatorInfo.pVulkanFunctions = &functions;
		vmaCreateAllocator(&allocatorInfo, &allocator);

		return allocator;
	}();
}

template <>
Device<kVk>::~Device()
{
	ZoneScopedN("~Device()");

	// it is the applications responsibility to wait and destroy all queues complete gpu execution before destroying the Device.

	if constexpr(SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		char* allocatorStatsJSON = nullptr;
		vmaBuildStatsString(myAllocator, &allocatorStatsJSON, 1U);
		std::cout << allocatorStatsJSON << std::endl;
		vmaFreeStatsString(myAllocator, allocatorStatsJSON);
	}

	vmaDestroyAllocator(myAllocator);
	vkDestroyDevice(myDevice, &myInstance->GetHostAllocationCallbacks());
}

template <>
DeviceObject<kVk>::DeviceObject(DeviceObject&& other) noexcept
	: myDevice(std::exchange(other.myDevice, {}))
	, myDesc(std::exchange(other.myDesc, {}))
	, myUuid(std::exchange(other.myUuid, {}))
{}

template <>
DeviceObject<kVk>::DeviceObject(
	const std::shared_ptr<Device<kVk>>& device, DeviceObjectCreateDesc&& desc, uuids::uuid&& uuid)
	: myDevice(device)
	, myDesc(std::forward<DeviceObjectCreateDesc>(desc))
	, myUuid(std::forward<uuids::uuid>(uuid))
{}

template <>
DeviceObject<kVk>::DeviceObject(
	const std::shared_ptr<Device<kVk>>& device,
	DeviceObjectCreateDesc&& desc,
	uint32_t objectCount,
	ObjectType<kVk> objectType,
	const uint64_t* objectHandles,
	uuids::uuid&& uuid)
	: DeviceObject(device, std::forward<DeviceObjectCreateDesc>(desc), std::forward<uuids::uuid>(uuid))
{
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		for (uint32_t objectIt = 0; objectIt < objectCount; objectIt++)
			device->AddOwnedObjectHandle(
				GetUuid(),
				objectType,
				objectHandles[objectIt],
				std::format("{}{}", GetName(), objectIt));
	}
#endif
}

template <>
DeviceObject<kVk>::~DeviceObject()
{
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		if (myDevice)
			myDevice->ClearOwnedObjectHandles(GetUuid());
	}
#endif
}

template <>
DeviceObject<kVk>& DeviceObject<kVk>::operator=(DeviceObject&& other) noexcept
{
	myDevice = std::exchange(other.myDevice, {});
	myDesc = std::exchange(other.myDesc, {});
	myUuid = std::exchange(other.myUuid, {});
	return *this;
}

template <>
void DeviceObject<kVk>::Swap(DeviceObject& rhs) noexcept
{
	std::swap(myDevice, rhs.myDevice);
	std::swap(myDesc, rhs.myDesc);
	std::swap(myUuid, rhs.myUuid);
}
