#include "frame.h"
#include "command-vulkan.h"
#include "vk-utils.h"

#include <Tracy.hpp>

template <>
uint32_t Frame<GraphicsBackend::Vulkan>::ourDebugCount = 0;

template <>
void Frame<GraphicsBackend::Vulkan>::waitForFence() const
{
    CHECK_VK(vkWaitForFences(myRenderTargetDesc.deviceContext->getDevice(), 1, &myFence, VK_TRUE, UINT64_MAX));
    CHECK_VK(vkResetFences(myRenderTargetDesc.deviceContext->getDevice(), 1, &myFence));
}

template <>
Frame<GraphicsBackend::Vulkan>::Frame(Frame<GraphicsBackend::Vulkan>&& other)
    : RenderTarget<GraphicsBackend::Vulkan>(std::move(other))
	, myFrameDesc(other.myFrameDesc)
	, myCommandContexts(std::move(other.myCommandContexts))
	, myFence(other.myFence)
	, myRenderCompleteSemaphore(other.myRenderCompleteSemaphore)
	, myNewImageAcquiredSemaphore(other.myNewImageAcquiredSemaphore)
    {
        ++ourDebugCount;

		other.myFrameDesc = {};
		other.myFence = 0;
		other.myRenderCompleteSemaphore = 0;
		other.myNewImageAcquiredSemaphore = 0;
    }

template <>
Frame<GraphicsBackend::Vulkan>::Frame(
    RenderTargetDesc<GraphicsBackend::Vulkan>&& renderTargetDesc, FrameDesc<GraphicsBackend::Vulkan>&& frameDesc)
: RenderTarget<GraphicsBackend::Vulkan>(std::move(renderTargetDesc))
, myFrameDesc(std::move(frameDesc))
{
    ZoneScopedN("Frame()");

    ++ourDebugCount;

    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    CHECK_VK(vkCreateFence(myRenderTargetDesc.deviceContext->getDevice(), &fenceInfo, nullptr, &myFence));

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK_VK(vkCreateSemaphore(
        myRenderTargetDesc.deviceContext->getDevice(), &semaphoreInfo, nullptr, &myRenderCompleteSemaphore));
    CHECK_VK(vkCreateSemaphore(
        myRenderTargetDesc.deviceContext->getDevice(), &semaphoreInfo, nullptr, &myNewImageAcquiredSemaphore));

    const auto& frameCommandPools = myRenderTargetDesc.deviceContext->getGraphicsCommandPools()[myFrameDesc.index];

    uint32_t commandContextCount = std::min<uint32_t>(frameCommandPools.size(), myFrameDesc.maxCommandContextCount);
    myCommandContexts.reserve(commandContextCount);
    for (uint32_t poolIt = 0; poolIt < commandContextCount; poolIt++)
    {
        myCommandContexts.emplace_back(std::make_shared<CommandContext<GraphicsBackend::Vulkan>>(
            CommandContextDesc<GraphicsBackend::Vulkan>{
                myRenderTargetDesc.deviceContext,
                frameCommandPools[poolIt],
                static_cast<uint32_t>(poolIt == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY),
                myFrameDesc.timelineSemaphore,
                myFrameDesc.timelineValue}));
    }

#ifdef PROFILING_ENABLED
    myCommandContexts[0]->userData<command_vulkan::UserData>().tracyContext =
        TracyVkContext(
            myRenderTargetDesc.deviceContext->getPhysicalDevice(),
            myRenderTargetDesc.deviceContext->getDevice(),
            myRenderTargetDesc.deviceContext->getPrimaryGraphicsQueue(),
            myCommandContexts[0]->commands());        
#endif
}

template <>
Frame<GraphicsBackend::Vulkan>::~Frame()
{
    ZoneScopedN("~Frame()");

    --ourDebugCount;

    for (auto& commandContext : myCommandContexts)
        commandContext->clear();
    
    vkDestroyFence(myRenderTargetDesc.deviceContext->getDevice(), myFence, nullptr);
    vkDestroySemaphore(myRenderTargetDesc.deviceContext->getDevice(), myRenderCompleteSemaphore, nullptr);
    vkDestroySemaphore(myRenderTargetDesc.deviceContext->getDevice(), myNewImageAcquiredSemaphore, nullptr);
}
