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
Frame<GraphicsBackend::Vulkan>::Frame(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    FrameCreateDesc<GraphicsBackend::Vulkan>&& desc)
: BaseType(deviceContext, std::move(desc))
{
    ZoneScopedN("Frame()");

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK_VK(vkCreateSemaphore(
        getDeviceContext()->getDevice(), &semaphoreInfo, nullptr, &myRenderCompleteSemaphore));
    CHECK_VK(vkCreateSemaphore(
        getDeviceContext()->getDevice(), &semaphoreInfo, nullptr, &myNewImageAcquiredSemaphore));

    char stringBuffer[64];
    static constexpr std::string_view renderCompleteSemaphoreStr = "_RenderCompleteSemaphore";
    static constexpr std::string_view newImageAcquiredSemaphoreStr = "_NewImageAcquiredSemaphore";
    
    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s",
        getName().size(),
        getName().c_str(),
        static_cast<int>(renderCompleteSemaphoreStr.size()),
        renderCompleteSemaphoreStr.data());

    addObject(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(myRenderCompleteSemaphore), stringBuffer);

    sprintf_s(
        stringBuffer,
        sizeof(stringBuffer),
        "%.*s%.*s",
        getName().size(),
        getName().c_str(),
        static_cast<int>(newImageAcquiredSemaphoreStr.size()),
        newImageAcquiredSemaphoreStr.data());

    addObject(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(myNewImageAcquiredSemaphore), stringBuffer);
}

template <>
Frame<GraphicsBackend::Vulkan>::~Frame()
{
    ZoneScopedN("~Frame()");
   
    vkDestroySemaphore(getDeviceContext()->getDevice(), myRenderCompleteSemaphore, nullptr);
    vkDestroySemaphore(getDeviceContext()->getDevice(), myNewImageAcquiredSemaphore, nullptr);
}
