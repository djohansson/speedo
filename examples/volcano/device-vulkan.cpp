#include "device.h"
#include "gfx.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <vector>

template <>
DeviceContext<GraphicsBackend::Vulkan>::DeviceContext(
    DeviceCreateDesc<GraphicsBackend::Vulkan>&& desc)
    : myDesc(std::move(desc))
{
    uint32_t deviceCount = 0;
    CHECK_VK(vkEnumeratePhysicalDevices(myDesc.instance, &deviceCount, nullptr));
    if (deviceCount == 0)
        throw std::runtime_error("failed to find GPUs with Vulkan support!");

    std::vector<PhysicalDeviceHandle<GraphicsBackend::Vulkan>> devices(deviceCount);
    CHECK_VK(vkEnumeratePhysicalDevices(myDesc.instance, &deviceCount, devices.data()));

    for (const auto& device : devices)
    {
        std::tie(mySwapChainInfo, mySelectedQueueFamilyIndex, myPhysicalDeviceProperties) =
            getSuitableSwapchainAndQueueFamilyIndex<GraphicsBackend::Vulkan>(myDesc.surface, device);

        if (mySelectedQueueFamilyIndex >= 0)
        {
            myPhysicalDevice = device;

            const Format<GraphicsBackend::Vulkan> requestSurfaceImageFormat[] = {
                VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM,
                VK_FORMAT_R8G8B8_UNORM};
            const ColorSpace<GraphicsBackend::Vulkan> requestSurfaceColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            const PresentMode<GraphicsBackend::Vulkan> requestPresentMode[] = {
                VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR,
                VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};

            // Request several formats, the first found will be used
            // If none of the requested image formats could be found, use the first available
            mySelectedSurfaceFormat = mySwapChainInfo.formats[0];
            for (uint32_t request_i = 0; request_i < sizeof_array(requestSurfaceImageFormat);
                    request_i++)
            {
                SurfaceFormat<GraphicsBackend::Vulkan> requestedFormat = {requestSurfaceImageFormat[request_i],
                                                        requestSurfaceColorSpace};
                auto formatIt = std::find(
                    mySwapChainInfo.formats.begin(), mySwapChainInfo.formats.end(),
                    requestedFormat);
                if (formatIt != mySwapChainInfo.formats.end())
                {
                    mySelectedSurfaceFormat = *formatIt;
                    break;
                }
            }

            // Request a certain mode and confirm that it is available. If not use
            // VK_PRESENT_MODE_FIFO_KHR which is mandatory
            mySelectedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
            mySelectedFrameCount = 2;
            for (uint32_t request_i = 0; request_i < sizeof_array(requestPresentMode);
                    request_i++)
            {
                auto modeIt = std::find(
                    mySwapChainInfo.presentModes.begin(), mySwapChainInfo.presentModes.end(),
                    requestPresentMode[request_i]);
                if (modeIt != mySwapChainInfo.presentModes.end())
                {
                    mySelectedPresentMode = *modeIt;

                    if (mySelectedPresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
                        mySelectedFrameCount = 3;

                    break;
                }
            }

            break;
        }
    }

    if (!myPhysicalDevice)
        throw std::runtime_error("failed to find a suitable GPU!");

    const float graphicsQueuePriority = 1.0f;

    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = mySelectedQueueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &graphicsQueuePriority;

    VkPhysicalDeviceFeatures deviceFeatures = {};
    deviceFeatures.samplerAnisotropy = VK_TRUE;

    uint32_t deviceExtensionCount;
    vkEnumerateDeviceExtensionProperties(
        myPhysicalDevice, nullptr, &deviceExtensionCount, nullptr);

    std::vector<VkExtensionProperties> availableDeviceExtensions(deviceExtensionCount);
    vkEnumerateDeviceExtensionProperties(
        myPhysicalDevice, nullptr, &deviceExtensionCount, availableDeviceExtensions.data());

    std::vector<const char*> deviceExtensions;
    deviceExtensions.reserve(deviceExtensionCount);
    for (uint32_t i = 0; i < deviceExtensionCount; i++)
    {
#if defined(__APPLE__)
        if (strcmp(availableDeviceExtensions[i].extensionName, "VK_MVK_moltenvk") == 0 ||
            strcmp(availableDeviceExtensions[i].extensionName, "VK_KHR_surface") == 0 ||
            strcmp(availableDeviceExtensions[i].extensionName, "VK_MVK_macos_surface") == 0)
            continue;
#endif
        deviceExtensions.push_back(availableDeviceExtensions[i].extensionName);
        std::cout << deviceExtensions.back() << "\n";
    }

    std::sort(
        deviceExtensions.begin(), deviceExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

    std::vector<const char*> requiredDeviceExtensions = {
        // must be sorted lexicographically for std::includes to work!
        "VK_KHR_swapchain"};

    assert(std::includes(
        deviceExtensions.begin(), deviceExtensions.end(), requiredDeviceExtensions.begin(),
        requiredDeviceExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }));

    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
    deviceCreateInfo.enabledExtensionCount =
        static_cast<uint32_t>(requiredDeviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = requiredDeviceExtensions.data();

    CHECK_VK(vkCreateDevice(myPhysicalDevice, &deviceCreateInfo, nullptr, &myDevice));

    vkGetDeviceQueue(myDevice, mySelectedQueueFamilyIndex, 0, &mySelectedQueue);
}

template <>
DeviceContext<GraphicsBackend::Vulkan>::~DeviceContext()
{
    vkDestroyDevice(myDevice, nullptr);
}
