#pragma once

#include <rhi/deviceobject.h>
#include <rhi/types.h>

#include <optional>
#include <variant>
#include <vector>

namespace rhi
{

template <GraphicsApi G>
struct RenderTargetCreateDesc : DeviceObjectCreateDesc<G>
{
	Extent2d extent{};
	std::vector<Format> imageFormats;
	std::vector<ImageLayout> imageLayouts;
	std::vector<ImageAspect> imageAspectFlags;
	std::vector<ImageHandle<G>> images;
	std::vector<ClearValue> clearValues{{.color = {0.2F, 0.2F, 0.2F, 1.0F}}, {.depth = 1.0F, .stencil = 0}};
	uint32_t layerCount = 1;
	bool useDynamicRendering = true;
};

template <GraphicsApi G>
struct DynamicRenderingInfo
{
	RenderingInfo<G> renderInfo;
	CommandBufferInheritanceRenderingInfo<G> inheritanceInfo;
};

template <GraphicsApi G>
using RenderTargetBeginInfo = std::variant<DynamicRenderingInfo<G>, RenderPassBeginInfo<G>>;

template <GraphicsApi G>
using RenderTargetPassHandle = std::pair<RenderPassHandle<G>, FramebufferHandle<G>>;

template <GraphicsApi G>
struct IRenderTarget
{
	[[nodiscard]] virtual RenderTargetPassHandle<G> GetHandle() = 0;
	[[nodiscard]] virtual Extent2d GetExtent() const = 0;
	[[nodiscard]] virtual std::span<const ImageHandle<G>> GetImages() const = 0;
	[[nodiscard]] virtual std::span<const ImageViewHandle<G>> GetAttachments() const = 0;
	[[nodiscard]] virtual std::span<const AttachmentDescription<G>> GetAttachmentDescs() const = 0;
	[[nodiscard]] virtual ImageLayout GetLayout(uint32_t index) const = 0;
	[[nodiscard]] virtual const std::optional<PipelineRenderingCreateInfo<G>>& GetPipelineRenderingCreateInfo() const = 0;

	// TODO(djohansson): make these two a single scoped call
	[[maybe_unused]] virtual const RenderTargetBeginInfo<G>& Begin(CommandBufferHandle<G> cmd, SubpassContents contents) = 0;
	virtual void End(CommandBufferHandle<G> cmd) = 0;
	//

	virtual void ClearAll(
		CommandBufferHandle<G> cmd,
		std::span<const ClearValue> values) const = 0;

	virtual void Blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<G>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter filter) = 0;

	virtual void Copy(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<G>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex) = 0;

	virtual void Clear(
		CommandBufferHandle<G> cmd,
		const ClearValue& value,
		uint32_t index) = 0;

	virtual void Transition(
		CommandBufferHandle<G> cmd,
		ImageLayout layout,
		ImageAspect aspectFlags,
		uint32_t index) = 0;

	virtual void SetLoadOp(LoadOp loadOp, uint32_t index, LoadOp stencilLoadOp = {}) = 0; //NOLINT(google-default-arguments)
	virtual void SetStoreOp(StoreOp storeOp, uint32_t index, StoreOp stencilStoreOp = {}) = 0; //NOLINT(google-default-arguments)
};

template <typename DerivedType, GraphicsApi G/* = Object<DerivedType>::Api*/>
class RenderTarget : public IRenderTarget<G>, public DeviceObject<DerivedType> {};

} // namespace rhi

#include "rendertarget.inl"
