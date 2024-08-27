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
	, mySemaphore(device, SemaphoreCreateDesc<kVk>{VK_SEMAPHORE_TYPE_BINARY})
	, myImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
{}

template <>
Frame<kVk>::Frame(Frame<kVk>&& other) noexcept
	: BaseType(std::forward<Frame<kVk>>(other))
	, myFence(std::exchange(other.myFence, {}))
	, myImageLayout(std::exchange(other.myImageLayout, {}))
{}

template <>
Frame<kVk>::~Frame()
{
	ZoneScopedN("~Frame()");

	if (myFence)
		myFence.Wait();
}

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
ImageLayout<kVk> Frame<kVk>::GetLayout(uint32_t) const
{
	return myImageLayout;
}

template <>
void Frame<kVk>::End(CommandBufferHandle<kVk> cmd)
{
	RenderTarget::End(cmd);

	myImageLayout = this->GetAttachmentDescs()[0].finalLayout;
}

template <>
void Frame<kVk>::Transition(CommandBufferHandle<kVk> cmd, ImageLayout<kVk> layout, uint32_t index)
{
	ZoneScopedN("Frame::TransitionColor");

	ASSERT(index == 0);

	if (GetLayout(index) != layout)
	{
		TransitionImageLayout(
			cmd,
			GetDesc().images[index],
			GetDesc().imageFormats[index],
			myImageLayout,
			layout,
			1);

		myImageLayout = layout;
	}
}

template <>
QueuePresentInfo<kVk> Frame<kVk>::PreparePresent(QueueHostSyncInfo<kVk>&& hostSyncInfo)
{
	hostSyncInfo.fences.push_back(myFence);

	return QueuePresentInfo<kVk>{
		{std::forward<QueueHostSyncInfo<kVk>>(hostSyncInfo)},
		{mySemaphore},
		{},
		{GetDesc().index},
		{}};
}
