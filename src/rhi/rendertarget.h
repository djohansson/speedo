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
	std::vector<Format<G>> imageFormats;
	std::vector<ImageLayout<G>> imageLayouts;
	std::vector<ImageHandle<G>> images;
	uint32_t layerCount = 1;
	bool useDynamicRendering = true;
};

template <GraphicsApi G>
struct IRenderTarget
{
	[[nodiscard]] virtual const RenderTargetCreateDesc<G>& GetRenderTargetDesc() const = 0;
	[[nodiscard]] virtual std::span<const ImageViewHandle<G>> GetAttachments() const = 0;
	[[nodiscard]] virtual std::span<const AttachmentDescription<G>> GetAttachmentDescs() const = 0;
	[[nodiscard]] virtual ImageLayout<G> GetLayout(uint32_t index) const = 0;

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

	virtual void ClearAll(
		CommandBufferHandle<G> cmd,
		std::span<const ClearValue<G>> values) const = 0;

	virtual void Clear(
		CommandBufferHandle<G> cmd,
		const ClearValue<G>& value,
		uint32_t index) = 0;

	virtual void Transition(
		CommandBufferHandle<G> cmd,
		ImageLayout<G> layout,
		uint32_t index) = 0;

	virtual void SetLoadOp(AttachmentLoadOp<G> loadOp, uint32_t index) = 0;
	virtual void SetStoreOp(AttachmentStoreOp<G> storeOp, uint32_t index) = 0;
};

template <GraphicsApi G>
using RenderTargetHandle = std::pair<RenderPassHandle<G>, FramebufferHandle<G>>;

template <GraphicsApi G>
class RenderTarget : public IRenderTarget<G>, public DeviceObject<G>
{
public:
	~RenderTarget() override;

	[[nodiscard]] operator auto() { return InternalGetValues(); };//NOLINT(google-explicit-constructor)

	std::span<const ImageViewHandle<G>> GetAttachments() const final { return myAttachments; }
	std::span<const AttachmentDescription<G>> GetAttachmentDescs() const final { return myAttachmentDescs; }

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

	void ClearAll(
		CommandBufferHandle<G> cmd,
		std::span<const ClearValue<G>> values) const final;

	void Clear(
		CommandBufferHandle<G> cmd,
		const ClearValue<G>& value,
		uint32_t index) final;

	void SetLoadOp(AttachmentLoadOp<G> loadOp, uint32_t index) final;
	void SetStoreOp(AttachmentStoreOp<G> storeOp, uint32_t index) final;

	void AddSubpassDescription(SubpassDescription<G>&& description);
	void AddSubpassDependency(SubpassDependency<G>&& dependency);
	void NextSubpass(CommandBufferHandle<G> cmd, SubpassContents<G> contents);
	void ResetSubpasses();

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

	[[maybe_unused]] const RenderTargetHandle<G>& InternalUpdateMap(const RenderTargetCreateDesc<G>& desc);
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
