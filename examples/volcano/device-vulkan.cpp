#include "device.h"
#include "vk-utils.h"

#include <algorithm>
#include <utility>
#include <shared_mutex>

#include <stb_sprintf.h>

#include <xxhash.h>

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
uint64_t DeviceContext<Vk>::addTimelineCallback(std::function<void(uint64_t)>&& callback)
{
    ZoneScopedN("DeviceContext::addTimelineCallback");

    auto timelineValue = myTimelineValue.load(std::memory_order_relaxed);

    myTimelineCallbacks.emplace_back(std::make_tuple(timelineValue, std::move(callback)));

    return timelineValue;
}

template <>
uint64_t DeviceContext<Vk>::addTimelineCallback(TimelineCallback&& callback)
{
    ZoneScopedN("DeviceContext::addTimelineCallback");

    auto timelineValue = std::get<0>(callback);

    myTimelineCallbacks.emplace_back(std::move(callback));

    return timelineValue;
}

template <>
bool DeviceContext<Vk>::processTimelineCallbacks(std::optional<uint64_t> timelineValue)
{
    ZoneScopedN("DeviceContext::processTimelineCallbacks");

    TimelineCallback callbackTuple;
    while (myTimelineCallbacks.try_pop_front(callbackTuple))
    {
        const auto& [commandBufferTimelineValue, callback] = callbackTuple;

        if (timelineValue && commandBufferTimelineValue > timelineValue.value())
        {
            myTimelineCallbacks.emplace_front(std::move(callbackTuple));
            return false;
        }

        callback(commandBufferTimelineValue);
    }

    return true;
}

template <>
void DeviceContext<Vk>::addOwnedObjectHandle(
    const uuids::uuid& ownerId,
    ObjectType<Vk> objectType,
    uint64_t objectHandle,
    const char* objectName)
{
    ZoneScopedN("DeviceContext::addOwnedObjectHandle");

    if (!objectHandle)
        return;

    uint64_t ownerIdHash = 0ull;

    {
        ZoneScopedN("DeviceContext::addOwnedObjectHandle::hash");

        ownerIdHash = XXH3_64bits(&ownerId, sizeof(ownerId));
    }

    {
        auto lock = std::unique_lock(myObjectMutex);

        auto& objectInfos = myOwnerToDeviceObjectInfoMap[ownerIdHash];

        auto& objectInfo = objectInfos.emplace_back(
            ObjectNameInfo{{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                nullptr,
                objectType,
                objectHandle}});
        objectInfo.name = objectName;
        objectInfo.pObjectName = objectInfo.name.c_str();

        {
            ZoneScopedN("DeviceContext::addOwnedObjectHandle::vkSetDebugUtilsObjectNameEXT");

            VK_CHECK(device::vkSetDebugUtilsObjectNameEXT(myDevice, &objectInfo));
        }

        myObjectTypeToCountMap[objectType]++;
    }
}

template <>
void DeviceContext<Vk>::eraseOwnedObjectHandle(const uuids::uuid& ownerId, uint64_t objectHandle)
{
    ZoneScopedN("DeviceContext::eraseOwnedObjectHandle");

    if (!objectHandle)
        return;

    uint64_t ownerIdHash = 0ull;

    {
        ZoneScopedN("DeviceContext::eraseOwnedObjectHandle::hash");

        ownerIdHash = XXH3_64bits(&ownerId, sizeof(ownerId));
    }

    {
        ZoneScopedN("DeviceContext::addOwnedObjectHandle::erase");

        auto lock = std::unique_lock(myObjectMutex);

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
void DeviceContext<Vk>::clearOwnedObjectHandles(const uuids::uuid& ownerId)
{
    ZoneScopedN("DeviceContext::clearOwnedObjectHandles");

    uint64_t ownerIdHash = 0ull;

    {
        ZoneScopedN("DeviceContext::clearOwnedObjectHandles::hash");

        ownerIdHash = XXH3_64bits(&ownerId, sizeof(ownerId));
    }

    {
        ZoneScopedN("DeviceContext::clearOwnedObjectHandles::clear");

        auto lock = std::unique_lock(myObjectMutex);

        auto& objectInfos = myOwnerToDeviceObjectInfoMap[ownerIdHash];

        for (auto it = objectInfos.begin(); it != objectInfos.end(); it++)
            myObjectTypeToCountMap[it->objectType]--;
    
        objectInfos.clear();
    }
}

template <>
uint32_t DeviceContext<Vk>::getTypeCount(ObjectType<Vk> type)
{
    auto lock = std::shared_lock(myObjectMutex);

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

        static constexpr Format<Vk> requestSurfaceImageFormat[] = {
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_B8G8R8_UNORM,
            VK_FORMAT_R8G8B8_UNORM };
        static constexpr ColorSpace<Vk> requestSurfaceColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        static constexpr PresentMode<Vk> requestPresentMode[] = {
            VK_PRESENT_MODE_MAILBOX_KHR,
            VK_PRESENT_MODE_FIFO_RELAXED_KHR,
            VK_PRESENT_MODE_FIFO_KHR,
            VK_PRESENT_MODE_IMMEDIATE_KHR };

        // Request several formats, the first found will be used
        // If none of the requested image formats could be found, use the first available
        for (uint32_t requestIt = 0ul; requestIt < sizeof_array(requestSurfaceImageFormat); requestIt++)
        {
            SurfaceFormat<Vk> requestedFormat =
            {
                requestSurfaceImageFormat[requestIt],
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
        for (uint32_t requestIt = 0ul; requestIt < sizeof_array(requestPresentMode); requestIt++)
        {
            auto modeIt = std::find(
                swapchainInfo.presentModes.begin(),
                swapchainInfo.presentModes.end(),
                requestPresentMode[requestIt]);

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
    for (uint32_t queueFamilyIt = 0ul; queueFamilyIt < physicalDeviceInfo.queueFamilyProperties.size(); queueFamilyIt++)
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
    for (uint32_t i = 0ul; i < deviceExtensionCount; i++)
    {
        deviceExtensions.push_back(availableDeviceExtensions[i].extensionName);
        std::cout << deviceExtensions.back() << "\n";
    }

    std::sort(
        deviceExtensions.begin(),
        deviceExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

    std::vector<const char*> requiredDeviceExtensions = {
        // must be sorted lexicographically for std::includes to work!
        VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_EXTENSION_NAME };

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

    //     stbsp_sprintf(
    //         stringBuffer,
    //         "%s_%u",
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

    for (uint32_t queueFamilyIt = 0ul; queueFamilyIt < physicalDeviceInfo.queueFamilyProperties.size(); queueFamilyIt++)
    {
        const auto& queueFamilyProperty = physicalDeviceInfo.queueFamilyProperties[queueFamilyIt];
        const auto& queueFamilyPresentSupport = physicalDeviceInfo.queueFamilyPresentSupport[queueFamilyIt];
        
        auto& queueFamilyDesc = myQueueFamilyDescs[queueFamilyIt];
        queueFamilyDesc.queueCount = queueFamilyProperty.queueCount;
        queueFamilyDesc.flags = queueFamilyProperty.queueFlags;
        
        if (queueFamilyPresentSupport)
            queueFamilyDesc.flags |= QueueFamilyFlagBits_Present;
    }

    myAllocator = createAllocator(
        myInstance->getInstance(),
        myDevice,
        getPhysicalDevice(),
        VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT);

    VkSemaphoreTypeCreateInfo timelineCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCreateInfo.initialValue = 0ull;

    VkSemaphoreCreateInfo semaphoreCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semaphoreCreateInfo.pNext = &timelineCreateInfo;
    semaphoreCreateInfo.flags = {};

    VK_CHECK(vkCreateSemaphore(
        myDevice, &semaphoreCreateInfo, nullptr, &myTimelineSemaphore));

    // addOwnedObjectHandle(
    //     getUid(),
    //     VK_OBJECT_TYPE_SEMAPHORE,
    //     reinterpret_cast<uint64_t>(myTimelineSemaphore),
    //     "Device_TimelineSemaphore");
}

template <>
DeviceContext<Vk>::~DeviceContext()
{
    ZoneScopedN("~DeviceContext()");

    processTimelineCallbacks();

    vkDestroySemaphore(myDevice, myTimelineSemaphore, nullptr);

    vmaDestroyAllocator(myAllocator);

    vkDestroyDevice(myDevice, nullptr);
}

template <> 
DeviceObject<Vk>::DeviceObject(DeviceObject&& other) noexcept
: myDevice(std::exchange(other.myDevice, {}))
, myDesc(std::exchange(other.myDesc, {}))
, myUid(std::exchange(other.myUid, {}))
{
}

template <> 
DeviceObject<Vk>::DeviceObject(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DeviceObjectCreateDesc&& desc)
: myDevice(deviceContext)
, myDesc(std::move(desc))
, myUid(uuids::uuid_system_generator{}())
{
}

template <> 
DeviceObject<Vk>::DeviceObject(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    DeviceObjectCreateDesc&& desc,
    uint32_t objectCount,
    ObjectType<Vk> objectType,
    const uint64_t* objectHandles)
: DeviceObject(deviceContext, std::move(desc))
{
    char stringBuffer[256];
    for (uint32_t objectIt = 0ul; objectIt < objectCount; objectIt++)
    {
        stbsp_sprintf(
            stringBuffer,
            "%.*s%.*u",
            getName().size(),
            getName().c_str(),
            2,
            objectIt);

        deviceContext->addOwnedObjectHandle(getUid(), objectType, objectHandles[objectIt], stringBuffer);
    }
}

template <>
DeviceObject<Vk>::~DeviceObject()
{
    if (myDevice)
        myDevice->clearOwnedObjectHandles(getUid());
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
