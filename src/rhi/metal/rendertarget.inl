namespace rhi
{

// the Metal backend's render target: its images' textures as attachments, and per attachment what a pass does with it
// (load and store actions, the clear value: the desc's), a render pass descriptor made at each Begin, whose render
// encoder is open until End. its members are in rhi/metal/rendertarget.cpp (no Metal headers here), instantiated there
// for the backend's render targets (RenderImageSet; the swapchain implements IRenderTarget itself)
template <typename DerivedType>
class RenderTarget<DerivedType, kMetal> : public IRenderTarget<kMetal>, public DeviceObject<DerivedType>
{
public:
	~RenderTarget() override;

	[[nodiscard]] RenderTargetPassHandle<kMetal> GetHandle() final { return {}; } // no render pass or framebuffer objects
	[[nodiscard]] Extent2d GetExtent() const final { return this->GetDesc().extent; }
	[[nodiscard]] std::span<const ImageHandle<kMetal>> GetImages() const final { return this->GetDesc().images; }
	[[nodiscard]] ImageLayout GetLayout(uint32_t index) const override { return this->GetDesc().imageLayouts[index]; }
	[[nodiscard]] std::span<const ImageViewHandle<kMetal>> GetAttachments() const final { return myAttachments; }
	[[nodiscard]] std::span<const AttachmentDescription<kMetal>> GetAttachmentDescs() const final { return myAttachmentDescs; }
	[[nodiscard]] const std::optional<PipelineRenderingCreateInfo<kMetal>>& GetPipelineRenderingCreateInfo() const final { return myPipelineRenderingCreateInfo; }

	[[maybe_unused]] const RenderTargetBeginInfo<kMetal>& Begin(CommandBufferHandle<kMetal> cmd, SubpassContents contents) final;
	void End(CommandBufferHandle<kMetal> cmd) override;

	void ClearAll(CommandBufferHandle<kMetal> cmd, std::span<const ClearValue> values) const final;
	void ClearRect(CommandBufferHandle<kMetal> cmd, const ClearValue& value, uint32_t index, const Rect& rect) const final;

	void Blit(
		CommandBufferHandle<kMetal> cmd,
		const IRenderTarget<kMetal>& srcRenderTarget,
		const ImageSubresourceLayers<kMetal>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<kMetal>& dstSubresource,
		uint32_t dstIndex,
		Filter filter) final;

	void Copy(
		CommandBufferHandle<kMetal> cmd,
		const IRenderTarget<kMetal>& srcRenderTarget,
		const ImageSubresourceLayers<kMetal>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<kMetal>& dstSubresource,
		uint32_t dstIndex) final;

	void Clear(CommandBufferHandle<kMetal> cmd, const ClearValue& value, uint32_t index) final;

	void SetLoadOp(LoadOp loadOp, uint32_t index, LoadOp stencilLoadOp = {}) final;
	void SetStoreOp(StoreOp storeOp, uint32_t index, StoreOp stencilStoreOp = {}) final;
	void SetClearValue(const ClearValue& value, uint32_t index) final;

	void Swap(RenderTarget& rhs) noexcept;

protected:
	constexpr RenderTarget() noexcept = default;
	RenderTarget(RenderTarget&& other) noexcept;
	explicit RenderTarget(typename ObjectTraits<DerivedType>::CreateDescType&& desc);

	[[maybe_unused]] RenderTarget& operator=(RenderTarget&& other) noexcept;

	// the attachments again, from the desc's images (after a transition changed an aspect)
	void InternalUpdateAttachments();

private:
	// what a pass does with an attachment (Vulkan's backend's defaults: don't care, both)
	struct AttachmentOps
	{
		LoadOp load = LoadOp::kDontCare;
		StoreOp store = StoreOp::kDontCare;
		LoadOp stencilLoad = LoadOp::kDontCare;
		StoreOp stencilStore = StoreOp::kDontCare;
	};

	void InternalReleaseAttachments() noexcept;

	std::vector<ImageViewHandle<kMetal>> myAttachments; // each image's texture (retained)
	std::vector<AttachmentDescription<kMetal>> myAttachmentDescs; // one per attachment (rhi's interface; empty in Metal)
	std::vector<AttachmentOps> myOps;
	std::optional<PipelineRenderingCreateInfo<kMetal>> myPipelineRenderingCreateInfo;
	RenderTargetBeginInfo<kMetal> myBeginInfo{}; // rhi's interface's (empty in Metal: the encoder is the command buffer's)
	std::unique_ptr<metal::RenderTargetClearObject> myClear; // ClearRect's pipelines (made at its first)
};

} // namespace rhi
