#include "device.h"
#include "frame.h"
#include "gfx.h"
#include "vk-utils.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <utility>

template <>
uint64_t DeviceContext<GraphicsBackend::Vulkan>::getTimelineSemaphoreValue() const
{
    uint64_t value;
    CHECK_VK(vkGetSemaphoreCounterValue(
        myDevice,
        myTimelineSemaphore,
        &value));

    return value;
}

template <>
void DeviceContext<GraphicsBackend::Vulkan>::wait(uint64_t timelineValue) const
{
    VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    waitInfo.flags = 0;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &myTimelineSemaphore;
    waitInfo.pValues = &timelineValue;

    CHECK_VK(vkWaitSemaphores(myDevice, &waitInfo, UINT64_MAX));
}

template <>
void DeviceContext<GraphicsBackend::Vulkan>::collectGarbage(std::optional<uint64_t> timelineValue)
{
    std::unique_lock<decltype(myGarbageCollectCallbacksMutex)> writeLock(myGarbageCollectCallbacksMutex);

    ZoneScopedN("collectGarbage");

    while (!myGarbageCollectCallbacks.empty())
    {
        uint64_t commandBufferTimelineValue = myGarbageCollectCallbacks.begin()->first;

        if (timelineValue && commandBufferTimelineValue > timelineValue.value())
            return;

        myGarbageCollectCallbacks.begin()->second(commandBufferTimelineValue);
        myGarbageCollectCallbacks.pop_front();
    }
}

template <>
void DeviceContext<GraphicsBackend::Vulkan>::addGarbageCollectCallback(std::function<void(uint64_t)>&& callback)
{
    std::unique_lock<decltype(myGarbageCollectCallbacksMutex)> writeLock(myGarbageCollectCallbacksMutex);

    myGarbageCollectCallbacks.emplace_back(
        std::make_pair(
            myTimelineValue->load(std::memory_order_relaxed),
            std::move(callback)));
}

template <>
void DeviceContext<GraphicsBackend::Vulkan>::addGarbageCollectCallback(uint64_t timelineValue, std::function<void(uint64_t)>&& callback)
{
    std::unique_lock<decltype(myGarbageCollectCallbacksMutex)> writeLock(myGarbageCollectCallbacksMutex);
    
    myGarbageCollectCallbacks.emplace_back(
        std::make_pair(
            timelineValue,
            std::move(callback)));
}

template <>
DeviceContext<GraphicsBackend::Vulkan>::DeviceContext(
    const std::shared_ptr<InstanceContext<GraphicsBackend::Vulkan>>& instanceContext,
    ScopedFileObject<DeviceConfiguration<GraphicsBackend::Vulkan>>&& config)
: myInstanceContext(instanceContext)
, myConfig(std::move(config))
{
    ZoneScopedN("DeviceContext()");

    const auto& physicalDeviceInfo =
        myInstanceContext->getPhysicalDeviceInfos()[myConfig.physicalDeviceIndex];
    const auto& swapchainInfo = physicalDeviceInfo.swapchainInfo;

    if (!myConfig.swapchainConfiguration)
    {
        myConfig.swapchainConfiguration = std::make_optional(SwapchainConfiguration<GraphicsBackend::Vulkan>{});

        const Format<GraphicsBackend::Vulkan> requestSurfaceImageFormat[] = {
            VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM,
            VK_FORMAT_R8G8B8_UNORM};
        const ColorSpace<GraphicsBackend::Vulkan> requestSurfaceColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        const PresentMode<GraphicsBackend::Vulkan> requestPresentMode[] = {
            VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR,
            VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};

        // Request several formats, the first found will be used
        // If none of the requested image formats could be found, use the first available
        for (uint32_t request_i = 0; request_i < sizeof_array(requestSurfaceImageFormat);
                request_i++)
        {
            SurfaceFormat<GraphicsBackend::Vulkan> requestedFormat =
            {
                requestSurfaceImageFormat[request_i],
                requestSurfaceColorSpace
            };
            auto formatIt = std::find(
                swapchainInfo.formats.begin(),
                swapchainInfo.formats.end(),
                requestedFormat);
            if (formatIt != swapchainInfo.formats.end())
            {
                myConfig.swapchainConfiguration->surfaceFormat = *formatIt;
                break;
            }
        }

        // Request a certain mode and confirm that it is available. If not use
        // VK_PRESENT_MODE_FIFO_KHR which is mandatory
        for (uint32_t request_i = 0; request_i < sizeof_array(requestPresentMode);
                request_i++)
        {
            auto modeIt = std::find(
                swapchainInfo.presentModes.begin(), swapchainInfo.presentModes.end(),
                requestPresentMode[request_i]);
            if (modeIt != swapchainInfo.presentModes.end())
            {
                myConfig.swapchainConfiguration->presentMode = *modeIt;

                switch (myConfig.swapchainConfiguration->presentMode)
                {
                case VK_PRESENT_MODE_MAILBOX_KHR:
                    myConfig.swapchainConfiguration->imageCount = 3;
                    break;
                default:
                    myConfig.swapchainConfiguration->imageCount = 2;
                    break;
                }

                break;
            }
        }
    }
 
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(physicalDeviceInfo.queueFamilyProperties.size());
    std::list<std::vector<float>> queuePriorityList;
    for (uint32_t queueFamilyIt = 0; queueFamilyIt < physicalDeviceInfo.queueFamilyProperties.size(); queueFamilyIt++)
    {
        auto& queuePriorities = queuePriorityList.emplace_back();
        const auto& queueFamilyProperty = physicalDeviceInfo.queueFamilyProperties[queueFamilyIt];
        queuePriorities.resize(queueFamilyProperty.queueCount);
        std::fill(queuePriorities.begin(), queuePriorities.end(), 1.0f);

        queueCreateInfos.emplace_back(VkDeviceQueueCreateInfo{
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            nullptr,
            0,//queueFamilyProperty.queueFlags,
            queueFamilyIt,
            static_cast<uint32_t>(queuePriorities.size()),
            queuePriorities.data()
        });
    }

    uint32_t deviceExtensionCount;
    vkEnumerateDeviceExtensionProperties(
        getPhysicalDevice(), nullptr, &deviceExtensionCount, nullptr);

    std::vector<VkExtensionProperties> availableDeviceExtensions(deviceExtensionCount);
    vkEnumerateDeviceExtensionProperties(
        getPhysicalDevice(), nullptr, &deviceExtensionCount, availableDeviceExtensions.data());

    std::vector<const char*> deviceExtensions;
    deviceExtensions.reserve(deviceExtensionCount);
    for (uint32_t i = 0; i < deviceExtensionCount; i++)
    {
        deviceExtensions.push_back(availableDeviceExtensions[i].extensionName);
        std::cout << deviceExtensions.back() << "\n";
    }

    std::sort(
        deviceExtensions.begin(), deviceExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

    std::vector<const char*> requiredDeviceExtensions = {
        // must be sorted lexicographically for std::includes to work!
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    assert(std::includes(
        deviceExtensions.begin(), deviceExtensions.end(), requiredDeviceExtensions.begin(),
        requiredDeviceExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }));

    if (!myConfig.useHostQueryReset)
    {
        if (auto it = std::find_if(deviceExtensions.begin(), deviceExtensions.end(),
            [](const char* extension) { return strcmp(extension, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME) == 0; });
            it != deviceExtensions.end())
        {
            myConfig.useHostQueryReset = std::make_optional(true);
        }
        else
        {
            myConfig.useHostQueryReset = std::make_optional(false);
        }
    }

    if (!myConfig.useTimelineSemaphores)
    {
        if (auto it = std::find_if(deviceExtensions.begin(), deviceExtensions.end(),
            [](const char* extension) { return strcmp(extension, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0; });
            it != deviceExtensions.end())
        {
            myConfig.useTimelineSemaphores = std::make_optional(true);
        }
        else
        {
            myConfig.useTimelineSemaphores = std::make_optional(false);
        }
    }

    VkPhysicalDeviceFeatures physicalDeviceFeatures = {};
    physicalDeviceFeatures.samplerAnisotropy = VK_TRUE;

    VkPhysicalDeviceHostQueryResetFeatures hostQueryResetFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES };
    hostQueryResetFeatures.hostQueryReset = myConfig.useHostQueryReset.value();

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    timelineFeatures.pNext = &hostQueryResetFeatures;
    timelineFeatures.timelineSemaphore = myConfig.useTimelineSemaphores.value();

    VkPhysicalDeviceFeatures2 physicalDeviceFeatures2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    physicalDeviceFeatures2.pNext = &timelineFeatures;
    physicalDeviceFeatures2.features = physicalDeviceFeatures;

    VkDeviceCreateInfo deviceCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceCreateInfo.pNext = &physicalDeviceFeatures2;
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
    deviceCreateInfo.queueCreateInfoCount = queueCreateInfos.size();
    deviceCreateInfo.enabledExtensionCount =
        static_cast<uint32_t>(requiredDeviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = requiredDeviceExtensions.data();

    CHECK_VK(vkCreateDevice(getPhysicalDevice(), &deviceCreateInfo, nullptr, &myDevice));

    VkCommandPoolCreateFlags cmdPoolCreateFlags =
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    myQueueFamilyDescs.resize(physicalDeviceInfo.queueFamilyProperties.size());
    for (uint32_t queueFamilyIt = 0; queueFamilyIt < physicalDeviceInfo.queueFamilyProperties.size(); queueFamilyIt++)
    {
        const auto& queueFamilyProperty = physicalDeviceInfo.queueFamilyProperties[queueFamilyIt];
        const auto& queueFamilyPresentSupport = physicalDeviceInfo.queueFamilyPresentSupport[queueFamilyIt];
        
        auto& queueFamilyDesc = myQueueFamilyDescs[queueFamilyIt];
        queueFamilyDesc.queues.resize(queueFamilyProperty.queueCount);
        queueFamilyDesc.flags = queueFamilyProperty.queueFlags;
        if (queueFamilyPresentSupport)
            queueFamilyDesc.flags |= QueueFamilyFlags::Present;

        for (uint32_t queueIt = 0; queueIt < queueFamilyProperty.queueCount; queueIt++)
            vkGetDeviceQueue(myDevice, queueFamilyIt, queueIt, &queueFamilyDesc.queues[queueIt]);

        uint32_t frameCount = queueFamilyPresentSupport ? myConfig.swapchainConfiguration->imageCount : 1;

        queueFamilyDesc.commandPools.resize(frameCount);

        for (uint32_t frameIt = 0; frameIt < frameCount; frameIt++)
        {
            auto& frameCommandPools = queueFamilyDesc.commandPools[frameIt];
            frameCommandPools.resize(queueFamilyProperty.queueCount);
            for (uint32_t queueIt = 0; queueIt < queueFamilyProperty.queueCount; queueIt++)
                frameCommandPools[queueIt] = createCommandPool(
                    myDevice,
                    cmdPoolCreateFlags,
                    queueFamilyIt);
        }
    }

    myAllocator = createAllocator<GraphicsBackend::Vulkan>(
        myInstanceContext->getInstance(),
        myDevice,
        getPhysicalDevice());

    myDescriptorPool = createDescriptorPool<GraphicsBackend::Vulkan>(myDevice);

    VkSemaphoreTypeCreateInfo timelineCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCreateInfo.initialValue = 0;

    bool useTimelineSemaphores = myConfig.useTimelineSemaphores.value();

    VkSemaphoreCreateInfo semaphoreCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semaphoreCreateInfo.pNext = useTimelineSemaphores ? &timelineCreateInfo : nullptr;
    semaphoreCreateInfo.flags = 0;

    CHECK_VK(vkCreateSemaphore(
        myDevice, &semaphoreCreateInfo, nullptr, &myTimelineSemaphore));

    myTimelineValue = std::make_shared<std::atomic_uint64_t>(0);
}

template <>
DeviceContext<GraphicsBackend::Vulkan>::~DeviceContext()
{
    ZoneScopedN("DeviceContext()");

    collectGarbage();

    vkDestroySemaphore(myDevice, myTimelineSemaphore, nullptr);
    
    vkDestroyDescriptorPool(myDevice, myDescriptorPool, nullptr);

    vmaDestroyAllocator(myAllocator);

    for (const auto& queueFamily : myQueueFamilyDescs)
        for (auto commandPoolVector : queueFamily.commandPools)
            for (auto commandPool : commandPoolVector)
                vkDestroyCommandPool(myDevice, commandPool, nullptr);

    vkDestroyDevice(myDevice, nullptr);
}
