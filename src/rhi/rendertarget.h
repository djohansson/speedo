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
	virtual const RenderTargetCreateDesc<G>& GetRenderTargetDesc() const = 0;

	virtual ImageLayout<G> GetColorImageLayout(uint32_t index) const = 0;
	virtual ImageLayout<G> GetDepthStencilImageLayout() const = 0;

	 // TODO: make these two a single scoped call
	virtual RenderPassBeginInfo<G> Begin(CommandBufferHandle<G> cmd, SubpassContents<G> contents) = 0;
	virtual void End(CommandBufferHandle<G> cmd) = 0;
	//

	virtual void Blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<G>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter<G> filter = {}) = 0;

	virtual void ClearSingleAttachment(
		CommandBufferHandle<G> cmd, const ClearAttachment<G>& clearAttachment) const = 0;
	virtual void ClearAllAttachments(
		CommandBufferHandle<G> cmd,
		const ClearColorValue<G>& color = {},
		const ClearDepthStencilValue<G>& depthStencil = {}) const = 0;

	virtual void
	ClearColor(CommandBufferHandle<G> cmd, const ClearColorValue<G>& color, uint32_t index) = 0;
	virtual void ClearDepthStencil(
		CommandBufferHandle<G> cmd, const ClearDepthStencilValue<G>& depthStencil) = 0;

	virtual void
	TransitionColor(CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index) = 0;
	virtual void TransitionDepthStencil(CommandBufferHandle<G> cmd, ImageLayout<G> layout) = 0;
};

template <GraphicsApi G>
using RenderTargetHandle = std::pair<RenderPassHandle<G>, FramebufferHandle<G>>;

template <GraphicsApi G>
class RenderTarget : public IRenderTarget<G>, public DeviceObject<G>
{
public:
	~RenderTarget();

	operator auto() { return InternalGetValues(); };

	virtual RenderPassBeginInfo<G> Begin(CommandBufferHandle<G> cmd, SubpassContents<G> contents) final;
	virtual void End(CommandBufferHandle<G> cmd) override;

	virtual void Blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<G>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter<G> filter = {}) final;

	virtual void ClearSingleAttachment(
		CommandBufferHandle<G> cmd, const ClearAttachment<G>& clearAttachment) const final;
	virtual void ClearAllAttachments(
		CommandBufferHandle<G> cmd,
		const ClearColorValue<G>& color = {},
		const ClearDepthStencilValue<G>& depthStencil = {}) const final;

	virtual void
	ClearColor(CommandBufferHandle<G> cmd, const ClearColorValue<G>& color, uint32_t index) final;
	virtual void ClearDepthStencil(
		CommandBufferHandle<G> cmd, const ClearDepthStencilValue<G>& depthStencil) final;

	auto GetAttachment(uint32_t index) const noexcept { return myAttachments[index]; }
	const auto& GetAttachmentDesc(uint32_t index) const noexcept
	{
		return myAttachmentDescs[index];
	}

	void AddSubpassDescription(SubpassDescription<G>&& description);
	void AddSubpassDependency(SubpassDependency<G>&& dependency);
	void NextSubpass(CommandBufferHandle<G> cmd, SubpassContents<G> contents);
	void ResetSubpasses();

	void SetColorAttachmentLoadOp(uint32_t index, AttachmentLoadOp<G> op);
	void SetColorAttachmentStoreOp(uint32_t index, AttachmentStoreOp<G> op);
	void SetDepthStencilAttachmentLoadOp(AttachmentLoadOp<G> op);
	void SetDepthStencilAttachmentStoreOp(AttachmentStoreOp<G> op);

protected:
	constexpr RenderTarget() noexcept = default;
	RenderTarget(RenderTarget<G>&& other) noexcept;
	RenderTarget(
		const std::shared_ptr<Device<G>>& device,
		const RenderTargetCreateDesc<G>& desc);

	RenderTarget& operator=(RenderTarget&& other) noexcept;

	void Swap(RenderTarget& rhs) noexcept;

private:
	uint64_t InternalCalculateHashKey(const RenderTargetCreateDesc<kVk>& desc) const;

	RenderTargetHandle<G> InternalCreateRenderPassAndFrameBuffer(
		uint64_t hashKey, const RenderTargetCreateDesc<kVk>& desc);

	void InternalInitializeAttachments(const RenderTargetCreateDesc<G>& desc);
	void InternalInitializeDefaultRenderPass(const RenderTargetCreateDesc<G>& desc);

	void InternalUpdateAttachments(const RenderTargetCreateDesc<G>& desc);
	void InternalUpdateRenderPasses(const RenderTargetCreateDesc<G>& desc);

	const RenderTargetHandle<G>& InternalUpdateMap(const RenderTargetCreateDesc<G>& desc);
	const RenderTargetHandle<G>& InternalGetValues();

	std::vector<ImageViewHandle<G>> myAttachments;
	std::vector<AttachmentDescription<G>> myAttachmentDescs;
	std::vector<AttachmentReference<G>> myAttachmentsReferences;
	std::vector<SubpassDescription<G>> mySubPassDescs;
	std::vector<SubpassDependency<G>> mySubPassDependencies;

	UnorderedMap<uint64_t, RenderTargetHandle<G>> myCache; // todo: consider making global
};

#include "rendertarget.inl"
