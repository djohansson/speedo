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
	, myFence(device, FenceCreateDesc<Vk>{})
	, myImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
{}

template <>
Frame<Vk>::Frame(Frame<Vk>&& other) noexcept
	: BaseType(std::forward<Frame<Vk>>(other))
	, myFence(std::exchange(other.myFence, {}))
	, myImageLayout(std::exchange(other.myImageLayout, {}))
{}

template <>
Frame<Vk>::~Frame()
{}

template <>
Frame<Vk>& Frame<Vk>::operator=(Frame<Vk>&& other) noexcept
{
	BaseType::operator=(std::forward<Frame<Vk>>(other));
	myFence = std::exchange(other.myFence, {});
	myImageLayout = std::exchange(other.myImageLayout, {});
	return *this;
}

template <>
void Frame<Vk>::swap(Frame& rhs) noexcept
{
	BaseType::swap(rhs);
	std::swap(myFence, rhs.myFence);
	std::swap(myImageLayout, rhs.myImageLayout);
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
QueuePresentInfo<Vk> Frame<Vk>::preparePresent(const QueueHostSyncInfo<Vk>& hostSyncInfo)
{
	auto hostSyncInfoCopy = hostSyncInfo;
	hostSyncInfoCopy.fences.push_back(myFence);

	return QueuePresentInfo<Vk>{
		{std::move(hostSyncInfoCopy)},
		{},
		{},
		{getDesc().index},
		{}};
}
