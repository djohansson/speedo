// the D3D12 backend's swapchain frames (see swapchain.cpp): a render target over one of the swapchain's buffers, its
// layout, and its index for the present. the layout's barriers come with the swapchain (step 5, docs/backends-plan.md):
// until then a frame is made by none, and keeps its layout for rhi's bookkeeping alone
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
void Frame<kD3D12>::Transition(CommandBufferHandle<kD3D12> /*cmd*/, ImageLayout /*layout*/, ImageAspect /*aspectFlags*/, uint32_t /*index*/)
{
	ENSUREF(false, "the D3D12 backend has no swapchain yet (docs/backends-plan.md, step 5)");
}

template <>
QueuePresentInfo<kD3D12> Frame<kD3D12>::PreparePresent()
{
	return QueuePresentInfo<kD3D12>{.swapchains = {}, .imageIndices = {GetDesc().index}, .results = {}};
}

} // namespace rhi
