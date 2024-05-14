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
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::~RenderTargetImpl() = default;

template <>
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>&
RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::operator=(RenderTargetImpl&& other) noexcept
{
	RenderTarget::operator=(std::forward<RenderTargetImpl>(other));
	myDesc = std::exchange(other.myDesc, {});
	return *this;
}

template <>
void RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::Swap(RenderTargetImpl& rhs) noexcept
{
	RenderTarget::Swap(rhs);
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
Frame<Vk>::~Frame() = default;

template <>
Frame<Vk>& Frame<Vk>::operator=(Frame<Vk>&& other) noexcept
{
	BaseType::operator=(std::forward<Frame<Vk>>(other));
	myFence = std::exchange(other.myFence, {});
	myImageLayout = std::exchange(other.myImageLayout, {});
	return *this;
}

template <>
void Frame<Vk>::Swap(Frame& rhs) noexcept
{
	BaseType::Swap(rhs);
	std::swap(myFence, rhs.myFence);
	std::swap(myImageLayout, rhs.myImageLayout);
}

template <>
ImageLayout<Vk> Frame<Vk>::GetColorImageLayout(uint32_t index) const
{
	//ASSERT(index == 0); // multiple layouts not supported

	return myImageLayout;
}

template <>
ImageLayout<Vk> Frame<Vk>::GetDepthStencilImageLayout() const
{
	ASSERT(false);

	return VK_IMAGE_LAYOUT_UNDEFINED;
}

template <>
void Frame<Vk>::End(CommandBufferHandle<Vk> cmd)
{
	RenderTarget::End(cmd);

	myImageLayout = this->GetAttachmentDesc(0).finalLayout;
}

template <>
void Frame<Vk>::TransitionColor(CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout, uint32_t index)
{
	ZoneScopedN("Frame::TransitionColor");

	ASSERT(index == 0);

	if (GetColorImageLayout(index) != layout)
	{
		TransitionImageLayout(
			cmd,
			GetDesc().colorImages[index],
			GetDesc().colorImageFormats[index],
			myImageLayout,
			layout,
			1);

		myImageLayout = layout;
	}
}

template <>
void Frame<Vk>::TransitionDepthStencil(CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout)
{
	ASSERT(false);
}

template <>
QueuePresentInfo<Vk> Frame<Vk>::PreparePresent(const QueueHostSyncInfo<Vk>& hostSyncInfo)
{
	auto hostSyncInfoCopy = hostSyncInfo;
	hostSyncInfoCopy.fences.push_back(myFence);

	return QueuePresentInfo<Vk>{
		{std::move(hostSyncInfoCopy)},
		{},
		{},
		{GetDesc().index},
		{}};
}
