#include "frame.h"

template <>
void Frame<GraphicsBackend::Vulkan>::waitForFence()
{
    CHECK_VK(vkWaitForFences(myDesc.deviceContext->getDevice(), 1, &myFence, VK_TRUE, UINT64_MAX));
    CHECK_VK(vkResetFences(myDesc.deviceContext->getDevice(), 1, &myFence));

    myTimestamp = std::chrono::high_resolution_clock::now();
}

template <>
Frame<GraphicsBackend::Vulkan>::Frame(RenderTargetDesc<GraphicsBackend::Vulkan>&& desc)
: RenderTarget<GraphicsBackend::Vulkan>(std::move(desc))
{
    VkSemaphoreTypeCreateInfo timelineCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCreateInfo.initialValue = 0;

    VkSemaphoreCreateInfo createInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    createInfo.pNext = &timelineCreateInfo;
    createInfo.flags = 0;

    CHECK_VK(vkCreateSemaphore(myDesc.deviceContext->getDevice(), &createInfo, NULL, &myTimelineSemaphore));
    myTimelineValue = std::make_shared<std::atomic_uint64_t>();

    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    CHECK_VK(vkCreateFence(myDesc.deviceContext->getDevice(), &fenceInfo, nullptr, &myFence));

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK_VK(vkCreateSemaphore(
        myDesc.deviceContext->getDevice(), &semaphoreInfo, nullptr, &myRenderCompleteSemaphore));
    CHECK_VK(vkCreateSemaphore(
        myDesc.deviceContext->getDevice(), &semaphoreInfo, nullptr, &myNewImageAcquiredSemaphore));

    for (uint32_t poolIt = 0; poolIt < myDesc.deviceContext->getFrameCommandPools().size(); poolIt++)
    {
        myCommands.emplace_back(
            CommandContext<GraphicsBackend::Vulkan>(
                CommandDesc<GraphicsBackend::Vulkan>{
                    myDesc.deviceContext,
                    myDesc.deviceContext->getFrameCommandPools()[poolIt],
                    static_cast<uint32_t>(poolIt == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY),
                    myTimelineSemaphore,
                    myTimelineValue}));
    }

    myTimestamp = std::chrono::high_resolution_clock::now();
}

template <>
Frame<GraphicsBackend::Vulkan>::~Frame()
{
    for (auto& cmd : myCommands)
    {
        cmd.sync();
        cmd.free();
    }

    vkDestroySemaphore(myDesc.deviceContext->getDevice(), myTimelineSemaphore, nullptr);
    vkDestroyFence(myDesc.deviceContext->getDevice(), myFence, nullptr);
    vkDestroySemaphore(myDesc.deviceContext->getDevice(), myRenderCompleteSemaphore, nullptr);
    vkDestroySemaphore(myDesc.deviceContext->getDevice(), myNewImageAcquiredSemaphore, nullptr);
}
