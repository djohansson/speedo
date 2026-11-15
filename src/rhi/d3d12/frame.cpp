// the D3D12 backend's swapchain frames (see swapchain.cpp): a render target over one of the swapchain's images, its
// layout (rhi's: the image tracks D3D12's, d3d12::TransitionImage), and its index for the present
#include <rhi/d3d12/backend.h>

#include <rhi/frame.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Frame<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Frame<kD3D12>);

template <>
Frame<kD3D12>::Frame(CreateDescType&& desc)
	: RenderTarget<Frame<kD3D12>, kD3D12>(std::forward<CreateDescType>(desc))
	, myImageLayout(ImageLayout::kUndefined)
{}

template <>
void Frame<kD3D12>::Swap(Frame& rhs) noexcept
{
	RenderTarget<Frame<kD3D12>, kD3D12>::Swap(rhs);
	std::swap(myImageLayout, rhs.myImageLayout);
}

template <>
Frame<kD3D12>::Frame(Frame&& other) noexcept
{
	Swap(other);
}

template <>
Frame<kD3D12>& Frame<kD3D12>::operator=(Frame&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
ImageLayout Frame<kD3D12>::GetLayout(uint32_t /*index*/) const
{
	return myImageLayout;
}

template <>
void Frame<kD3D12>::End(CommandBufferHandle<kD3D12> cmd)
{
	RenderTarget<Frame<kD3D12>, kD3D12>::End(cmd);
	myImageLayout = ImageLayout::kColorAttachment; // as Vulkan's render pass leaves it
}

template <>
void Frame<kD3D12>::Transition(CommandBufferHandle<kD3D12> cmd, ImageLayout layout, ImageAspect /*aspectFlags*/, uint32_t index)
{
	ENSURE(index == 0);
	d3d12::TransitionImage(*cmd, *GetDesc().images[0], d3d12::ToD3D12Layout(layout), myImageLayout == ImageLayout::kUndefined);
	myImageLayout = layout;
}

template <>
QueuePresentInfo<kD3D12> Frame<kD3D12>::PreparePresent()
{
	return QueuePresentInfo<kD3D12>{.swapchains = {}, .imageIndices = {GetDesc().index}, .results = {}};
}

} // namespace rhi
