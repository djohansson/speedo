#include "frame.h"
#include "command.h"
#include "rendertarget.h"
#include "vk-utils.h"

#include <Tracy.hpp>

template <>
uint32_t Frame<GraphicsBackend::Vulkan>::ourDebugCount = 0;

template RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

template RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(FrameCreateDesc<GraphicsBackend::Vulkan>&& desc);

template RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTargetImpl();


template <>
void Frame<GraphicsBackend::Vulkan>::waitForFence() const
{
    CHECK_VK(vkWaitForFences(getDesc().deviceContext->getDevice(), 1, &myFence, VK_TRUE, UINT64_MAX));
    CHECK_VK(vkResetFences(getDesc().deviceContext->getDevice(), 1, &myFence));
}

template <>
Frame<GraphicsBackend::Vulkan>::Frame(Frame<GraphicsBackend::Vulkan>&& other)
: BaseType(std::move(other))
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
Frame<GraphicsBackend::Vulkan>::Frame(FrameCreateDesc<GraphicsBackend::Vulkan>&& desc)
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
}

template <>
Frame<GraphicsBackend::Vulkan>::~Frame()
{
    ZoneScopedN("~Frame()");

    --ourDebugCount;
    
    vkDestroyFence(getDesc().deviceContext->getDevice(), myFence, nullptr);
    vkDestroySemaphore(getDesc().deviceContext->getDevice(), myRenderCompleteSemaphore, nullptr);
    vkDestroySemaphore(getDesc().deviceContext->getDevice(), myNewImageAcquiredSemaphore, nullptr);
}
