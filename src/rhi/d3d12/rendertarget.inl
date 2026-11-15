namespace rhi
{

// the D3D12 backend's render target: its images as attachments (each view's render target or depth stencil descriptor),
// and per attachment what a pass does with it (load and store: the desc's, the clear value), set on the command list at
// each Begin (OMSetRenderTargets, and the clears the loads ask for). its members are in rhi/d3d12/rendertarget.cpp (no
// D3D12 headers here), instantiated there for the backend's render targets (RenderImageSet; the swapchain implements
// IRenderTarget itself)
template <typename DerivedType>
class RenderTarget<DerivedType, kD3D12> : public IRenderTarget<kD3D12>, public DeviceObject<DerivedType>
{
public:
	~RenderTarget() override;

	[[nodiscard]] RenderTargetPassHandle<kD3D12> GetHandle() final { return {}; } // no render pass or framebuffer objects
	[[nodiscard]] Extent2d GetExtent() const final { return this->GetDesc().extent; }
	[[nodiscard]] std::span<const ImageHandle<kD3D12>> GetImages() const final { return this->GetDesc().images; }
	[[nodiscard]] ImageLayout GetLayout(uint32_t index) const override { return this->GetDesc().imageLayouts[index]; }
	[[nodiscard]] std::span<const ImageViewHandle<kD3D12>> GetAttachments() const final { return myAttachments; }
	[[nodiscard]] std::span<const AttachmentDescription<kD3D12>> GetAttachmentDescs() const final { return myAttachmentDescs; }
	[[nodiscard]] const std::optional<PipelineRenderingCreateInfo<kD3D12>>& GetPipelineRenderingCreateInfo() const final { return myPipelineRenderingCreateInfo; }

	[[maybe_unused]] const RenderTargetBeginInfo<kD3D12>& Begin(CommandBufferHandle<kD3D12> cmd, SubpassContents contents) final;
	void End(CommandBufferHandle<kD3D12> cmd) override;

	void ClearAll(CommandBufferHandle<kD3D12> cmd, std::span<const ClearValue> values) const final;
	void ClearRect(CommandBufferHandle<kD3D12> cmd, const ClearValue& value, uint32_t index, const Rect& rect) const final;

	void Blit(
		CommandBufferHandle<kD3D12> cmd,
		const IRenderTarget<kD3D12>& srcRenderTarget,
		const ImageSubresourceLayers<kD3D12>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<kD3D12>& dstSubresource,
		uint32_t dstIndex,
		Filter filter) final;

	void Copy(
		CommandBufferHandle<kD3D12> cmd,
		const IRenderTarget<kD3D12>& srcRenderTarget,
		const ImageSubresourceLayers<kD3D12>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<kD3D12>& dstSubresource,
		uint32_t dstIndex) final;

	void Clear(CommandBufferHandle<kD3D12> cmd, const ClearValue& value, uint32_t index) final;

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

	std::vector<ImageViewHandle<kD3D12>> myAttachments; // each image's view (the target's own)
	std::vector<AttachmentDescription<kD3D12>> myAttachmentDescs; // one per attachment (rhi's interface; empty in D3D12)
	std::vector<AttachmentOps> myOps;
	std::optional<PipelineRenderingCreateInfo<kD3D12>> myPipelineRenderingCreateInfo;
	RenderTargetBeginInfo<kD3D12> myBeginInfo{}; // rhi's interface's: the command buffer recording the pass
};

} // namespace rhi
