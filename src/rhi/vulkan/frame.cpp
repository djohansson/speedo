#include "../command.h"
#include "../frame.h"
#include "../rendertarget.h"

#include "utils.h"

#include <format>
#include <string_view>

template <>
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::RenderTargetImpl(
	const std::shared_ptr<Device<Vk>>& device, FrameCreateDesc<Vk>&& desc)
	: RenderTarget(device, desc)
	, myDesc(std::forward<FrameCreateDesc<Vk>>(desc))
{}

template <>
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::RenderTargetImpl(RenderTargetImpl&& other) noexcept
	: RenderTarget(std::forward<RenderTargetImpl>(other))
	, myDesc(std::exchange(other.myDesc, {}))
{}

template <>
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::~RenderTargetImpl()
{}

template <>
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>&
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::operator=(RenderTargetImpl&& other) noexcept
{
	RenderTarget::operator=(std::forward<RenderTargetImpl>(other));
	myDesc = std::exchange(other.myDesc, {});
	return *this;
}

template <>
void RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::swap(RenderTargetImpl& rhs) noexcept
{
	RenderTarget::swap(rhs);
	std::swap(myDesc, rhs.myDesc);
}

template <>
Frame<Vk>::Frame(
	const std::shared_ptr<Device<Vk>>& device, FrameCreateDesc<Vk>&& desc)
	: BaseType(device, std::forward<FrameCreateDesc<Vk>>(desc))
	, myImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
{
	ZoneScopedN("Frame()");

	VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VK_CHECK(vkCreateSemaphore(
		*getDevice(),
		&semaphoreInfo,
		&getDevice()->getInstance()->getHostAllocationCallbacks(),
		&myRenderSemaphore));
	VK_CHECK(vkCreateSemaphore(
		*getDevice(),
		&semaphoreInfo,
		&getDevice()->getInstance()->getHostAllocationCallbacks(),
		&myPresentSemaphore));

#if GRAPHICS_VALIDATION_ENABLED
	{
		device->addOwnedObjectHandle(
			getUid(),
			VK_OBJECT_TYPE_SEMAPHORE,
			reinterpret_cast<uint64_t>(myRenderSemaphore),
			std::format("{0}_RenderSemaphore", getName()));

		device->addOwnedObjectHandle(
			getUid(),
			VK_OBJECT_TYPE_SEMAPHORE,
			reinterpret_cast<uint64_t>(myPresentSemaphore),
			std::format("{0}_PresentSemaphore",getName()));
	}
#endif
}

template <>
Frame<Vk>::Frame(Frame<Vk>&& other) noexcept
	: BaseType(std::forward<Frame<Vk>>(other))
	, myRenderSemaphore(std::exchange(other.myRenderSemaphore, {}))
	, myPresentSemaphore(std::exchange(other.myPresentSemaphore, {}))
	, myImageLayout(std::exchange(other.myImageLayout, {}))
	, myPresentSyncInfo(std::exchange(other.myPresentSyncInfo, {}))
{}

template <>
Frame<Vk>::~Frame()
{
	ZoneScopedN("~Frame()");

	if (isValid())
	{
		vkDestroySemaphore(
			*getDevice(),
			myRenderSemaphore,
			&getDevice()->getInstance()->getHostAllocationCallbacks());
		vkDestroySemaphore(
			*getDevice(),
			myPresentSemaphore,
			&getDevice()->getInstance()->getHostAllocationCallbacks());
	}
}

template <>
Frame<Vk>& Frame<Vk>::operator=(Frame<Vk>&& other) noexcept
{
	BaseType::operator=(std::forward<Frame<Vk>>(other));
	myRenderSemaphore = std::exchange(other.myRenderSemaphore, {});
	myPresentSemaphore = std::exchange(other.myPresentSemaphore, {});
	myImageLayout = std::exchange(other.myImageLayout, {});
	myPresentSyncInfo = std::exchange(other.myPresentSyncInfo, {});
	return *this;
}

template <>
void Frame<Vk>::swap(Frame& rhs) noexcept
{
	BaseType::swap(rhs);
	std::swap(myRenderSemaphore, rhs.myRenderSemaphore);
	std::swap(myPresentSemaphore, rhs.myPresentSemaphore);
	std::swap(myImageLayout, rhs.myImageLayout);
	std::swap(myPresentSyncInfo, rhs.myPresentSyncInfo);
}

template <>
ImageLayout<Vk> Frame<Vk>::getColorImageLayout(uint32_t index) const
{
	//assert(index == 0); // multiple layouts not supported

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
	RenderTarget::end(cmd);

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
QueuePresentInfo<Vk> Frame<Vk>::preparePresent(QueueHostSyncInfo<Vk>&& hostSyncInfo)
{
	myPresentSyncInfo = std::forward<QueueHostSyncInfo<Vk>>(hostSyncInfo);

	return QueuePresentInfo<Vk>{
		{myPresentSyncInfo},
		{myRenderSemaphore},
		{},
		{getDesc().index},
		{}};
}
