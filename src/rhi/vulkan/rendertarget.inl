#include <core/profiling.h>
#include <rhi/vulkan/utils.h>

#include <xxhash.h>

namespace rhi
{

template <typename DerivedType>
class RenderTarget<DerivedType, kVk> : public IRenderTarget<kVk>, public DeviceObject<DerivedType>
{
public:
	~RenderTarget() override;

	[[nodiscard]] RenderTargetPassHandle<kVk> GetHandle() final { return InternalGetValues(); }
	[[nodiscard]] Extent2d GetExtent() const final { return this->GetDesc().extent; }
	[[nodiscard]] std::span<const ImageHandle<kVk>> GetImages() const final { return this->GetDesc().images; }
	[[nodiscard]] ImageLayout GetLayout(uint32_t index) const override { return this->GetDesc().imageLayouts[index]; }
	[[nodiscard]] std::span<const ImageViewHandle<kVk>> GetAttachments() const final { return myAttachments; }
	[[nodiscard]] std::span<const AttachmentDescription<kVk>> GetAttachmentDescs() const final { return myAttachmentDescs; }
	[[nodiscard]] const std::optional<PipelineRenderingCreateInfo<kVk>>& GetPipelineRenderingCreateInfo() const final { return myPipelineRenderingCreateInfo; }

	// TODO(djohansson): make these two a single scoped call
	[[maybe_unused]] const RenderTargetBeginInfo<kVk>& Begin(CommandBufferHandle<kVk> cmd, SubpassContents contents) final;
	void End(CommandBufferHandle<kVk> cmd) override;
	//

	void ClearAll(
		CommandBufferHandle<kVk> cmd,
		std::span<const ClearValue> values) const final;

	void Blit(
		CommandBufferHandle<kVk> cmd,
		const IRenderTarget<kVk>& srcRenderTarget,
		const ImageSubresourceLayers<kVk>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<kVk>& dstSubresource,
		uint32_t dstIndex,
		Filter filter) final;

	void Copy(
		CommandBufferHandle<kVk> cmd,
		const IRenderTarget<kVk>& srcRenderTarget,
		const ImageSubresourceLayers<kVk>& srcSubresource,
		uint32_t srcIndex,
		const ImageSubresourceLayers<kVk>& dstSubresource,
		uint32_t dstIndex) final;

	void Clear(
		CommandBufferHandle<kVk> cmd,
		const ClearValue& value,
		uint32_t index) final;

	void SetLoadOp(LoadOp loadOp, uint32_t index, LoadOp stencilLoadOp = {}) final;
	void SetStoreOp(StoreOp storeOp, uint32_t index, StoreOp stencilStoreOp = {}) final;
	void SetClearValue(const ClearValue& value, uint32_t index) final;

	void AddSubpassDescription(SubpassDescription<kVk>&& description);
	void AddSubpassDependency(SubpassDependency<kVk>&& dependency);
	void NextSubpass(CommandBufferHandle<kVk> cmd, SubpassContents contents);
	void ResetSubpasses();

	void Swap(RenderTarget& rhs) noexcept;

	[[nodiscard]] operator auto() { return InternalGetValues(); };//NOLINT(google-explicit-constructor)

protected:
	constexpr RenderTarget() noexcept = default;
	RenderTarget(RenderTarget&& other) noexcept;

	explicit RenderTarget(typename ObjectTraits<DerivedType>::CreateDescType&& desc);

	[[maybe_unused]] RenderTarget& operator=(RenderTarget&& other) noexcept;

	void InternalUpdateAttachments();

private:
	[[nodiscard]] uint64_t InternalCalculateHashKey() const;
	[[nodiscard]] RenderTargetPassHandle<kVk> InternalCreateRenderPassAndFrameBuffer(uint64_t hashKey);

	void InternalInitializeAttachments();
	void InternalInitializeDefaultRenderPass();
	void InternalUpdateRenderPasses();

	[[maybe_unused]] const RenderTargetPassHandle<kVk>& InternalUpdateMap();
	[[nodiscard]] const RenderTargetPassHandle<kVk>& InternalGetValues();

	std::vector<ImageViewHandle<kVk>> myAttachments; // todo: store a pool of ImageView<kVk> instead of raw handles to avoid creating/destroying them every frame
	std::vector<AttachmentDescription<kVk>> myAttachmentDescs;
	std::vector<AttachmentReference<kVk>> myAttachmentsReferences;
	std::vector<SubpassDescription<kVk>> mySubPassDescs;
	std::vector<SubpassDependency<kVk>> mySubPassDependencies;

	std::optional<RenderTargetBeginInfo<kVk>> myRenderTargetBeginInfo;
	std::optional<PipelineRenderingCreateInfo<kVk>> myPipelineRenderingCreateInfo;
	std::vector<RenderingAttachmentInfo<kVk>> myColorAttachmentInfos;
	std::optional<RenderingAttachmentInfo<kVk>> myDepthAttachmentInfo;
	std::optional<RenderingAttachmentInfo<kVk>> myStencilAttachmentInfo;
	std::vector<VkFormat> myColorAttachmentFormats;
	std::optional<VkFormat> myDepthAttachmentFormat;
	std::optional<VkFormat> myStencilAttachmentFormat;
	std::vector<VkClearValue> myClearValues; // the desc's, for the render pass

	core::UnorderedMap<uint64_t, RenderTargetPassHandle<kVk>> myCache; // todo: consider making global
};

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::AddSubpassDescription(SubpassDescription<kVk>&& description)
{
	ASSERT(!myRenderTargetBeginInfo.has_value());
	
	mySubPassDescs.emplace_back(std::forward<SubpassDescription<kVk>>(description));
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::AddSubpassDependency(SubpassDependency<kVk>&& dependency)
{
	ASSERT(!myRenderTargetBeginInfo.has_value());

	mySubPassDependencies.emplace_back(std::forward<SubpassDependency<kVk>>(dependency));
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::ResetSubpasses()
{
	ASSERT(!myRenderTargetBeginInfo.has_value());

	mySubPassDescs.clear();
	mySubPassDependencies.clear();
}

template <typename DerivedType>
RenderTarget<DerivedType, kVk>::RenderTarget(typename ObjectTraits<DerivedType>::CreateDescType&& desc)
	: DeviceObject<DerivedType>(std::forward<typename ObjectTraits<DerivedType>::CreateDescType>(desc))
{
	ZoneScopedN("RenderTarget()");

	InternalInitializeAttachments();

	if (!this->GetDesc().useDynamicRendering)
	{
		InternalInitializeDefaultRenderPass();
		InternalUpdateMap();
	}
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::InternalInitializeAttachments()
{
	ZoneScopedN("RenderTarget::InternalInitializeAttachments");

	myAttachments.clear();

	uint32_t attachmentIt = 0UL;
	for (; attachmentIt < this->GetDesc().images.size(); attachmentIt++)
	{
		VkImageLayout finalLayout = HasColorComponent(this->GetDesc().imageFormats[attachmentIt]) ?
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		myAttachments.emplace_back(CreateImageView2D(
			this->GetDevice(),
			&this->GetInstance().GetHostAllocationCallbacks(),
			0,
			this->GetDesc().images[attachmentIt],
			vk::ToVk(this->GetDesc().imageFormats[attachmentIt]),
			vk::ToVk(this->GetDesc().imageAspectFlags[attachmentIt]),
			1,
			std::format("{} Attachment {} ImageView", this->GetName(), attachmentIt)));

		auto& attachment = myAttachmentDescs.emplace_back();
		attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
		attachment.format = vk::ToVk(this->GetDesc().imageFormats[attachmentIt]);
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = vk::ToVk(this->GetDesc().imageLayouts[attachmentIt]);
		attachment.finalLayout = finalLayout;

		auto& attachmentRef = myAttachmentsReferences.emplace_back();
		attachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
		attachmentRef.attachment = attachmentIt;
		attachmentRef.layout = finalLayout;

		auto aspectMask = vk::ToVk(this->GetDesc().imageAspectFlags[attachmentIt]);

		if (aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
			aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

		attachmentRef.aspectMask = aspectMask;
	}

	ENSURE(attachmentIt == myAttachmentsReferences.size());
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::InternalInitializeDefaultRenderPass()
{
	ZoneScopedN("RenderTarget::InternalInitializeDefaultRenderPass");

	bool hasDepth = false;
	bool hasStencil = false;
	for (const auto& format : this->GetDesc().imageFormats)
	{
		if (HasDepthComponent(format))
			hasDepth |= true;
		if (HasStencilComponent(format))
			hasStencil |= true;
	}

	uint32_t subPassIt = 0UL;

	if (hasDepth && hasStencil)
	{
		VkSubpassDescription2 colorAndDepth{.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
		colorAndDepth.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		colorAndDepth.colorAttachmentCount = myAttachmentsReferences.size() - 1;
		colorAndDepth.pColorAttachments = myAttachmentsReferences.data();
		colorAndDepth.pDepthStencilAttachment = &myAttachmentsReferences.back();
		AddSubpassDescription(std::move(colorAndDepth));

		VkSubpassDependency2 dep1{.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
		dep1.srcSubpass = VK_SUBPASS_EXTERNAL;
		dep1.dstSubpass = subPassIt;
		dep1.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
							VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dep1.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
							VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dep1.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		dep1.dstAccessMask =
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dep1.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
		AddSubpassDependency(std::move(dep1));
	}
	else
	{
		VkSubpassDescription2 color{.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
		color.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		color.colorAttachmentCount = myAttachmentsReferences.size();
		color.pColorAttachments = myAttachmentsReferences.data();
		color.pDepthStencilAttachment = nullptr;
		AddSubpassDescription(std::move(color));

		VkSubpassDependency2 dep0{.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
		dep0.srcSubpass = VK_SUBPASS_EXTERNAL;
		dep0.dstSubpass = subPassIt;
		dep0.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep0.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep0.srcAccessMask = 0UL;
		dep0.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dep0.dependencyFlags = 0UL;
		AddSubpassDependency(std::move(dep0));
	}
}

template <typename DerivedType>
uint64_t RenderTarget<DerivedType, kVk>::InternalCalculateHashKey() const
{
	ZoneScopedN("RenderTarget::InternalCalculateHashKey");

	thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode (*)(XXH3_state_t*)> gThreadXxhState{
		XXH3_createState(), XXH3_freeState};

	auto result = XXH3_64bits_reset(gThreadXxhState.get());
	ENSURE(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gThreadXxhState.get(),
		myAttachments.data(),
		myAttachments.size() * sizeof(myAttachments.front()));
	ENSURE(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gThreadXxhState.get(),
		myAttachmentDescs.data(),
		myAttachmentDescs.size() * sizeof(myAttachmentDescs.front()));
	ENSURE(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gThreadXxhState.get(),
		myAttachmentsReferences.data(),
		myAttachmentsReferences.size() * sizeof(myAttachmentsReferences.front()));
	ENSURE(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gThreadXxhState.get(),
		mySubPassDescs.data(),
		mySubPassDescs.size() * sizeof(mySubPassDescs.front()));
	ENSURE(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gThreadXxhState.get(),
		mySubPassDependencies.data(),
		mySubPassDependencies.size() * sizeof(mySubPassDependencies.front()));
	ENSURE(result != XXH_ERROR);

	result = XXH3_64bits_update(gThreadXxhState.get(), &this->GetDesc().extent, sizeof(this->GetDesc().extent));
	ENSURE(result != XXH_ERROR);

	return XXH3_64bits_digest(gThreadXxhState.get());
}

template <typename DerivedType>
RenderTargetPassHandle<kVk>
RenderTarget<DerivedType, kVk>::InternalCreateRenderPassAndFrameBuffer(uint64_t hashKey)
{
	ZoneScopedN("RenderTarget::InternalCreateRenderPassAndFrameBuffer");

	auto* renderPass = CreateRenderPass(
		this->GetDevice(),
		&this->GetInstance().GetHostAllocationCallbacks(),
		myAttachmentDescs,
		mySubPassDescs,
		mySubPassDependencies,
		std::format("{} RenderPass {}", this->GetName(), hashKey));

	auto* frameBuffer = CreateFramebuffer(
		this->GetDevice(),
		&this->GetInstance().GetHostAllocationCallbacks(),
		renderPass,
		myAttachments.size(),
		myAttachments.data(),
		this->GetDesc().extent.width,
		this->GetDesc().extent.height,
		this->GetDesc().layerCount,
		std::format("{} Framebuffer {}", this->GetName(), hashKey));

	return std::make_tuple(renderPass, frameBuffer);
}

template <typename DerivedType>
const RenderTargetPassHandle<kVk>&
RenderTarget<DerivedType, kVk>::InternalUpdateMap()
{
	ZoneScopedN("RenderTarget::InternalUpdateMap");

	auto [keyValIt, insertResult] = myCache.emplace(
		InternalCalculateHashKey(),
		std::make_tuple(RenderPassHandle<kVk>{}, FramebufferHandle<kVk>{}));
	auto& [key, renderPassAndFramebuffer] = *keyValIt;

	if (insertResult)
		renderPassAndFramebuffer = InternalCreateRenderPassAndFrameBuffer(key);

	return renderPassAndFramebuffer;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::InternalUpdateRenderPasses()
{
	ZoneScopedN("RenderTarget::InternalUpdateRenderPasses");
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::InternalUpdateAttachments()
{
	ZoneScopedN("RenderTarget::InternalUpdateAttachments");

	uint32_t attachmentIt = 0UL;
	for (; attachmentIt < this->GetDesc().images.size(); attachmentIt++)
	{
		auto& attachmentDesc = myAttachmentDescs[attachmentIt];
		auto& attachmentRef = myAttachmentsReferences[attachmentIt];

		if (auto layout = vk::ToVk(this->GetLayout(attachmentIt)); layout != attachmentDesc.initialLayout)
			attachmentDesc.initialLayout = layout;

		if (auto aspectMask = vk::ToVk(this->GetDesc().imageAspectFlags[attachmentIt]); aspectMask != attachmentRef.aspectMask)
		{
			if (aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
				aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

			// todo: investigate if we need to push this on the timeline
			// todo: store a pool of these to aviud recreating them every time
			attachmentRef.aspectMask = aspectMask;
			if (myAttachments[attachmentIt] != VK_NULL_HANDLE)
				DestroyImageView(
					this->GetDevice(),
					&this->GetInstance().GetHostAllocationCallbacks(),
					myAttachments[attachmentIt]);
			myAttachments[attachmentIt] = CreateImageView2D(
				this->GetDevice(),
				&this->GetInstance().GetHostAllocationCallbacks(),
				0,
				this->GetDesc().images[attachmentIt],
				vk::ToVk(this->GetDesc().imageFormats[attachmentIt]),
				aspectMask,
				1,
				std::format("{} Attachment {} ImageView", this->GetName(), attachmentIt));
		}
	}

	if (this->GetDesc().useDynamicRendering)
	{
		myColorAttachmentInfos.clear();
		myColorAttachmentInfos.reserve(myAttachments.size());
		myDepthAttachmentInfo.reset();
		myStencilAttachmentInfo.reset();
		myColorAttachmentFormats.clear();
		myColorAttachmentFormats.reserve(myAttachments.size());
		myDepthAttachmentFormat.reset();
		myStencilAttachmentFormat.reset();

		ENSURE(this->GetDesc().clearValues.size() == myAttachments.size());
			
		for (uint32_t i = 0; i < myAttachments.size(); i++)
		{
			const auto& attachment = myAttachments[i];
			const auto& attachmentDesc = myAttachmentDescs[i];
			const auto& clearValue = this->GetDesc().clearValues[i];

			if (HasColorComponent(attachmentDesc.format))
			{
				myColorAttachmentInfos.emplace_back(VkRenderingAttachmentInfoKHR
				{
					.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
					.pNext = nullptr,
					.imageView = attachment,
					.imageLayout = vk::ToVk(GetLayout(i)),
					.loadOp = attachmentDesc.loadOp,
					.storeOp = attachmentDesc.storeOp,
					.clearValue = vk::ToVk(clearValue, AspectOf(this->GetDesc().imageFormats[i])),
				});
				myColorAttachmentFormats.emplace_back(attachmentDesc.format);
			}
			else if (HasDepthComponent(attachmentDesc.format))
			{
				myDepthAttachmentInfo = VkRenderingAttachmentInfoKHR
				{
					.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
					.pNext = nullptr,
					.imageView = attachment,
					.imageLayout = vk::ToVk(GetLayout(i)),
					.loadOp = attachmentDesc.loadOp,
					.storeOp = attachmentDesc.storeOp,
					.clearValue = vk::ToVk(clearValue, AspectOf(this->GetDesc().imageFormats[i])),
				};
				myDepthAttachmentFormat = attachmentDesc.format;
			}
			else if (HasStencilComponent(attachmentDesc.format))
			{
				myStencilAttachmentInfo = VkRenderingAttachmentInfoKHR
				{
					.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
					.pNext = nullptr,
					.imageView = attachment,
					.imageLayout = vk::ToVk(GetLayout(i)),
					.loadOp = attachmentDesc.loadOp,
					.storeOp = attachmentDesc.storeOp,
					.clearValue = vk::ToVk(clearValue, AspectOf(this->GetDesc().imageFormats[i])),
				};
				myStencilAttachmentFormat = attachmentDesc.format;
			}
		}

		myPipelineRenderingCreateInfo = VkPipelineRenderingCreateInfoKHR{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
			.pNext = nullptr,
			.viewMask = 0,
			.colorAttachmentCount = static_cast<uint32_t>(myColorAttachmentInfos.size()),
			.pColorAttachmentFormats = myColorAttachmentFormats.data(),
			.depthAttachmentFormat = myDepthAttachmentFormat.value_or(VK_FORMAT_UNDEFINED),
			.stencilAttachmentFormat = myStencilAttachmentFormat.value_or(VK_FORMAT_UNDEFINED),
		};
	}

	myClearValues.clear();
	for (uint32_t i = 0; i < this->GetDesc().clearValues.size(); i++)
		myClearValues.push_back(vk::ToVk(
			this->GetDesc().clearValues[i],
			i < this->GetDesc().imageFormats.size() ? AspectOf(this->GetDesc().imageFormats[i]) : ImageAspect::kColor));
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::Blit(
	CommandBufferHandle<kVk> cmd,
	const IRenderTarget<kVk>& srcRenderTarget,
	const ImageSubresourceLayers<kVk>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kVk>& dstSubresource,
	uint32_t dstIndex,
	Filter filter)
{
	ZoneScopedN("RenderTarget::blit");

	const auto& srcExtent = srcRenderTarget.GetExtent();

	VkImageBlit imageBlit{};
	imageBlit.srcSubresource = srcSubresource;
	imageBlit.srcOffsets[1] = { .x = static_cast<int32_t>(srcExtent.width), .y = static_cast<int32_t>(srcExtent.height), .z = 1 };
	imageBlit.dstSubresource = dstSubresource;
	imageBlit.dstOffsets[1] = { .x = static_cast<int32_t>(this->GetDesc().extent.width), .y = static_cast<int32_t>(this->GetDesc().extent.height), .z = 1 };

	this->Transition(cmd, ImageLayout::kTransferDestination, AspectOf(this->GetDesc().imageFormats[srcIndex]), dstIndex);

	vkCmdBlitImage(
		cmd,
		srcRenderTarget.GetImages()[srcIndex],
		vk::ToVk(srcRenderTarget.GetLayout(srcIndex)),
		this->GetDesc().images[dstIndex],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&imageBlit,
		vk::ToVk(filter));
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::Copy(
	CommandBufferHandle<kVk> cmd,
	const IRenderTarget<kVk>& srcRenderTarget,
	const ImageSubresourceLayers<kVk>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kVk>& dstSubresource,
	uint32_t dstIndex)
{
	ZoneScopedN("RenderTarget::Copy");

	const auto srcExtent = srcRenderTarget.GetExtent();

	VkImageCopy imageCopy{};
	imageCopy.srcSubresource = srcSubresource;
	imageCopy.srcOffset = { .x = 0, .y = 0, .z = 0 };
	imageCopy.dstSubresource = dstSubresource;
	imageCopy.dstOffset = { .x = 0, .y = 0, .z = 0 };
	imageCopy.extent = { .width = srcExtent.width, .height = srcExtent.height, .depth = 1 };

	this->Transition(cmd, ImageLayout::kTransferDestination, AspectOf(this->GetDesc().imageFormats[srcIndex]), dstIndex);

	vkCmdCopyImage(
		cmd,
		srcRenderTarget.GetImages()[srcIndex],
		vk::ToVk(srcRenderTarget.GetLayout(srcIndex)),
		this->GetImages()[dstIndex],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&imageCopy);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::ClearAll(
	CommandBufferHandle<kVk> cmd,
	std::span<const ClearValue> values) const
{
	ZoneScopedN("RenderTarget::ClearAll");

	uint32_t attachmentIt = 0UL;
	VkClearRect rect{
		.rect = {.offset = {.x = 0, .y = 0}, .extent = vk::ToVk(this->GetDesc().extent)},
		.baseArrayLayer = 0,
		.layerCount = this->GetDesc().layerCount};
	
	std::vector<VkClearAttachment> clearAttachments(this->GetDesc().images.size());
	for (auto& attachment : clearAttachments)
	{
		auto aspect = AspectOf(this->GetDesc().imageFormats[attachmentIt]);
		attachment.aspectMask = vk::ToVk(aspect);
		attachment.colorAttachment = attachmentIt;
		attachment.clearValue = vk::ToVk(values[attachmentIt++], aspect);
	}

	vkCmdClearAttachments(cmd, clearAttachments.size(), clearAttachments.data(), 1, &rect);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::Clear(
	CommandBufferHandle<kVk> cmd,
	const ClearValue& value,
	uint32_t index)
{
	ZoneScopedN("RenderTarget::Clear");

	auto aspect = AspectOf(this->GetDesc().imageFormats[index]);

	this->Transition(cmd, ImageLayout::kTransferDestination, aspect, index);

	auto clearValue = vk::ToVk(value, aspect);
	VkImageSubresourceRange range{
		.aspectMask = vk::ToVk(aspect),
		.baseMipLevel = 0,
		.levelCount = VK_REMAINING_MIP_LEVELS,
		.baseArrayLayer = 0,
		.layerCount = VK_REMAINING_ARRAY_LAYERS
	};

	if (HasColorComponent(this->GetDesc().imageFormats[index]))
	{
		vkCmdClearColorImage(
			cmd,
			this->GetDesc().images[index],
			vk::ToVk(this->GetLayout(index)),
			&clearValue.color,
			1,
			&range);
	}
	else
	{
		vkCmdClearDepthStencilImage(
			cmd,
			this->GetDesc().images[index],
			vk::ToVk(this->GetLayout(index)),
			&clearValue.depthStencil,
			1,
			&range);
	}
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::SetLoadOp(
	LoadOp loadOp,
	uint32_t index,
	LoadOp stencilLoadOp)
{
	myAttachmentDescs[index].loadOp = vk::ToVk(loadOp);
	if (HasStencilComponent(this->GetDesc().imageFormats[index]))
		myAttachmentDescs[index].stencilLoadOp = vk::ToVk(stencilLoadOp);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::SetClearValue(const ClearValue& value, uint32_t index)
{
	ENSURE(index < this->GetDesc().clearValues.size());
	this->InternalGetDesc().clearValues[index] = value;
	InternalUpdateAttachments();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::SetStoreOp(
	StoreOp storeOp,
	uint32_t index,
	StoreOp stencilStoreOp)
{
	myAttachmentDescs[index].storeOp = vk::ToVk(storeOp);
	if (HasStencilComponent(this->GetDesc().imageFormats[index]))
		myAttachmentDescs[index].stencilStoreOp = vk::ToVk(stencilStoreOp);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::NextSubpass(CommandBufferHandle<kVk> cmd, SubpassContents contents)
{
	ZoneScopedN("RenderTarget::NextSubpass");

	vkCmdNextSubpass(cmd, vk::ToVk(contents));
}

template <typename DerivedType>
const RenderTargetPassHandle<kVk>& RenderTarget<DerivedType, kVk>::InternalGetValues()
{
	InternalUpdateAttachments();
	
	if (this->GetDesc().useDynamicRendering)
	{
		static const RenderTargetPassHandle<kVk> kEmptyRTHandle{};
		return kEmptyRTHandle;
	}

	InternalUpdateRenderPasses();

	return InternalUpdateMap();
}

template <typename DerivedType>
const RenderTargetBeginInfo<kVk>& RenderTarget<DerivedType, kVk>::Begin(
	CommandBufferHandle<kVk> cmd,
	SubpassContents contents)
{
	ZoneScopedN("RenderTarget::Begin");

	ENSURE(!myRenderTargetBeginInfo.has_value());

	const auto& desc = this->GetDesc();

	if (desc.useDynamicRendering)
	{
		myRenderTargetBeginInfo = DynamicRenderingInfo<kVk>
		{
			.renderInfo = RenderingInfo<kVk>
			{
				.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
				.pNext = nullptr,
				.flags = contents == SubpassContents::kSecondaryCommandBuffers ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0U,
				.renderArea = {.offset = {.x = 0, .y = 0}, .extent = {.width = desc.extent.width, .height = desc.extent.height}},
				.layerCount = 1,
				.viewMask = 0,
				.colorAttachmentCount = static_cast<uint32_t>(myColorAttachmentInfos.size()),
				.pColorAttachments = myColorAttachmentInfos.data(),
				.pDepthAttachment = myDepthAttachmentInfo ? &myDepthAttachmentInfo.value() : nullptr,
				.pStencilAttachment = myStencilAttachmentInfo ? &myStencilAttachmentInfo.value() : nullptr,
			},
			.inheritanceInfo = CommandBufferInheritanceRenderingInfo<kVk>
			{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO_KHR,
				.pNext = nullptr,
				.flags = contents == SubpassContents::kSecondaryCommandBuffers ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0U,
				.viewMask = 0,
				.colorAttachmentCount = static_cast<uint32_t>(myColorAttachmentFormats.size()),
				.pColorAttachmentFormats = myColorAttachmentFormats.data(),
				.depthAttachmentFormat = myDepthAttachmentFormat.value_or(VK_FORMAT_UNDEFINED),
				.stencilAttachmentFormat = myStencilAttachmentFormat.value_or(VK_FORMAT_UNDEFINED),
				.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
			}
		};

		gVkCmdBeginRenderingKHR(cmd, &std::get<DynamicRenderingInfo<kVk>>(myRenderTargetBeginInfo.value()).renderInfo);
	}
	else
	{
		ENSURE(desc.clearValues.size() == myAttachments.size());

		const auto& [renderPass, frameBuffer] = InternalGetValues();

		myRenderTargetBeginInfo = RenderPassBeginInfo<kVk>{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.pNext = nullptr,
			.renderPass = renderPass,
			.framebuffer = frameBuffer,
			.renderArea = {.offset = {.x = 0, .y = 0}, .extent = {.width = desc.extent.width, .height = desc.extent.height}},
			.clearValueCount = static_cast<uint32_t>(myClearValues.size()),
			.pClearValues = myClearValues.data()};

		vkCmdBeginRenderPass(cmd, &std::get<VkRenderPassBeginInfo>(myRenderTargetBeginInfo.value()), vk::ToVk(contents));
	}

	return myRenderTargetBeginInfo.value();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::End(CommandBufferHandle<kVk> cmd)
{
	ZoneScopedN("RenderTarget::End");

	ENSURE(myRenderTargetBeginInfo.has_value());

	if (this->GetDesc().useDynamicRendering)
	{
		gVkCmdEndRenderingKHR(cmd);
	}
	else
	{
		vkCmdEndRenderPass(cmd);
	}

	myRenderTargetBeginInfo = {};
}

template <typename DerivedType>
void RenderTarget<DerivedType, kVk>::Swap(RenderTarget& other) noexcept
{
	this->Swap(other);
	std::swap(myAttachmentDescs, other.myAttachmentDescs);
	std::swap(myAttachmentsReferences, other.myAttachmentsReferences);
	std::swap(mySubPassDescs, other.mySubPassDescs);
	std::swap(mySubPassDependencies, other.mySubPassDependencies);
	std::swap(myAttachments, other.myAttachments);
	std::swap(myCache, other.myCache);
}

template <typename DerivedType>
RenderTarget<DerivedType, kVk>::RenderTarget(RenderTarget&& other) noexcept
{
	Swap(other);
}

template <typename DerivedType>
RenderTarget<DerivedType, kVk>::~RenderTarget()
{
	ZoneScopedN("~RenderTarget()");

	ENSURE(!myRenderTargetBeginInfo.has_value());

	for (const auto& entry : myCache)
	{
		DestroyRenderPass(this->GetDevice(), &this->GetInstance().GetHostAllocationCallbacks(), std::get<0>(entry.second));
		DestroyFramebuffer(this->GetDevice(), &this->GetInstance().GetHostAllocationCallbacks(), std::get<1>(entry.second));
	}

	for (const auto& colorView : myAttachments)
		DestroyImageView(this->GetDevice(), &this->GetInstance().GetHostAllocationCallbacks(), colorView);
}

template <typename DerivedType>
RenderTarget<DerivedType, kVk>&
RenderTarget<DerivedType, kVk>::operator=(RenderTarget&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
