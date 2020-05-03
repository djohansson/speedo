#include "frame.h"
#include "command-vulkan.h"
#include "vk-utils.h"

#include <Tracy.hpp>

template <>
uint32_t Frame<GraphicsBackend::Vulkan>::ourDebugCount = 0;

template <>
void Frame<GraphicsBackend::Vulkan>::waitForFence() const
{
    CHECK_VK(vkWaitForFences(myDesc.deviceContext->getDevice(), 1, &myFence, VK_TRUE, UINT64_MAX));
    CHECK_VK(vkResetFences(myDesc.deviceContext->getDevice(), 1, &myFence));
}

template <>
Frame<GraphicsBackend::Vulkan>::Frame(Frame<GraphicsBackend::Vulkan>&& other)
    : RenderTarget<GraphicsBackend::Vulkan>(std::move(other))
	, myCommandContexts(std::move(other.myCommandContexts))
	, myFence(other.myFence)
	, myRenderCompleteSemaphore(other.myRenderCompleteSemaphore)
	, myNewImageAcquiredSemaphore(other.myNewImageAcquiredSemaphore)
    {
        ++ourDebugCount;

		other.myFence = 0;
		other.myRenderCompleteSemaphore = 0;
		other.myNewImageAcquiredSemaphore = 0;
    }

template <>
Frame<GraphicsBackend::Vulkan>::Frame(
    FrameCreateDesc<GraphicsBackend::Vulkan>&& desc)
: RenderTarget<GraphicsBackend::Vulkan>(std::move(desc))
{
    ZoneScopedN("Frame()");

    ++ourDebugCount;

    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    CHECK_VK(vkCreateFence(myDesc.deviceContext->getDevice(), &fenceInfo, nullptr, &myFence));

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK_VK(vkCreateSemaphore(
        myDesc.deviceContext->getDevice(), &semaphoreInfo, nullptr, &myRenderCompleteSemaphore));
    CHECK_VK(vkCreateSemaphore(
        myDesc.deviceContext->getDevice(), &semaphoreInfo, nullptr, &myNewImageAcquiredSemaphore));

    const auto& frameCommandPools = myDesc.deviceContext->getGraphicsCommandPools()[getDesc<DescType>().index];

    uint32_t commandContextCount = std::min<uint32_t>(frameCommandPools.size(), getDesc<DescType>().maxCommandContextCount);
    myCommandContexts.reserve(commandContextCount);
    for (uint32_t poolIt = 0; poolIt < commandContextCount; poolIt++)
    {
        myCommandContexts.emplace_back(std::make_shared<CommandContext<GraphicsBackend::Vulkan>>(
            CommandContextCreateDesc<GraphicsBackend::Vulkan>{
                myDesc.deviceContext,
                frameCommandPools[poolIt],
                static_cast<uint32_t>(poolIt == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY),
                getDesc<DescType>().timelineSemaphore,
                getDesc<DescType>().timelineValue}));
    }

#ifdef PROFILING_ENABLED
    myCommandContexts[0]->userData<command_vulkan::UserData>().tracyContext =
        TracyVkContext(
            myDesc.deviceContext->getPhysicalDevice(),
            myDesc.deviceContext->getDevice(),
            myDesc.deviceContext->getPrimaryGraphicsQueue(),
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
    
    vkDestroyFence(myDesc.deviceContext->getDevice(), myFence, nullptr);
    vkDestroySemaphore(myDesc.deviceContext->getDevice(), myRenderCompleteSemaphore, nullptr);
    vkDestroySemaphore(myDesc.deviceContext->getDevice(), myNewImageAcquiredSemaphore, nullptr);
}
