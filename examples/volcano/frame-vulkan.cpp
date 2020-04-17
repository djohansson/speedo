#include "frame.h"

template <>
void Frame<GraphicsBackend::Vulkan>::waitForFence()
{
    CHECK_VK(vkWaitForFences(myRenderTargetDesc.deviceContext->getDevice(), 1, &myFence, VK_TRUE, UINT64_MAX));
    CHECK_VK(vkResetFences(myRenderTargetDesc.deviceContext->getDevice(), 1, &myFence));

    for (auto& commandContext : myCommandContexts)
        commandContext->sync();
        
    myTimestamp = std::chrono::high_resolution_clock::now();
}

template <>
Frame<GraphicsBackend::Vulkan>::Frame(
    RenderTargetDesc<GraphicsBackend::Vulkan>&& renderTargetDesc, FrameDesc<GraphicsBackend::Vulkan>&& frameDesc)
: RenderTarget<GraphicsBackend::Vulkan>(std::move(renderTargetDesc))
, myFrameDesc(std::move(frameDesc))
{
    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    CHECK_VK(vkCreateFence(myRenderTargetDesc.deviceContext->getDevice(), &fenceInfo, nullptr, &myFence));

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK_VK(vkCreateSemaphore(
        myRenderTargetDesc.deviceContext->getDevice(), &semaphoreInfo, nullptr, &myRenderCompleteSemaphore));
    CHECK_VK(vkCreateSemaphore(
        myRenderTargetDesc.deviceContext->getDevice(), &semaphoreInfo, nullptr, &myNewImageAcquiredSemaphore));

    for (uint32_t poolIt = 0; poolIt < myRenderTargetDesc.deviceContext->getFrameCommandPools().size(); poolIt++)
    {
        myCommandContexts.emplace_back(std::make_shared<CommandContext<GraphicsBackend::Vulkan>>(
            CommandContextDesc<GraphicsBackend::Vulkan>{
                myRenderTargetDesc.deviceContext,
                myRenderTargetDesc.deviceContext->getFrameCommandPools()[poolIt],
                static_cast<uint32_t>(poolIt == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY),
                myFrameDesc.timelineSemaphore,
                myFrameDesc.timelineValue}));
    }

    myTimestamp = std::chrono::high_resolution_clock::now();
}

template <>
Frame<GraphicsBackend::Vulkan>::~Frame()
{
    myCommandContexts.clear();
    
    vkDestroyFence(myRenderTargetDesc.deviceContext->getDevice(), myFence, nullptr);
    vkDestroySemaphore(myRenderTargetDesc.deviceContext->getDevice(), myRenderCompleteSemaphore, nullptr);
    vkDestroySemaphore(myRenderTargetDesc.deviceContext->getDevice(), myNewImageAcquiredSemaphore, nullptr);
}
