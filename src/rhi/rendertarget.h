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
	bool useDynamicRendering = true;
};

template <GraphicsApi G>
struct IRenderTarget
{
	[[nodiscard]] virtual const RenderTargetCreateDesc<G>& GetRenderTargetDesc() const = 0;

	[[nodiscard]] virtual ImageViewHandle<G> GetColor(uint32_t index) const = 0;
	[[nodiscard]] virtual ImageViewHandle<G> GetDepthStencil() const = 0;

	[[nodiscard]] virtual ImageLayout<G> GetColorLayout(uint32_t index) const = 0;
	[[nodiscard]] virtual ImageLayout<G> GetDepthStencilLayout() const = 0;

	// TODO(djohansson): make these two a single scoped call
	[[maybe_unused]] virtual const RenderInfo<G>& Begin(CommandBufferHandle<G> cmd, SubpassContents<G> contents, std::span<const VkClearValue> clearValues) = 0;
	virtual void End(CommandBufferHandle<G> cmd) = 0;
	//

	virtual void Blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<G>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter<G> filter) = 0;

	virtual void ClearSingleAttachment(
		CommandBufferHandle<G> cmd, const ClearAttachment<G>& clearAttachment) const = 0;
	virtual void ClearAllAttachments(
		CommandBufferHandle<G> cmd,
		const ClearColorValue<G>& color,
		const ClearDepthStencilValue<G>& depthStencil) const = 0;

	virtual void
	ClearColor(CommandBufferHandle<G> cmd, const ClearColorValue<G>& color, uint32_t index) = 0;
	virtual void ClearDepthStencil(
		CommandBufferHandle<G> cmd, const ClearDepthStencilValue<G>& depthStencil) = 0;

	virtual void
	TransitionColor(CommandBufferHandle<G> cmd, ImageLayout<G> layout, uint32_t index) = 0;
	virtual void TransitionDepthStencil(CommandBufferHandle<G> cmd, ImageLayout<G> layout) = 0;

	[[nodiscard]] virtual AttachmentLoadOp<G> GetColorLoadOp(uint32_t index) const = 0;
	[[nodiscard]] virtual AttachmentStoreOp<G> GetColorStoreOp(uint32_t index) const = 0;
	[[nodiscard]] virtual AttachmentLoadOp<G> GetDepthStencilLoadOp() const = 0;
	[[nodiscard]] virtual AttachmentStoreOp<G> GetDepthStencilStoreOp() const = 0;

	virtual void SetColorLoadOp(uint32_t index, AttachmentLoadOp<G> loadOp) = 0;
	virtual void SetColorStoreOp(uint32_t index, AttachmentStoreOp<G> storeOp) = 0;
	virtual void SetDepthStencilLoadOp(AttachmentLoadOp<G> loadOp) = 0;
	virtual void SetDepthStencilStoreOp(AttachmentStoreOp<G> storeOp) = 0;
};

template <GraphicsApi G>
using RenderTargetHandle = std::pair<RenderPassHandle<G>, FramebufferHandle<G>>;

template <GraphicsApi G>
class RenderTarget : public IRenderTarget<G>, public DeviceObject<G>
{
public:
	~RenderTarget() override;

	[[nodiscard]] operator auto() { return InternalGetValues(); };//NOLINT(google-explicit-constructor)

	const RenderInfo<G>& Begin(CommandBufferHandle<G> cmd, SubpassContents<G> contents, std::span<const VkClearValue> clearValues) final;
	void End(CommandBufferHandle<G> cmd) override;

	void Blit(
		CommandBufferHandle<G> cmd,
		const IRenderTarget<G>& srcRenderTarget,
		const ImageSubresourceLayers<G>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<G>& dstSubresource,
		uint32_t dstIndex,
		Filter<G> filter) final;

	void ClearSingleAttachment(
		CommandBufferHandle<G> cmd, const ClearAttachment<G>& clearAttachment) const final;
	void ClearAllAttachments(
		CommandBufferHandle<G> cmd,
		const ClearColorValue<G>& color,
		const ClearDepthStencilValue<G>& depthStencil) const final;

	void
	ClearColor(CommandBufferHandle<G> cmd, const ClearColorValue<G>& color, uint32_t index) final;
	void ClearDepthStencil(
		CommandBufferHandle<G> cmd, const ClearDepthStencilValue<G>& depthStencil) final;

	ImageViewHandle<G> GetColor(uint32_t index) const override { return GetAttachment(index); }
	ImageViewHandle<G> GetDepthStencil() const override { return GetAttachment(myAttachments.size() - 1); }

	[[nodiscard]] auto GetAttachment(uint32_t index) const noexcept { return myAttachments[index]; }
	[[nodiscard]] const auto& GetAttachmentDesc(uint32_t index) const noexcept
	{
		return myAttachmentDescs[index];
	}

	void AddSubpassDescription(SubpassDescription<G>&& description);
	void AddSubpassDependency(SubpassDependency<G>&& dependency);
	void NextSubpass(CommandBufferHandle<G> cmd, SubpassContents<G> contents);
	void ResetSubpasses();

	AttachmentLoadOp<G> GetColorLoadOp(uint32_t index) const final;
	AttachmentStoreOp<G> GetColorStoreOp(uint32_t index) const final;
	AttachmentLoadOp<G> GetDepthStencilLoadOp() const final;
	AttachmentStoreOp<G> GetDepthStencilStoreOp() const final;

	void SetColorLoadOp(uint32_t index, AttachmentLoadOp<G> loadOp) final;
	void SetColorStoreOp(uint32_t index, AttachmentStoreOp<G> storeOp) final;
	void SetDepthStencilLoadOp(AttachmentLoadOp<G> loadOp) final;
	void SetDepthStencilStoreOp(AttachmentStoreOp<G> storeOp) final;

	void Swap(RenderTarget& rhs) noexcept;

protected:
	constexpr RenderTarget() noexcept = default;
	RenderTarget(RenderTarget<G>&& other) noexcept;
	RenderTarget(
		const std::shared_ptr<Device<G>>& device,
		const RenderTargetCreateDesc<G>& desc);

	RenderTarget& operator=(RenderTarget&& other) noexcept;

private:
	[[nodiscard]] uint64_t InternalCalculateHashKey(const RenderTargetCreateDesc<kVk>& desc) const;

	[[nodiscard]] RenderTargetHandle<G> InternalCreateRenderPassAndFrameBuffer(
		uint64_t hashKey, const RenderTargetCreateDesc<kVk>& desc);

	void InternalInitializeAttachments(const RenderTargetCreateDesc<G>& desc);
	void InternalInitializeDefaultRenderPass(const RenderTargetCreateDesc<G>& desc);

	void InternalUpdateAttachments(const RenderTargetCreateDesc<G>& desc);
	void InternalUpdateRenderPasses(const RenderTargetCreateDesc<G>& desc);

	const RenderTargetHandle<G>& InternalUpdateMap(const RenderTargetCreateDesc<G>& desc);
	[[nodiscard]] const RenderTargetHandle<G>& InternalGetValues();

	std::vector<ImageViewHandle<G>> myAttachments;
	std::vector<AttachmentDescription<G>> myAttachmentDescs;
	std::vector<AttachmentReference<G>> myAttachmentsReferences;
	std::vector<SubpassDescription<G>> mySubPassDescs;
	std::vector<SubpassDependency<G>> mySubPassDependencies;

	std::optional<RenderInfo<G>> myRenderInfo;

	UnorderedMap<uint64_t, RenderTargetHandle<G>> myCache; // todo: consider making global
};

#include "rendertarget.inl"
