#pragma once

#include "device.h"
#include "types.h"

#include <optional>
#include <tuple>
#include <vector>

template <GraphicsApi G>
struct RenderTargetCreateDesc
{
	Extent2d<G> extent{};
	std::vector<Format<G>> colorImageFormats;
	std::vector<ImageLayout<G>> colorImageLayouts;
	std::vector<ImageHandle<G>> colorImages;
	Format<G> depthStencilImageFormat{};
	ImageLayout<G> depthStencilImageLayout{};
	ImageHandle<G> depthStencilImage{}; // optional
	uint32_t layerCount = 1;
	bool useDefaultInitialization = true; // create default render passes & framebuffer objects
};

template <GraphicsApi G>
struct IRenderTarget
{
	virtual const RenderTargetCreateDesc<G>& getRenderTargetDesc() const = 0;

	virtual ImageLayout<G> getColorImageLayout(uint32_t index) const = 0;
	virtual ImageLayout<G> getDepthStencilImageLayout() const = 0;

	 // TODO: make these two a single scoped call
	virtual RenderPassBeginInfo<G> begin(CommandBufferHandle<G> cmd, SubpassContents<G> contents) = 0;
	virtual void end(CommandBufferHandle<G> cmd) = 0;
	//

	virtual void blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<G>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter<G> filter = {}) = 0;

	virtual void clearSingleAttachment(
		CommandBufferHandle<G> cmd, const ClearAttachment<G>& clearAttachment) const = 0;
	virtual void clearAllAttachments(
		CommandBufferHandle<G> cmd,
		const ClearColorValue<G>& color = {},
		const ClearDepthStencilValue<G>& depthStencil = {}) const = 0;

	virtual void
	clearColor(CommandBufferHandle<G> cmd, const ClearColorValue<G>& color, uint32_t index) = 0;
	virtual void clearDepthStencil(
		CommandBufferHandle<G> cmd, const ClearDepthStencilValue<G>& depthStencil) = 0;

	virtual void
	transitionColor(CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index) = 0;
	virtual void transitionDepthStencil(CommandBufferHandle<G> cmd, ImageLayout<G> layout) = 0;
};

template <GraphicsApi G>
using RenderTargetHandle = std::pair<RenderPassHandle<G>, FramebufferHandle<G>>;

template <GraphicsApi G>
class RenderTarget : public IRenderTarget<G>, public DeviceObject<G>
{
public:
	~RenderTarget();

	operator auto() { return internalGetValues(); };

	virtual RenderPassBeginInfo<G> begin(CommandBufferHandle<G> cmd, SubpassContents<G> contents) final;
	virtual void end(CommandBufferHandle<G> cmd) override;

	virtual void blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<G>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter<G> filter = {}) final;

	virtual void clearSingleAttachment(
		CommandBufferHandle<G> cmd, const ClearAttachment<G>& clearAttachment) const final;
	virtual void clearAllAttachments(
		CommandBufferHandle<G> cmd,
		const ClearColorValue<G>& color = {},
		const ClearDepthStencilValue<G>& depthStencil = {}) const final;

	virtual void
	clearColor(CommandBufferHandle<G> cmd, const ClearColorValue<G>& color, uint32_t index) final;
	virtual void clearDepthStencil(
		CommandBufferHandle<G> cmd, const ClearDepthStencilValue<G>& depthStencil) final;

	auto getAttachment(uint32_t index) const noexcept { return myAttachments[index]; }
	const auto& getAttachmentDesc(uint32_t index) const noexcept
	{
		return myAttachmentDescs[index];
	}

	void addSubpassDescription(SubpassDescription<G>&& description);
	void addSubpassDependency(SubpassDependency<G>&& dependency);
	void nextSubpass(CommandBufferHandle<G> cmd, SubpassContents<G> contents);
	void resetSubpasses();

	void setColorAttachmentLoadOp(uint32_t index, AttachmentLoadOp<G> op);
	void setColorAttachmentStoreOp(uint32_t index, AttachmentStoreOp<G> op);
	void setDepthStencilAttachmentLoadOp(AttachmentLoadOp<G> op);
	void setDepthStencilAttachmentStoreOp(AttachmentStoreOp<G> op);

protected:
	constexpr RenderTarget() noexcept = default;
	RenderTarget(RenderTarget<G>&& other) noexcept;
	RenderTarget(
		const std::shared_ptr<Device<G>>& device,
		const RenderTargetCreateDesc<G>& desc);

	RenderTarget& operator=(RenderTarget&& other) noexcept;

	void swap(RenderTarget& rhs) noexcept;

private:
	uint64_t internalCalculateHashKey(const RenderTargetCreateDesc<Vk>& desc) const;

	RenderTargetHandle<G> internalCreateRenderPassAndFrameBuffer(
		uint64_t hashKey, const RenderTargetCreateDesc<Vk>& desc);

	void internalInitializeAttachments(const RenderTargetCreateDesc<G>& desc);
	void internalInitializeDefaultRenderPass(const RenderTargetCreateDesc<G>& desc);

	void internalUpdateAttachments(const RenderTargetCreateDesc<G>& desc);
	void internalUpdateRenderPasses(const RenderTargetCreateDesc<G>& desc);

	const RenderTargetHandle<G>& internalUpdateMap(const RenderTargetCreateDesc<G>& desc);
	const RenderTargetHandle<G>& internalGetValues();

	std::vector<ImageViewHandle<G>> myAttachments;
	std::vector<AttachmentDescription<G>> myAttachmentDescs;
	std::vector<AttachmentReference<G>> myAttachmentsReferences;
	std::vector<SubpassDescription<G>> mySubPassDescs;
	std::vector<SubpassDependency<G>> mySubPassDependencies;

	UnorderedMap<uint64_t, RenderTargetHandle<G>> myCache; // todo: consider making global
};

#include "rendertarget.inl"
