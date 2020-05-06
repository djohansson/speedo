#include "frame.h"
#include "command.h"
#include "rendertarget.h"
#include "vk-utils.h"

#include <Tracy.hpp>

template <>
uint32_t Frame<GraphicsBackend::Vulkan>::ourDebugCount = 0;

template RenderTarget<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTarget(
    RenderTarget<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

template RenderTarget<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTarget(CreateDescType&& desc);

template RenderTarget<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTarget();

template <>
void Frame<GraphicsBackend::Vulkan>::waitForFence() const
{
    CHECK_VK(vkWaitForFences(getDesc().deviceContext->getDevice(), 1, &myFence, VK_TRUE, UINT64_MAX));
    CHECK_VK(vkResetFences(getDesc().deviceContext->getDevice(), 1, &myFence));
}

template <>
Frame<GraphicsBackend::Vulkan>::Frame(Frame<GraphicsBackend::Vulkan>&& other)
: BaseType(std::move(other))
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
Frame<GraphicsBackend::Vulkan>::Frame(CreateDescType&& desc)
: BaseType(std::move(desc))
{
    ZoneScopedN("Frame()");

    ++ourDebugCount;

    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    CHECK_VK(vkCreateFence(getDesc().deviceContext->getDevice(), &fenceInfo, nullptr, &myFence));

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK_VK(vkCreateSemaphore(
        getDesc().deviceContext->getDevice(), &semaphoreInfo, nullptr, &myRenderCompleteSemaphore));
    CHECK_VK(vkCreateSemaphore(
        getDesc().deviceContext->getDevice(), &semaphoreInfo, nullptr, &myNewImageAcquiredSemaphore));

    const auto& frameCommandPools = getDesc().deviceContext->getGraphicsCommandPools()[myDesc.index];

    uint32_t commandContextCount = std::min<uint32_t>(frameCommandPools.size(), myDesc.maxCommandContextCount);
    myCommandContexts.reserve(commandContextCount);
    for (uint32_t poolIt = 0; poolIt < commandContextCount; poolIt++)
    {
        myCommandContexts.emplace_back(std::make_shared<CommandContext<GraphicsBackend::Vulkan>>(
            CommandContextCreateDesc<GraphicsBackend::Vulkan>{
                getDesc().deviceContext,
                frameCommandPools[poolIt],
                static_cast<uint32_t>(poolIt == 0 ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY),
                myDesc.timelineSemaphore,
                myDesc.timelineValue}));
    }

#ifdef PROFILING_ENABLED
    myCommandContexts[0]->userData<command_vulkan::UserData>().tracyContext =
        TracyVkContext(
            getDesc().deviceContext->getPhysicalDevice(),
            getDesc().deviceContext->getDevice(),
            getDesc().deviceContext->getPrimaryGraphicsQueue(),
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
    
    vkDestroyFence(getDesc().deviceContext->getDevice(), myFence, nullptr);
    vkDestroySemaphore(getDesc().deviceContext->getDevice(), myRenderCompleteSemaphore, nullptr);
    vkDestroySemaphore(getDesc().deviceContext->getDevice(), myNewImageAcquiredSemaphore, nullptr);
}
