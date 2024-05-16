#include "../command.h"
#include "../frame.h"
#include "../rendertarget.h"

#include "utils.h"

#include <format>
#include <string_view>

template <>
RenderTargetImpl<FrameCreateDesc<kVk>, kVk>::RenderTargetImpl(
	const std::shared_ptr<Device<kVk>>& device, FrameCreateDesc<kVk>&& desc)
	: RenderTarget(device, desc)
	, myDesc(std::forward<FrameCreateDesc<kVk>>(desc))
{}

template <>
RenderTargetImpl<FrameCreateDesc<kVk>, kVk>::RenderTargetImpl(RenderTargetImpl&& other) noexcept
	: RenderTarget(std::forward<RenderTargetImpl>(other))
	, myDesc(std::exchange(other.myDesc, {}))
{}

template <>
RenderTargetImpl<FrameCreateDesc<kVk>, kVk>::~RenderTargetImpl() = default;

template <>
RenderTargetImpl<FrameCreateDesc<kVk>, kVk>&
RenderTargetImpl<FrameCreateDesc<kVk>, kVk>::operator=(RenderTargetImpl&& other) noexcept
{
	RenderTarget::operator=(std::forward<RenderTargetImpl>(other));
	myDesc = std::exchange(other.myDesc, {});
	return *this;
}

template <>
void RenderTargetImpl<FrameCreateDesc<kVk>, kVk>::Swap(RenderTargetImpl& rhs) noexcept
{
	RenderTarget::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
}

template <>
Frame<kVk>::Frame(
	const std::shared_ptr<Device<kVk>>& device, FrameCreateDesc<kVk>&& desc)
	: BaseType(device, std::forward<FrameCreateDesc<kVk>>(desc))
	, myFence(device, FenceCreateDesc<kVk>{})
	, myImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
{}

template <>
Frame<kVk>::Frame(Frame<kVk>&& other) noexcept
	: BaseType(std::forward<Frame<kVk>>(other))
	, myFence(std::exchange(other.myFence, {}))
	, myImageLayout(std::exchange(other.myImageLayout, {}))
{}

template <>
Frame<kVk>::~Frame() = default;

template <>
Frame<kVk>& Frame<kVk>::operator=(Frame<kVk>&& other) noexcept
{
	BaseType::operator=(std::forward<Frame<kVk>>(other));
	myFence = std::exchange(other.myFence, {});
	myImageLayout = std::exchange(other.myImageLayout, {});
	return *this;
}

template <>
void Frame<kVk>::Swap(Frame& rhs) noexcept
{
	BaseType::Swap(rhs);
	std::swap(myFence, rhs.myFence);
	std::swap(myImageLayout, rhs.myImageLayout);
}

template <>
ImageLayout<kVk> Frame<kVk>::GetColorImageLayout(uint32_t index) const
{
	//ASSERT(index == 0); // multiple layouts not supported

	return myImageLayout;
}

template <>
ImageLayout<kVk> Frame<kVk>::GetDepthStencilImageLayout() const
{
	ASSERT(false);

	return VK_IMAGE_LAYOUT_UNDEFINED;
}

template <>
void Frame<kVk>::End(CommandBufferHandle<kVk> cmd)
{
	RenderTarget::End(cmd);

	myImageLayout = this->GetAttachmentDesc(0).finalLayout;
}

template <>
void Frame<kVk>::TransitionColor(CommandBufferHandle<kVk> cmd, ImageLayout<kVk> layout, uint32_t index)
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
void Frame<kVk>::TransitionDepthStencil(CommandBufferHandle<kVk> cmd, ImageLayout<kVk> layout)
{
	ASSERT(false);
}

template <>
QueuePresentInfo<kVk> Frame<kVk>::PreparePresent(const QueueHostSyncInfo<kVk>& hostSyncInfo)
{
	auto hostSyncInfoCopy = hostSyncInfo;
	hostSyncInfoCopy.fences.push_back(myFence);

	return QueuePresentInfo<kVk>{
		{std::move(hostSyncInfoCopy)},
		{},
		{},
		{GetDesc().index},
		{}};
}
