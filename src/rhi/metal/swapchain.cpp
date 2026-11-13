// the Metal backend's swapchains come with presentation (docs/backends-plan.md, step 6): the RHI holds its windows'
// swapchains, none when headless, so only one never made is destroyed here; its render target members (the destructor
// emits the vtable, which names them) fail where they are called
#include <rhi/metal/backend.h>

#include <rhi/rhi.h>
#include <rhi/swapchain.h>

#include <core/assert.h>

#include <optional>
#include <span>

namespace rhi
{

namespace swapchain
{

[[noreturn]] static void NotYet()
{
	ENSUREF(false, "the Metal backend's swapchains come with presentation (docs/backends-plan.md, step 6)");
	__builtin_trap();
}

} // namespace swapchain

template <>
Swapchain<kMetal>::~Swapchain()
{
	if (IsValid())
		swapchain::NotYet();
}

template <>
RenderTargetPassHandle<kMetal> Swapchain<kMetal>::GetHandle()
{
	swapchain::NotYet();
}

template <>
Extent2d Swapchain<kMetal>::GetExtent() const
{
	swapchain::NotYet();
}

template <>
std::span<const ImageHandle<kMetal>> Swapchain<kMetal>::GetImages() const
{
	swapchain::NotYet();
}

template <>
std::span<const ImageViewHandle<kMetal>> Swapchain<kMetal>::GetAttachments() const
{
	swapchain::NotYet();
}

template <>
std::span<const AttachmentDescription<kMetal>> Swapchain<kMetal>::GetAttachmentDescs() const
{
	swapchain::NotYet();
}

template <>
ImageLayout Swapchain<kMetal>::GetLayout(uint32_t /*index*/) const
{
	swapchain::NotYet();
}

template <>
const std::optional<PipelineRenderingCreateInfo<kMetal>>& Swapchain<kMetal>::GetPipelineRenderingCreateInfo() const
{
	swapchain::NotYet();
}

template <>
const RenderTargetBeginInfo<kMetal>& Swapchain<kMetal>::Begin(CommandBufferHandle<kMetal> /*cmd*/, SubpassContents /*contents*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kMetal>::End(CommandBufferHandle<kMetal> /*cmd*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kMetal>::ClearAll(CommandBufferHandle<kMetal> /*cmd*/, std::span<const ClearValue> /*values*/) const
{
	swapchain::NotYet();
}

template <>
void Swapchain<kMetal>::Blit(
	CommandBufferHandle<kMetal> /*cmd*/,
	const IRenderTarget<kMetal>& /*srcRenderTarget*/,
	const ImageSubresourceLayers<kMetal>& /*srcSubresource*/,
	uint32_t /*srcIndex*/,
	const ImageSubresourceLayers<kMetal>& /*dstSubresource*/,
	uint32_t /*dstIndex*/,
	Filter /*filter*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kMetal>::Copy(
	CommandBufferHandle<kMetal> /*cmd*/,
	const IRenderTarget<kMetal>& /*srcRenderTarget*/,
	const ImageSubresourceLayers<kMetal>& /*srcSubresource*/,
	uint32_t /*srcIndex*/,
	const ImageSubresourceLayers<kMetal>& /*dstSubresource*/,
	uint32_t /*dstIndex*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kMetal>::Clear(CommandBufferHandle<kMetal> /*cmd*/, const ClearValue& /*value*/, uint32_t /*index*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kMetal>::ClearRect(CommandBufferHandle<kMetal> /*cmd*/, const ClearValue& /*value*/, uint32_t /*index*/, const Rect& /*rect*/) const
{
	swapchain::NotYet();
}

template <>
void Swapchain<kMetal>::Transition(CommandBufferHandle<kMetal> /*cmd*/, ImageLayout /*layout*/, ImageAspect /*aspectFlags*/, uint32_t /*index*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kMetal>::SetLoadOp(LoadOp /*loadOp*/, uint32_t /*index*/, LoadOp /*stencilLoadOp*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kMetal>::SetStoreOp(StoreOp /*storeOp*/, uint32_t /*index*/, StoreOp /*stencilStoreOp*/)
{
	swapchain::NotYet();
}

template <>
void Swapchain<kMetal>::SetClearValue(const ClearValue& /*value*/, uint32_t /*index*/)
{
	swapchain::NotYet();
}

} // namespace rhi
