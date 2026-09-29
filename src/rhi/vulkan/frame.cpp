#include <rhi/frame.h>
#include <rhi/rhiapplication.h>
#include <rhi/rendertarget.h>
#include <rhi/vulkan/utils.h>

#include <string_view>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Frame<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Frame<kVk>);

template <>
Frame<kVk>::Frame(CreateDescType&& desc)
	: RenderTarget<Frame<kVk>, kVk>(std::forward<CreateDescType>(desc))
	, myImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
{}

template <>
void Frame<kVk>::Swap(Frame& rhs) noexcept
{
	RenderTarget<Frame<kVk>, kVk>::Swap(rhs);
	std::swap(myImageLayout, rhs.myImageLayout);
}

template <>
Frame<kVk>::Frame(Frame&& other) noexcept
{
	Swap(other);
}

template <>
Frame<kVk>& Frame<kVk>::operator=(Frame&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
ImageLayout<kVk> Frame<kVk>::GetLayout(uint32_t) const
{
	return myImageLayout;
}

template <>
void Frame<kVk>::End(CommandBufferHandle<kVk> cmd)
{
	RenderTarget<Frame<kVk>, kVk>::End(cmd);

	myImageLayout = GetAttachmentDescs()[0].finalLayout;
}

template <>
void Frame<kVk>::Transition(CommandBufferHandle<kVk> cmd, ImageLayout<kVk> layout, ImageAspectFlags<kVk> aspectFlags, uint32_t index)
{
	ZoneScopedN("Frame::TransitionColor");

	ENSURE(index == 0);

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
	return QueuePresentInfo<kVk>{.swapchains = {}, .imageIndices = { GetDesc().index }, .results = {}};
}

} // namespace rhi
