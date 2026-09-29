#include <rhi/device.h>
#include <rhi/rhiapplication.h>
#include <rhi/vulkan/utils.h>

#include <core/std_extra.h>

#include <list>
#include <iostream>
#include <vector>

#include <xxhash.h>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Device<kVk>);

template <>
void Device<kVk>::WaitIdle() const
{
	ZoneScopedN("Device::waitIdle");

	VK_CHECK(vkDeviceWaitIdle(myDevice));
}

template <>
bool Device<kVk>::SupportsFeature(StructureType<kVk> feature, const Instance<kVk>& instance) const
{
	const auto& physicalDeviceInfo = instance.GetPhysicalDeviceInfo(GetPhysicalDevice());
	const auto& featureIt = physicalDeviceInfo.deviceFeatureParams.find(feature);
	bool supported = false;
	if (featureIt != physicalDeviceInfo.deviceFeatureParams.end())
		std::visit(
			core::std_extra::overloaded
			{
				[&supported](const auto& featureVariant) {},
				[&supported](const SwapchainMaintenance1Features<kVk>& featureVariant) { supported = featureVariant.swapchainMaintenance1; }
			},
			*featureIt);
	return supported;
}

template <>
void Device<kVk>::InternalCreateQueues()
{
	ZoneScopedN("Device::InternalCreateQueues");

	auto& queues = GetQueues();

	queues.emplace(
		kQueueTypeGraphics,
		std::make_shared<QueueTimelineContext<kVk>>(std::make_shared<QueueTimelineContextData<kVk>>(
			Semaphore<kVk>{SemaphoreCreateDesc<kVk>{CreateDeviceObjectCreateDesc(std::format("Graphics Queue Timeline Semaphore")), VK_SEMAPHORE_TYPE_TIMELINE}},
			uint64_t{},
			uint32_t{},
			core::CircularContainer<QueueContext<kVk>>{})));
	queues.emplace(
		kQueueTypeCompute,
		std::make_shared<QueueTimelineContext<kVk>>(std::make_shared<QueueTimelineContextData<kVk>>(
			Semaphore<kVk>{SemaphoreCreateDesc<kVk>{CreateDeviceObjectCreateDesc(std::format("Compute Queue Timeline Semaphore")), VK_SEMAPHORE_TYPE_TIMELINE}},
			uint64_t{},
			uint32_t{},
			core::CircularContainer<QueueContext<kVk>>{})));
	queues.emplace(
		kQueueTypeTransfer,
		std::make_shared<QueueTimelineContext<kVk>>(std::make_shared<QueueTimelineContextData<kVk>>(
			Semaphore<kVk>{SemaphoreCreateDesc<kVk>{CreateDeviceObjectCreateDesc(std::format("Transfer Queue Timeline Semaphore")), VK_SEMAPHORE_TYPE_TIMELINE}},
			uint64_t{},
			uint32_t{},
			core::CircularContainer<QueueContext<kVk>>{})));

	auto isDedicatedQueueFamily = [](const QueueFamilyDesc<kVk>& queueFamily, VkQueueFlagBits type)
	{
		return (queueFamily.flags & type) && (queueFamily.flags >= type) && (queueFamily.queueCount > 0);
	};

	bool hasDedicatedCompute = false;
	bool hasDedicatedTransfer = false;
	{
		auto graphics = queues[kQueueTypeGraphics]->Write();
		auto compute = queues[kQueueTypeCompute]->Write();
		auto transfer = queues[kQueueTypeTransfer]->Write();
	
		const auto& queueFamilies = GetQueueFamilies();
		for (unsigned queueFamilyIt = 0; queueFamilyIt < queueFamilies.size(); queueFamilyIt++)
		{
			const auto& queueFamily = queueFamilies[queueFamilyIt];

			auto queueCount = queueFamily.queueCount;

			if (isDedicatedQueueFamily(queueFamily, VK_QUEUE_GRAPHICS_BIT))
			{
				graphics->queues = std::vector<QueueContext<kVk>>(queueCount);
				graphics->queueFamilyIndex = queueFamilyIt;
				for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
				{
					auto& [queue, syncInfo] = graphics->queues.FetchAdd();
					queue = Queue<kVk>(
						QueueCreateDesc<kVk>
						{
							CreateDeviceObjectCreateDesc(std::format("Graphics Queue {}", queueIt)),
							queueIt,
							queueFamilyIt,
							15,
							static_cast<uint32_t>(queueFamily.timestampValidBits > 0)
						}
					);
				}
			}
			else if (isDedicatedQueueFamily(queueFamily, VK_QUEUE_COMPUTE_BIT))
			{
				compute->queues = std::vector<QueueContext<kVk>>(queueCount);
				compute->queueFamilyIndex = queueFamilyIt;
				for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
				{
					auto& [queue, syncInfo] = compute->queues.FetchAdd();
					queue = Queue<kVk>(
						QueueCreateDesc<kVk>
						{
							CreateDeviceObjectCreateDesc(std::format("Compute Queue {}", queueIt)),
							queueIt,
							queueFamilyIt,
							1,
							static_cast<uint32_t>(queueFamily.timestampValidBits > 0)
						}
					);
				}
			}
			else if (isDedicatedQueueFamily(queueFamily, VK_QUEUE_TRANSFER_BIT))
			{
				transfer->queues = std::vector<QueueContext<kVk>>(queueCount);
				transfer->queueFamilyIndex = queueFamilyIt;
				for (unsigned queueIt = 0; queueIt < queueCount; queueIt++)
				{
					auto& [queue, syncInfo] = transfer->queues.FetchAdd();
					queue = Queue<kVk>(
						QueueCreateDesc<kVk>
						{
							CreateDeviceObjectCreateDesc(std::format("Transfer Queue {}", queueIt)),
							queueIt,
							queueFamilyIt,
							1,
							VK_FALSE // requires VK_QUEUE_GRAPHICS_BIT or VK_QUEUE_COMPUTE_BIT
						}
					);
				}
			}
		}

		ENSUREF(!graphics->queues.Empty(), "Failed to find a suitable graphics queue!");

		hasDedicatedCompute = !compute->queues.Empty();
		hasDedicatedTransfer = !transfer->queues.Empty();
	}

	// alias queue types without a dedicated queue to another type. the whole ConcurrentAccess is shared (not just its
	// data), so that locking any alias locks the same mutex. done after the write scopes above have been released.

	// Alias compute to graphics queue if no dedicated compute queue is found.
	// This is valid as long as the graphics queue family supports compute operations, which is guaranteed by the Vulkan spec.
	if (!hasDedicatedCompute)
		queues[kQueueTypeCompute] = queues[kQueueTypeGraphics];

	// Alias transfer to compute queue if no dedicated transfer queue is found.
	// This is valid as long as the compute queue family supports transfer operations, which is guaranteed by the Vulkan spec.
	if (!hasDedicatedTransfer)
		queues[kQueueTypeTransfer] = queues[kQueueTypeCompute];
}

template <>
void Device<kVk>::InternalCreatePipeline()
{
	ZoneScopedN("Device::InternalCreatePipeline");

	myPipeline = Pipeline<kVk>(
		PipelineCreateDesc<kVk>{
			CreateDeviceObjectCreateDesc("Pipeline"),
			(std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]) / "pipeline.cache").string()
		});
}

template <>
Device<kVk>::Device(CreateDescType&& desc, const Instance<kVk>& instance)
	: Object(std::forward<CreateDescType>(desc))
{
	ZoneScopedN("Device()");

	const auto& physicalDeviceInfo = instance.GetPhysicalDeviceInfo(GetPhysicalDevice());

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
		std::ranges::fill(queuePriorities, 1.0F);

		if constexpr (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
			std::cout << "Queue Family " << queueFamilyIt << 
				", queueFlags: " << queueFamilyProperty.queueFlags <<
				", queueCount: " << queueFamilyProperty.queueCount <<
				", timestampValidBits: " << queueFamilyProperty.timestampValidBits <<
				", minImageTransferGranularity: " << queueFamilyProperty.minImageTransferGranularity.width <<
				"x" << queueFamilyProperty.minImageTransferGranularity.height <<
				"x" << queueFamilyProperty.minImageTransferGranularity.depth << '\n';

		queueCreateInfos.emplace_back(VkDeviceQueueCreateInfo{
			.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.pNext=nullptr,
			.flags=0, //VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT,
			.queueFamilyIndex=queueFamilyIt,
			.queueCount=static_cast<uint32_t>(queuePriorities.size()),
			.pQueuePriorities=queuePriorities.data()});
	}

	std::vector<const char*> requiredExtensions = {
		VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
		VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME,
		VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME};

	for (const char* extensionName : requiredExtensions)
		ENSUREF(SupportsExtension(extensionName, GetDesc().physicalDevice), "Vulkan device extension not supported: {}", extensionName);

	std::vector<const char*> desiredExtensions = requiredExtensions;

#if defined(__OSX__)
	if (SupportsExtension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, GetDesc().physicalDevice))
		desiredExtensions.emplace_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

	if (SupportsExtension(VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME, GetDesc().physicalDevice))
		desiredExtensions.emplace_back(VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME);

	if (SupportsExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, GetDesc().physicalDevice))
		desiredExtensions.emplace_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);

	if (SupportsExtension(VK_KHR_PRESENT_ID_EXTENSION_NAME, GetDesc().physicalDevice))
		desiredExtensions.emplace_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);

	if (SupportsExtension(VK_KHR_PRESENT_WAIT_EXTENSION_NAME, GetDesc().physicalDevice))
		desiredExtensions.emplace_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);

	if (SupportsExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, GetDesc().physicalDevice))
		desiredExtensions.emplace_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
	
	VkDeviceCreateInfo deviceCreateInfo{.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	deviceCreateInfo.pNext = &physicalDeviceInfo.deviceFeatures;
	deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
	deviceCreateInfo.queueCreateInfoCount = queueCreateInfos.size();
	deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(desiredExtensions.size());
	deviceCreateInfo.ppEnabledExtensionNames = desiredExtensions.data();

	VK_CHECK(vkCreateDevice(GetDesc().physicalDevice, &deviceCreateInfo, &instance.GetHostAllocationCallbacks(), &myDevice));

	InitDeviceExtensions(myDevice);

	// AddOwnedObjectHandle(
	//     GetDesc().uuid,
	//     VK_OBJECT_TYPE_INSTANCE,
	//     reinterpret_cast<uint64_t>(myInstance->instance),
	//     "Instance");

	// AddOwnedObjectHandle(
	//     GetDesc().uuid,
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
	//         GetDesc().uuid,
	//         VK_OBJECT_TYPE_PHYSICAL_DEVICE,
	//         reinterpret_cast<uint64_t>(physicalDevice),
	//         stringBuffer);
	// }

	// AddOwnedObjectHandle(
	//     GetDesc().uuid,
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
		queueFamilyDesc.timestampValidBits = queueFamilyProperty.timestampValidBits;
		queueFamilyDesc.minImageTransferGranularity = queueFamilyProperty.minImageTransferGranularity;
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

	myAllocator = [this, &instance]
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
		allocatorInfo.physicalDevice = GetDesc().physicalDevice;
        allocatorInfo.preferredLargeHeapBlockSize = 0; // 0 = default (256Mb)
		allocatorInfo.device = myDevice;
		allocatorInfo.instance = instance;
        allocatorInfo.pAllocationCallbacks = &instance.GetHostAllocationCallbacks();
		allocatorInfo.pVulkanFunctions = &functions;
		vmaCreateAllocator(&allocatorInfo, &allocator);

		return allocator;
	}();
}

template <>
void Device<kVk>::Swap(Device& other) noexcept
{
	SuperType::Swap(other);
	std::swap(myDevice, other.myDevice);
	std::swap(myAllocator, other.myAllocator);
	std::swap(myQueueFamilyDescs, other.myQueueFamilyDescs);
	std::swap(myQueues, other.myQueues);
	std::swap(myPipeline, other.myPipeline);
	std::swap(myPipelineLayoutHandles, other.myPipelineLayoutHandles);
	std::swap(myResources, other.myResources);
}

template <>
Device<kVk>::Device(Device&& other) noexcept
{
	Swap(other);
}

template <>
Device<kVk>& Device<kVk>::operator=(Device<kVk>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Device<kVk>::~Device()
{
	ZoneScopedN("~Device()");

	// it is the applications responsibility to wait and destroy all queues complete gpu execution before destroying the Device.

	if (!IsValid())
		return;

	if constexpr(SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	{
		char* allocatorStatsJSON = nullptr;
		vmaBuildStatsString(myAllocator, &allocatorStatsJSON, 1U);
		std::cout << allocatorStatsJSON << '\n';
		vmaFreeStatsString(myAllocator, allocatorStatsJSON);
	}

	// members are only destroyed after this body has run, so release everything that owns device memory/objects
	// before the allocator and device go away
	myResources.clear();
	myPipelineLayoutHandles.clear();
	myPipeline = Pipeline<kVk>{};
	myQueues.clear();

	vmaDestroyAllocator(myAllocator);
	vkDestroyDevice(myDevice, &GetInstance().GetHostAllocationCallbacks());
}

} // namespace rhi
