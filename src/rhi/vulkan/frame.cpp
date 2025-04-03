#include "../frame.h"
#include "../rendertarget.h"

#include "utils.h"

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
	, myImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
{}

template <>
Frame<kVk>::Frame(Frame<kVk>&& other) noexcept
	: BaseType(std::forward<Frame<kVk>>(other))
	, myImageLayout(std::exchange(other.myImageLayout, {}))
{}

template <>
Frame<kVk>& Frame<kVk>::operator=(Frame<kVk>&& other) noexcept
{
	BaseType::operator=(std::forward<Frame<kVk>>(other));
	myImageLayout = std::exchange(other.myImageLayout, {});
	return *this;
}

template <>
void Frame<kVk>::Swap(Frame& rhs) noexcept
{
	BaseType::Swap(rhs);
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

	myImageLayout = GetAttachmentDescs()[0].finalLayout;
}

template <>
void Frame<kVk>::Transition(CommandBufferHandle<kVk> cmd, ImageLayout<kVk> layout, ImageAspectFlags<kVk> aspectFlags, uint32_t index)
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
			1,
			aspectFlags);

		myImageLayout = layout;
	}
}

template <>
QueuePresentInfo<kVk> Frame<kVk>::PreparePresent()
{
	return QueuePresentInfo<kVk>{{{}, {}}, {}, {GetDesc().index}, {}};
}
