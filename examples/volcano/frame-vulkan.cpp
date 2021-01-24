#include "frame.h"
#include "command.h"
#include "rendertarget.h"
#include "vk-utils.h"

#include <string_view>

#include <stb_sprintf.h>

template <>
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::RenderTargetImpl(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    FrameCreateDesc<Vk>&& desc)
: RenderTarget<Vk>(deviceContext, desc)
, myDesc(std::move(desc))
{
}

template <>
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::RenderTargetImpl(RenderTargetImpl&& other) noexcept
: RenderTarget<Vk>(std::move(other))
, myDesc(std::exchange(other.myDesc, {}))
{
}

template <>
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::~RenderTargetImpl()
{
}

template <>
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>& RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::operator=(
    RenderTargetImpl&& other) noexcept
{
    RenderTarget<Vk>::operator=(std::move(other));
    myDesc = std::exchange(other.myDesc, {});
    return *this;
}

template <>
void RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::swap(RenderTargetImpl& rhs) noexcept
{
    RenderTarget<Vk>::swap(rhs);
    std::swap(myDesc, rhs.myDesc);
}

template <>
Frame<Vk>::Frame(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    FrameCreateDesc<Vk>&& desc)
: BaseType(deviceContext, std::move(desc))
{
    ZoneScopedN("Frame()");

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK_CHECK(vkCreateSemaphore(
        getDeviceContext()->getDevice(), &semaphoreInfo, nullptr, &myRenderCompleteSemaphore));
    VK_CHECK(vkCreateSemaphore(
        getDeviceContext()->getDevice(), &semaphoreInfo, nullptr, &myNewImageAcquiredSemaphore));

    char stringBuffer[64];
    static constexpr std::string_view renderCompleteSemaphoreStr = "_RenderCompleteSemaphore";
    static constexpr std::string_view newImageAcquiredSemaphoreStr = "_NewImageAcquiredSemaphore";
    
    stbsp_sprintf(
        stringBuffer,
        "%.*s%.*s",
        getName().size(),
        getName().c_str(),
        static_cast<int>(renderCompleteSemaphoreStr.size()),
        renderCompleteSemaphoreStr.data());

    deviceContext->addOwnedObjectHandle(
        getId(),
        VK_OBJECT_TYPE_SEMAPHORE,
        reinterpret_cast<uint64_t>(myRenderCompleteSemaphore),
        stringBuffer);

    stbsp_sprintf(
        stringBuffer,
        "%.*s%.*s",
        getName().size(),
        getName().c_str(),
        static_cast<int>(newImageAcquiredSemaphoreStr.size()),
        newImageAcquiredSemaphoreStr.data());

    deviceContext->addOwnedObjectHandle(
        getId(),
        VK_OBJECT_TYPE_SEMAPHORE,
        reinterpret_cast<uint64_t>(myNewImageAcquiredSemaphore),
        stringBuffer);

    myImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

template <>
Frame<Vk>::Frame(Frame<Vk>&& other) noexcept
: BaseType(std::move(other))
, myRenderCompleteSemaphore(std::exchange(other.myRenderCompleteSemaphore, {}))
, myNewImageAcquiredSemaphore(std::exchange(other.myNewImageAcquiredSemaphore, {}))
, myImageLayout(std::exchange(other.myImageLayout, {}))
, myLastPresentTimelineValue(std::exchange(other.myLastPresentTimelineValue, {}))
{
}

template <>
Frame<Vk>::~Frame()
{
    ZoneScopedN("~Frame()");

    if (isValid())
    {
        vkDestroySemaphore(getDeviceContext()->getDevice(), myRenderCompleteSemaphore, nullptr);
        vkDestroySemaphore(getDeviceContext()->getDevice(), myNewImageAcquiredSemaphore, nullptr);
    }
}

template <>
Frame<Vk>& Frame<Vk>::operator=(Frame<Vk>&& other) noexcept
{
    BaseType::operator=(std::move(other));
    myRenderCompleteSemaphore = std::exchange(other.myRenderCompleteSemaphore, {});
    myNewImageAcquiredSemaphore = std::exchange(other.myNewImageAcquiredSemaphore, {});
    myImageLayout = std::exchange(other.myImageLayout, {});
    myLastPresentTimelineValue = std::exchange(other.myLastPresentTimelineValue, {});
    return *this;
}

template <>
void Frame<Vk>::swap(Frame& rhs) noexcept
{
    BaseType::swap(rhs);
    std::swap(myRenderCompleteSemaphore, rhs.myRenderCompleteSemaphore);
    std::swap(myNewImageAcquiredSemaphore, rhs.myNewImageAcquiredSemaphore);
    std::swap(myImageLayout, rhs.myImageLayout);
    std::swap(myLastPresentTimelineValue, rhs.myLastPresentTimelineValue);
}

template <>
ImageLayout<Vk> Frame<Vk>::getColorImageLayout(uint32_t index) const
{
    assert(index == 0);
    
    return myImageLayout;
}

template <>
ImageLayout<Vk> Frame<Vk>::getDepthStencilImageLayout() const
{
    assert(false);
    
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

template <>
void Frame<Vk>::end(CommandBufferHandle<Vk> cmd)
{
    RenderTarget<Vk>::end(cmd);

    myImageLayout = this->getAttachmentDesc(0).finalLayout;
}

template <>
void Frame<Vk>::transitionColor(CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout, uint32_t index)
{
    ZoneScopedN("Frame::transitionColor");

    assert(index == 0);

    if (getColorImageLayout(index) != layout)
    {
        transitionImageLayout(
            cmd,
            getDesc().colorImages[index],
            getDesc().colorImageFormats[index],
            myImageLayout,
            layout,
            1);

        myImageLayout = layout;
    }
}

template <>
void Frame<Vk>::transitionDepthStencil(CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout)
{
    assert(false);
}

template <>
QueuePresentInfo<Vk> Frame<Vk>::preparePresent(uint64_t timelineValue)
{
    myLastPresentTimelineValue = timelineValue;

    return QueuePresentInfo<Vk>{{myRenderCompleteSemaphore}, {}, {getDesc().index}, {}, timelineValue};
}
