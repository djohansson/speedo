// the D3D12 backend's swapchain: a DXGI flip model swapchain on the window (docs/backends-plan.md, step 5). until then
// none is made (the RHI is headless, see rhi.cpp): its render target's members, which the RHI's swapchains reference
// (its vtable), forward to the current frame as Metal's do, or fail hard
#include <rhi/d3d12/backend.h>

#include <rhi/swapchain.h>

#include <core/assert.h>

namespace rhi
{

namespace swapchain
{

[[noreturn]] static void NotYet()
{
	ENSUREF(false, "the D3D12 backend has no swapchain yet (docs/backends-plan.md, step 5)");
	std::abort();
}

} // namespace swapchain

template <>
const RenderTargetBeginInfo<kD3D12>& Swapchain<kD3D12>::Begin(CommandBufferHandle<kD3D12> /*cmd*/, SubpassContents /*contents*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::End(CommandBufferHandle<kD3D12> /*cmd*/)
{
	swapchain::NotYet();
}

template <>
RenderTargetPassHandle<kD3D12> Swapchain<kD3D12>::GetHandle()
{
	return myFrames[myFrameIndex].GetHandle();
}

template <>
Extent2d Swapchain<kD3D12>::GetExtent() const
{
	return myFrames[myFrameIndex].GetExtent();
}

template <>
std::span<const ImageHandle<kD3D12>> Swapchain<kD3D12>::GetImages() const
{
	return myFrames[myFrameIndex].GetImages();
}

template <>
std::span<const ImageViewHandle<kD3D12>> Swapchain<kD3D12>::GetAttachments() const
{
	return myFrames[myFrameIndex].GetAttachments();
}

template <>
std::span<const AttachmentDescription<kD3D12>> Swapchain<kD3D12>::GetAttachmentDescs() const
{
	return myFrames[myFrameIndex].GetAttachmentDescs();
}

template <>
ImageLayout Swapchain<kD3D12>::GetLayout(uint32_t index) const
{
	return myFrames[myFrameIndex].GetLayout(index);
}

template <>
const std::optional<PipelineRenderingCreateInfo<kD3D12>>& Swapchain<kD3D12>::GetPipelineRenderingCreateInfo() const
{
	return myFrames[myFrameIndex].GetPipelineRenderingCreateInfo();
}

template <>
void Swapchain<kD3D12>::Blit(
	CommandBufferHandle<kD3D12> /*cmd*/,
	const IRenderTarget<kD3D12>& /*srcRenderTarget*/,
	const ImageSubresourceLayers<kD3D12>& /*srcSubresource*/,
	uint32_t /*srcIndex*/,
	const ImageSubresourceLayers<kD3D12>& /*dstSubresource*/,
	uint32_t /*dstIndex*/,
	Filter /*filter*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::Copy(
	CommandBufferHandle<kD3D12> /*cmd*/,
	const IRenderTarget<kD3D12>& /*srcRenderTarget*/,
	const ImageSubresourceLayers<kD3D12>& /*srcSubresource*/,
	uint32_t /*srcIndex*/,
	const ImageSubresourceLayers<kD3D12>& /*dstSubresource*/,
	uint32_t /*dstIndex*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::ClearAll(CommandBufferHandle<kD3D12> /*cmd*/, std::span<const ClearValue> /*values*/) const
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::ClearRect(CommandBufferHandle<kD3D12> /*cmd*/, const ClearValue& /*value*/, uint32_t /*index*/, const Rect& /*rect*/) const
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::Clear(CommandBufferHandle<kD3D12> /*cmd*/, const ClearValue& /*value*/, uint32_t /*index*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::Transition(CommandBufferHandle<kD3D12> /*cmd*/, ImageLayout /*layout*/, ImageAspect /*aspectFlags*/, uint32_t /*index*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::SetLoadOp(LoadOp /*loadOp*/, uint32_t /*index*/, LoadOp /*stencilLoadOp*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::SetStoreOp(StoreOp /*storeOp*/, uint32_t /*index*/, StoreOp /*stencilStoreOp*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::SetClearValue(const ClearValue& /*value*/, uint32_t /*index*/)
{
	swapchain::NotYet();
}

template <>
Swapchain<kD3D12>::~Swapchain()
{
	ENSUREF(!IsValid(), "the D3D12 backend has no swapchain yet (docs/backends-plan.md, step 5)");
}

template <>
void Swapchain<kD3D12>::OnPresentResult(PresentResult /*result*/) noexcept
{
	swapchain::NotYet();
}

template <>
Extent2d Swapchain<kD3D12>::QuerySurfaceExtent()
{
	swapchain::NotYet();
}

template <>
FlipResult<kD3D12> Swapchain<kD3D12>::Flip()
{
	swapchain::NotYet();
}

template <>
QueuePresentInfo<kD3D12> Swapchain<kD3D12>::PreparePresent()
{
	swapchain::NotYet();
}

template <>
bool Swapchain<kD3D12>::WaitPresent(uint64_t /*presentId*/, uint64_t /*timeout*/) const
{
	swapchain::NotYet();
}

template <>
bool Swapchain<kD3D12>::WaitPresented(uint64_t /*timeout*/) const
{
	swapchain::NotYet();
}

template <>
std::pair<uint32_t, uint32_t> Swapchain<kD3D12>::GetImageCountLimits() const
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::Configure(PresentationMode /*mode*/, uint32_t /*imageCount*/)
{
	swapchain::NotYet();
}

template <>
PresentationMode Swapchain<kD3D12>::GetPresentationMode() const
{
	swapchain::NotYet();
}

template <>
uint32_t Swapchain<kD3D12>::GetPresentationModes() const
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::SetExclusiveFullscreen(WindowHandle /*window*/) noexcept
{
	swapchain::NotYet();
}

template <>
bool Swapchain<kD3D12>::CreateSwapchain()
{
	swapchain::NotYet();
}

template <>
void Swapchain<kD3D12>::Release()
{
	swapchain::NotYet();
}

} // namespace rhi
