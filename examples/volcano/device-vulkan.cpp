#include "device.h"
#include "vk-utils.h"

#include <algorithm>
#include <utility>

#include <stb_sprintf.h>

namespace device
{

static PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT = {};

}

template <>
uint64_t DeviceContext<Vk>::getTimelineSemaphoreValue() const
{
    uint64_t value;
    VK_CHECK(vkGetSemaphoreCounterValue(
        myDevice,
        myTimelineSemaphore,
        &value));

    return value;
}

template <>
void DeviceContext<Vk>::wait(uint64_t timelineValue) const
{
    ZoneScopedN("DeviceContext::wait");

    VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    waitInfo.flags = {};
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &myTimelineSemaphore;
    waitInfo.pValues = &timelineValue;
    
    VK_CHECK(vkWaitSemaphores(myDevice, &waitInfo, UINT64_MAX));
}

template <>
void DeviceContext<Vk>::waitIdle() const
{
    ZoneScopedN("DeviceContext::waitIdle");

    VK_CHECK(vkDeviceWaitIdle(myDevice));
}

template <>
void DeviceContext<Vk>::addTimelineCallback(std::function<void(uint64_t)>&& callback)
{
    ZoneScopedN("DeviceContext::addTimelineCallback");

    std::unique_lock<decltype(myTimelineCallbacksMutex)> writeLock(myTimelineCallbacksMutex);

    myTimelineCallbacks.emplace_back(
        std::make_pair(
            myTimelineValue.load(std::memory_order_relaxed),
            std::move(callback)));
}

template <>
void DeviceContext<Vk>::addTimelineCallback(uint64_t timelineValue, std::function<void(uint64_t)>&& callback)
{
    ZoneScopedN("DeviceContext::addTimelineCallback");

    std::unique_lock<decltype(myTimelineCallbacksMutex)> writeLock(myTimelineCallbacksMutex);
    
    myTimelineCallbacks.emplace_back(
        std::make_pair(
            timelineValue,
            std::move(callback)));
}

template <>
void DeviceContext<Vk>::addTimelineCallbacks(
    uint64_t timelineValue,
    const std::list<std::function<void(uint64_t)>>& callbacks)
{
    ZoneScopedN("DeviceContext::addTimelineCallbacks");

    std::unique_lock<decltype(myTimelineCallbacksMutex)> writeLock(myTimelineCallbacksMutex);
    
    for (const auto& callback : callbacks)
        myTimelineCallbacks.emplace_back(
            std::make_pair(
                timelineValue,
                std::move(callback)));
}

template <>
void DeviceContext<Vk>::processTimelineCallbacks(std::optional<uint64_t> timelineValue)
{
    ZoneScopedN("DeviceContext::processTimelineCallbacks");

    std::unique_lock<decltype(myTimelineCallbacksMutex)> writeLock(myTimelineCallbacksMutex);

    while (!myTimelineCallbacks.empty())
    {
        uint64_t commandBufferTimelineValue = myTimelineCallbacks.begin()->first;

        if (timelineValue && commandBufferTimelineValue > timelineValue.value())
            return;

        myTimelineCallbacks.begin()->second(commandBufferTimelineValue);
        myTimelineCallbacks.pop_front();
    }
}

template <>
void DeviceContext<Vk>::addOwnedObject(
    uint32_t ownerId,
    ObjectType<Vk> objectType,
    uint64_t objectHandle,
    const char* objectName)
{
    ZoneScopedN("DeviceContext::addOwnedObject");

    auto imageNameInfo = VkDebugUtilsObjectNameInfoEXT{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        nullptr,
        objectType,
        objectHandle,
        objectName};

    VK_CHECK(device::vkSetDebugUtilsObjectNameEXT(myDevice, &imageNameInfo));

    {
        std::unique_lock writelock(myObjectsMutex);
        myOwnerToDeviceObjectsMap[ownerId].emplace_back(Object{objectType, objectHandle, objectName});
        myObjectTypeToCountMap[objectType]++;
    }
}

template <>
void DeviceContext<Vk>::clearOwnedObjects(uint32_t ownerId)
{
    ZoneScopedN("DeviceContext::clearOwnedObjects");

    std::unique_lock writelock(myObjectsMutex);
    auto& objects = myOwnerToDeviceObjectsMap[ownerId];
    for (const auto& object : objects)
        myObjectTypeToCountMap[object.type]--;
    objects.clear();
}

template <>
uint32_t DeviceContext<Vk>::getTypeCount(ObjectType<Vk> type)
{
    std::shared_lock readlock(myObjectsMutex);
    return myObjectTypeToCountMap[type];
}

template <>
DeviceContext<Vk>::DeviceContext(
    const std::shared_ptr<InstanceContext<Vk>>& instanceContext,
    AutoSaveJSONFileObject<DeviceConfiguration<Vk>>&& config)
: myInstance(instanceContext)
, myConfig(std::move(config))
{
    ZoneScopedN("DeviceContext()");

    const auto& physicalDeviceInfo = myInstance->getPhysicalDeviceInfo(getPhysicalDevice());
    const auto& swapchainInfo = physicalDeviceInfo.swapchainInfo;

    if (!myConfig.swapchainConfig)
    {
        myConfig.swapchainConfig = std::make_optional(SwapchainConfiguration<Vk>{});

        const Format<Vk> requestSurfaceImageFormat[] = {
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_B8G8R8_UNORM,
            VK_FORMAT_R8G8B8_UNORM };
        const ColorSpace<Vk> requestSurfaceColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        const PresentMode<Vk> requestPresentMode[] = {
            VK_PRESENT_MODE_MAILBOX_KHR,
            VK_PRESENT_MODE_FIFO_RELAXED_KHR,
            VK_PRESENT_MODE_FIFO_KHR,
            VK_PRESENT_MODE_IMMEDIATE_KHR };

        // Request several formats, the first found will be used
        // If none of the requested image formats could be found, use the first available
        for (uint32_t request_i = 0; request_i < sizeof_array(requestSurfaceImageFormat);
                request_i++)
        {
            SurfaceFormat<Vk> requestedFormat =
            {
                requestSurfaceImageFormat[request_i],
                requestSurfaceColorSpace
            };
            auto formatIt = std::find_if(
                swapchainInfo.formats.begin(),
                swapchainInfo.formats.end(),
                [&requestedFormat](VkSurfaceFormatKHR format)
                {
                    return requestedFormat.format == format.format && requestedFormat.colorSpace == format.colorSpace;
                });
            if (formatIt != swapchainInfo.formats.end())
            {
                myConfig.swapchainConfig->surfaceFormat = *formatIt;
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
                myConfig.swapchainConfig->presentMode = *modeIt;

                switch (myConfig.swapchainConfig->presentMode)
                {
                case VK_PRESENT_MODE_MAILBOX_KHR:
                    myConfig.swapchainConfig->imageCount = 3;
                    break;
                default:
                    myConfig.swapchainConfig->imageCount = 2;
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
        VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    assert(std::includes(
        deviceExtensions.begin(), deviceExtensions.end(), requiredDeviceExtensions.begin(),
        requiredDeviceExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }));

    // auto& deviceFeaturesEx = physicalDeviceInfo.deviceFeaturesEx;
    // deviceFeaturesEx.shaderFloat16 = myConfig.useShaderFloat16.value();
    // deviceFeaturesEx.descriptorIndexing = myConfig.useDescriptorIndexing.value();
    // deviceFeaturesEx.scalarBlockLayout = myConfig.useScalarBlockLayout.value();
    // deviceFeaturesEx.hostQueryReset = myConfig.useHostQueryReset.value();
    // deviceFeaturesEx.timelineSemaphore = myConfig.useTimelineSemaphores.value();
    // deviceFeaturesEx.bufferDeviceAddress = myConfig.useBufferDeviceAddress.value();

    VkDeviceCreateInfo deviceCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceCreateInfo.pNext = &physicalDeviceInfo.deviceFeatures;
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
    deviceCreateInfo.queueCreateInfoCount = queueCreateInfos.size();
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(requiredDeviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = requiredDeviceExtensions.data();

    VK_CHECK(vkCreateDevice(getPhysicalDevice(), &deviceCreateInfo, nullptr, &myDevice));

    device::vkSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(
            myDevice,
            "vkSetDebugUtilsObjectNameEXT"));

    // addOwnedObject(
    //     0,
    //     VK_OBJECT_TYPE_INSTANCE,
    //     reinterpret_cast<uint64_t>(myInstance->getInstance()),
    //     "Instance");

    addOwnedObject(
        0,
        VK_OBJECT_TYPE_SURFACE_KHR,
        reinterpret_cast<uint64_t>(myInstance->getSurface()),
        "Instance_Surface");

    char stringBuffer[256];

    // for (uint32_t physicalDeviceIt = 0; physicalDeviceIt < myInstance->getPhysicalDevices().size(); physicalDeviceIt++)
    // {
    //     const auto& physicalDevice = myInstance->getPhysicalDevices()[physicalDeviceIt];

    //     static constexpr std::string_view physicalDeviceStr = "Instance_PhysicalDevice";

    //     stbsp_sprintf(
    //         stringBuffer,
    //         "%s_%u",
    //         physicalDeviceStr.data(),
    //         physicalDeviceIt);

    //     addOwnedObject(
    //         0,
    //         VK_OBJECT_TYPE_PHYSICAL_DEVICE,
    //         reinterpret_cast<uint64_t>(physicalDevice),
    //         stringBuffer);
    // }

    addOwnedObject(
        0,
        VK_OBJECT_TYPE_DEVICE,
        reinterpret_cast<uint64_t>(myDevice),
        "Device");

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
        {
            auto& queue = queueFamilyDesc.queues[queueIt];

            vkGetDeviceQueue(myDevice, queueFamilyIt, queueIt, &queue);

            static constexpr std::string_view queueStr = "Device_Queue";

            stbsp_sprintf(
                stringBuffer,
                "%s_%u",
                queueStr.data(),
                queueIt);

            addOwnedObject(
                0,
                VK_OBJECT_TYPE_QUEUE,
                reinterpret_cast<uint64_t>(queue),
                stringBuffer);
        }

        uint32_t frameCount = queueFamilyPresentSupport ? myConfig.swapchainConfig->imageCount : 1;

        queueFamilyDesc.commandPools.resize(frameCount * queueFamilyProperty.queueCount);

        for (uint32_t frameIt = 0; frameIt < frameCount; frameIt++)
        {
            for (uint32_t queueIt = 0; queueIt < queueFamilyProperty.queueCount; queueIt++)
            {
                auto commandPool = createCommandPool(
                    myDevice,
                    cmdPoolCreateFlags,
                    queueFamilyIt);

                static constexpr std::string_view commandPoolStr = "Device_CommandPool";

                stbsp_sprintf(
                    stringBuffer,
                    "%s_qf%u_f%u_q%u",
                    commandPoolStr.data(),
                    queueFamilyIt,
                    frameIt,
                    queueIt);

                addOwnedObject(
                    0,
                    VK_OBJECT_TYPE_COMMAND_POOL,
                    reinterpret_cast<uint64_t>(commandPool),
                    stringBuffer);

                queueFamilyDesc.commandPools[frameIt * queueFamilyProperty.queueCount + queueIt] = commandPool;
            }
        }
    }

    myAllocator = createAllocator(
        myInstance->getInstance(),
        myDevice,
        getPhysicalDevice(),
        VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT);

    myDescriptorPool = createDescriptorPool(myDevice);

    addOwnedObject(
        0,
        VK_OBJECT_TYPE_DESCRIPTOR_POOL,
        reinterpret_cast<uint64_t>(myDescriptorPool),
        "Device_DescriptorPool");

    VkSemaphoreTypeCreateInfo timelineCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCreateInfo.initialValue = 0;

    VkSemaphoreCreateInfo semaphoreCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semaphoreCreateInfo.pNext = &timelineCreateInfo;
    semaphoreCreateInfo.flags = {};

    VK_CHECK(vkCreateSemaphore(
        myDevice, &semaphoreCreateInfo, nullptr, &myTimelineSemaphore));

    addOwnedObject(
        0,
        VK_OBJECT_TYPE_SEMAPHORE,
        reinterpret_cast<uint64_t>(myTimelineSemaphore),
        "Device_TimelineSemaphore");
}

template <>
DeviceContext<Vk>::~DeviceContext()
{
    ZoneScopedN("~DeviceContext()");

    processTimelineCallbacks();

    clearOwnedObjects(0);

    vkDestroySemaphore(myDevice, myTimelineSemaphore, nullptr);
    
    vkDestroyDescriptorPool(myDevice, myDescriptorPool, nullptr);

    vmaDestroyAllocator(myAllocator);

    for (const auto& queueFamily : myQueueFamilyDescs)
        for (auto commandPool : queueFamily.commandPools)
            vkDestroyCommandPool(myDevice, commandPool, nullptr);

    vkDestroyDevice(myDevice, nullptr);
}

template <> 
uint32_t DeviceResource<Vk>::sId(0);

template <> 
DeviceResource<Vk>::DeviceResource(DeviceResource<Vk>&& other)
: myDevice(std::exchange(other.myDevice, {}))
, myName(std::exchange(other.myName, {}))
, myId(std::exchange(other.myId, {}))
{
}

template <> 
DeviceResource<Vk>::DeviceResource(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const DeviceResourceCreateDesc<Vk>& desc)
: myDevice(deviceContext)
, myName(std::move(desc.name))
, myId(++sId)
{
}

template <> 
DeviceResource<Vk>::DeviceResource(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const DeviceResourceCreateDesc<Vk>& desc,
    uint32_t objectCount,
    ObjectType<Vk> objectType,
    const uint64_t* objectHandles)
: DeviceResource(deviceContext, desc)
{
    char stringBuffer[256];
    for (uint32_t objectIt = 0; objectIt < objectCount; objectIt++)
    {
        stbsp_sprintf(
            stringBuffer,
            "%.*s%.*u",
            getName().size(),
            getName().c_str(),
            2,
            objectIt);

        deviceContext->addOwnedObject(getId(), objectType, objectHandles[objectIt], stringBuffer);
    }
}

template <>
DeviceResource<Vk>::~DeviceResource()
{
    if (myDevice)
        myDevice->clearOwnedObjects(getId());
}

template <>
DeviceResource<Vk>& DeviceResource<Vk>::operator=(DeviceResource&& other)
{
    myDevice = std::exchange(other.myDevice, {});
    myName = std::exchange(other.myName, {});
    myId = std::exchange(other.myId, {});
    
    return *this;
}
