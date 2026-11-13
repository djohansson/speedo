// the Metal backend's swapchain frames (see swapchain.cpp): a render target over one of the swapchain's images, its
// layout kept for rhi's bookkeeping alone (Metal has none), and its index for the present
#include <rhi/metal/backend.h>

#include <rhi/frame.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Frame<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Frame<kMetal>);

template <>
Frame<kMetal>::Frame(CreateDescType&& desc)
	: RenderTarget<Frame<kMetal>, kMetal>(std::forward<CreateDescType>(desc))
	, myImageLayout(ImageLayout::kUndefined)
{}

template <>
void Frame<kMetal>::Swap(Frame& rhs) noexcept
{
	RenderTarget<Frame<kMetal>, kMetal>::Swap(rhs);
	std::swap(myImageLayout, rhs.myImageLayout);
}

template <>
Frame<kMetal>::Frame(Frame&& other) noexcept
{
	Swap(other);
}

template <>
Frame<kMetal>& Frame<kMetal>::operator=(Frame&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
ImageLayout Frame<kMetal>::GetLayout(uint32_t /*index*/) const
{
	return myImageLayout;
}

template <>
void Frame<kMetal>::End(CommandBufferHandle<kMetal> cmd)
{
	RenderTarget<Frame<kMetal>, kMetal>::End(cmd);
	myImageLayout = ImageLayout::kColorAttachment; // as Vulkan's render pass leaves it
}

template <>
void Frame<kMetal>::Transition(CommandBufferHandle<kMetal> /*cmd*/, ImageLayout layout, ImageAspect /*aspectFlags*/, uint32_t index)
{
	ENSURE(index == 0);
	myImageLayout = layout;
}

template <>
QueuePresentInfo<kMetal> Frame<kMetal>::PreparePresent()
{
	return QueuePresentInfo<kMetal>{.swapchains = {}, .imageIndices = {GetDesc().index}, .results = {}};
}

} // namespace rhi
