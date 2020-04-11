#include "frame.h"

template <>
Frame<GraphicsBackend::Vulkan>::Frame(FrameCreateDesc<GraphicsBackend::Vulkan>&& desc)
: myDesc(std::move(desc))
{
    VkSemaphoreTypeCreateInfo timelineCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCreateInfo.initialValue = 0;

    VkSemaphoreCreateInfo createInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    createInfo.pNext = &timelineCreateInfo;
    createInfo.flags = 0;

    CHECK_VK(vkCreateSemaphore(myDesc.device, &createInfo, NULL, &timelineSemaphore));
    timelineValue = std::make_shared<std::atomic_uint64_t>();

    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    CHECK_VK(vkCreateFence(myDesc.device, &fenceInfo, nullptr, &fence));

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK_VK(vkCreateSemaphore(
        myDesc.device, &semaphoreInfo, nullptr, &renderCompleteSemaphore));
    CHECK_VK(vkCreateSemaphore(
        myDesc.device, &semaphoreInfo, nullptr, &newImageAcquiredSemaphore));

    graphicsFrameTimestamp = std::chrono::high_resolution_clock::now();
}

template <>
Frame<GraphicsBackend::Vulkan>::~Frame()
{
    for (auto& cmd : commands)
    {
        cmd.sync();
        cmd.free();
    }

    vkDestroySemaphore(myDesc.device, timelineSemaphore, nullptr);
    vkDestroyFence(myDesc.device, fence, nullptr);
    vkDestroySemaphore(myDesc.device, renderCompleteSemaphore, nullptr);
    vkDestroySemaphore(myDesc.device, newImageAcquiredSemaphore, nullptr);
    vkDestroyFramebuffer(myDesc.device, frameBuffer, nullptr);
}
