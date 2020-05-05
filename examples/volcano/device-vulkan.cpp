#include "device.h"
#include "frame.h"
#include "gfx.h"
#include "vk-utils.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <Tracy.hpp>


template <>
DeviceContext<GraphicsBackend::Vulkan>::DeviceContext(
    DeviceCreateDesc<GraphicsBackend::Vulkan>&& desc)
    : myDesc(std::move(desc))
{
    ZoneScopedN("DeviceContext()");

    const auto& physicalDeviceInfo =
        myDesc.instanceContext->getPhysicalDeviceInfos()[myDesc.physicalDeviceIndex];
    const auto& swapchainInfo = physicalDeviceInfo.swapchainInfo;

    if (!myDesc.swapchainConfiguration.has_value())
    {
        myDesc.swapchainConfiguration = std::make_optional(SwapchainConfiguration<GraphicsBackend::Vulkan>{});

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
                myDesc.swapchainConfiguration->surfaceFormat = *formatIt;
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
                myDesc.swapchainConfiguration->presentMode = *modeIt;

                switch (myDesc.swapchainConfiguration->presentMode)
                {
                case VK_PRESENT_MODE_MAILBOX_KHR:
                    myDesc.swapchainConfiguration->imageCount = 3;
                    break;
                default:
                    myDesc.swapchainConfiguration->imageCount = 2;
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

    if (!myDesc.useTimelineSemaphores.has_value())
        if (auto it = std::find_if(deviceExtensions.begin(), deviceExtensions.end(),
            [](const char* extension) { return strcmp(extension, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0; });
            it != deviceExtensions.end())
            myDesc.useTimelineSemaphores = std::make_optional(true);

    if (!myDesc.useCommandPoolReset.has_value())
        myDesc.useCommandPoolReset = std::make_optional(false);

    VkPhysicalDeviceFeatures physicalDeviceFeatures = {};
    physicalDeviceFeatures.samplerAnisotropy = VK_TRUE;

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    timelineFeatures.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceFeatures2 physicalDeviceFeatures2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    physicalDeviceFeatures2.pNext = myDesc.useTimelineSemaphores.value() ? &timelineFeatures : nullptr;
    physicalDeviceFeatures2.features = physicalDeviceFeatures;

    VkDeviceCreateInfo deviceCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceCreateInfo.pNext = &physicalDeviceFeatures2;
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
    deviceCreateInfo.queueCreateInfoCount = queueCreateInfos.size();
    deviceCreateInfo.enabledExtensionCount =
        static_cast<uint32_t>(requiredDeviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = requiredDeviceExtensions.data();

    CHECK_VK(vkCreateDevice(getPhysicalDevice(), &deviceCreateInfo, nullptr, &myDevice));

    VkCommandPoolCreateFlags cmdPoolCreateFlags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (!myDesc.useCommandPoolReset.value())
        cmdPoolCreateFlags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

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

        uint32_t frameCount = queueFamilyPresentSupport ? myDesc.swapchainConfiguration->imageCount : 1;

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
        myDesc.instanceContext->getInstance(),
        myDevice,
        getPhysicalDevice());

    myDescriptorPool = createDescriptorPool<GraphicsBackend::Vulkan>(myDevice);
}

template <>
DeviceContext<GraphicsBackend::Vulkan>::~DeviceContext()
{
    ZoneScopedN("DeviceContext()");
    
    vkDestroyDescriptorPool(myDevice, myDescriptorPool, nullptr);

    vmaDestroyAllocator(myAllocator);

    for (const auto& queueFamily : myQueueFamilyDescs)
        for (auto commandPoolVector : queueFamily.commandPools)
            for (auto commandPool : commandPoolVector)
                vkDestroyCommandPool(myDevice, commandPool, nullptr);

    vkDestroyDevice(myDevice, nullptr);
}
