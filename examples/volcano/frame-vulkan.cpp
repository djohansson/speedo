#include "frame.h"
#include "command.h"
#include "rendertarget.h"
#include "vk-utils.h"


template RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

template RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    FrameCreateDesc<GraphicsBackend::Vulkan>&& desc);

template RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTargetImpl();

template <>
void Frame<GraphicsBackend::Vulkan>::waitForFence() const
{
    // CHECK_VK(vkWaitForFences(getDeviceContext()->getDevice(), 1, &myFence, VK_TRUE, UINT64_MAX));
    // CHECK_VK(vkResetFences(getDeviceContext()->getDevice(), 1, &myFence));
}

template <>
Frame<GraphicsBackend::Vulkan>::Frame(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    FrameCreateDesc<GraphicsBackend::Vulkan>&& desc)
: BaseType(deviceContext, std::move(desc))
{
    ZoneScopedN("Frame()");

    // VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    // fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    // CHECK_VK(vkCreateFence(getDeviceContext()->getDevice(), &fenceInfo, nullptr, &myFence));

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK_VK(vkCreateSemaphore(
        getDeviceContext()->getDevice(), &semaphoreInfo, nullptr, &myRenderCompleteSemaphore));
    CHECK_VK(vkCreateSemaphore(
        getDeviceContext()->getDevice(), &semaphoreInfo, nullptr, &myNewImageAcquiredSemaphore));
}

template <>
Frame<GraphicsBackend::Vulkan>::~Frame()
{
    ZoneScopedN("~Frame()");
   
    //vkDestroyFence(getDeviceContext()->getDevice(), myFence, nullptr);
    vkDestroySemaphore(getDeviceContext()->getDevice(), myRenderCompleteSemaphore, nullptr);
    vkDestroySemaphore(getDeviceContext()->getDevice(), myNewImageAcquiredSemaphore, nullptr);
}
