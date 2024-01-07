#include "../rendertarget.h"

#include "utils.h"

#include <xxhash.h>

#include <format>
#include <memory>

template <>
void RenderTarget<Vk>::internalInitializeAttachments(const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::internalInitializeAttachments");

	myAttachments.clear();

	uint32_t attachmentIt = 0ul;
	for (; attachmentIt < desc.colorImages.size(); attachmentIt++)
	{
		myAttachments.emplace_back(createImageView2D(
			*getDevice(),
			&getDevice()->getInstance()->getHostAllocationCallbacks(),
			0,
			desc.colorImages[attachmentIt],
			desc.colorImageFormats[attachmentIt],
			VK_IMAGE_ASPECT_COLOR_BIT,
			1));

	#if GRAPHICS_VALIDATION_ENABLED
		getDevice()->addOwnedObjectHandle(
			getUid(),
			VK_OBJECT_TYPE_IMAGE_VIEW,
			reinterpret_cast<uint64_t>(myAttachments.back()),
			std::format("{0}_ColorImageView_{1}", getName(), attachmentIt));
	#endif

		auto& colorAttachment = myAttachmentDescs.emplace_back();
		colorAttachment.format = desc.colorImageFormats[attachmentIt];
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = desc.colorImageLayouts[attachmentIt];
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		auto& colorAttachmentRef = myAttachmentsReferences.emplace_back();
		colorAttachmentRef.attachment = 0ul;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	if (desc.depthStencilImage)
	{
		VkImageAspectFlags depthAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (hasStencilComponent(desc.depthStencilImageFormat))
			depthAspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;

		myAttachments.emplace_back(createImageView2D(
			*getDevice(),
			&getDevice()->getInstance()->getHostAllocationCallbacks(),
			0,
			desc.depthStencilImage,
			desc.depthStencilImageFormat,
			depthAspectFlags,
			1));

	#if GRAPHICS_VALIDATION_ENABLED
		getDevice()->addOwnedObjectHandle(
			getUid(),
			VK_OBJECT_TYPE_IMAGE_VIEW,
			reinterpret_cast<uint64_t>(myAttachments.back()),
			std::format("{0}_DepthImageView", getName()));
	#endif

		auto& depthStencilAttachment = myAttachmentDescs.emplace_back();
		depthStencilAttachment.format = desc.depthStencilImageFormat;
		depthStencilAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthStencilAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthStencilAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depthStencilAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthStencilAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthStencilAttachment.initialLayout = desc.depthStencilImageLayout;
		depthStencilAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		auto& depthStencilAttachmentRef = myAttachmentsReferences.emplace_back();
		depthStencilAttachmentRef.attachment = attachmentIt;
		depthStencilAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		attachmentIt++;
	}

	assert(attachmentIt == myAttachmentsReferences.size());
}

template <>
void RenderTarget<Vk>::internalInitializeDefaultRenderPass(const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::internalInitializeDefaultRenderPass");

	uint32_t subPassIt = 0ul;

	if (desc.depthStencilImage)
	{
		VkSubpassDescription colorAndDepth{};
		colorAndDepth.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		colorAndDepth.colorAttachmentCount = myAttachmentsReferences.size() - 1;
		colorAndDepth.pColorAttachments = myAttachmentsReferences.data();
		colorAndDepth.pDepthStencilAttachment = &myAttachmentsReferences.back();
		addSubpassDescription(std::move(colorAndDepth));

		VkSubpassDependency dep1{};
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
		addSubpassDependency(std::move(dep1));
	}
	else
	{
		VkSubpassDescription color{};
		color.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		color.colorAttachmentCount = myAttachmentsReferences.size();
		color.pColorAttachments = myAttachmentsReferences.data();
		color.pDepthStencilAttachment = nullptr;
		addSubpassDescription(std::move(color));

		VkSubpassDependency dep0{};
		dep0.srcSubpass = VK_SUBPASS_EXTERNAL;
		dep0.dstSubpass = subPassIt;
		dep0.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep0.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep0.srcAccessMask = 0ul;
		dep0.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dep0.dependencyFlags = 0ul;
		addSubpassDependency(std::move(dep0));
	}
}

template <>
uint64_t RenderTarget<Vk>::internalCalculateHashKey(const RenderTargetCreateDesc<Vk>& desc) const
{
	ZoneScopedN("RenderTarget::internalCalculateHashKey");

	thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode (*)(XXH3_state_t*)> threadXXHState{
		XXH3_createState(), XXH3_freeState};

	auto result = XXH3_64bits_reset(threadXXHState.get());
	(void)result;
	assert(result != XXH_ERROR);

	result = XXH3_64bits_update(
		threadXXHState.get(),
		myAttachments.data(),
		myAttachments.size() * sizeof(myAttachments.front()));
	assert(result != XXH_ERROR);

	result = XXH3_64bits_update(
		threadXXHState.get(),
		myAttachmentDescs.data(),
		myAttachmentDescs.size() * sizeof(myAttachmentDescs.front()));
	assert(result != XXH_ERROR);

	result = XXH3_64bits_update(
		threadXXHState.get(),
		myAttachmentsReferences.data(),
		myAttachmentsReferences.size() * sizeof(myAttachmentsReferences.front()));
	assert(result != XXH_ERROR);

	result = XXH3_64bits_update(
		threadXXHState.get(),
		mySubPassDescs.data(),
		mySubPassDescs.size() * sizeof(mySubPassDescs.front()));
	assert(result != XXH_ERROR);

	result = XXH3_64bits_update(
		threadXXHState.get(),
		mySubPassDependencies.data(),
		mySubPassDependencies.size() * sizeof(mySubPassDependencies.front()));
	assert(result != XXH_ERROR);

	result = XXH3_64bits_update(threadXXHState.get(), &desc.extent, sizeof(desc.extent));
	assert(result != XXH_ERROR);

	return XXH3_64bits_digest(threadXXHState.get());
}

template <>
RenderTargetHandle<Vk> RenderTarget<Vk>::internalCreateRenderPassAndFrameBuffer(
	uint64_t hashKey, const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::internalCreateRenderPassAndFrameBuffer");

	auto renderPass = createRenderPass(
		*getDevice(),
		&getDevice()->getInstance()->getHostAllocationCallbacks(),
		myAttachmentDescs,
		mySubPassDescs,
		mySubPassDependencies);

	auto frameBuffer = createFramebuffer(
		*getDevice(),
		&getDevice()->getInstance()->getHostAllocationCallbacks(),
		renderPass,
		myAttachments.size(),
		myAttachments.data(),
		desc.extent.width,
		desc.extent.height,
		desc.layerCount);

#if GRAPHICS_VALIDATION_ENABLED
	getDevice()->addOwnedObjectHandle(
		getUid(),
		VK_OBJECT_TYPE_RENDER_PASS,
		reinterpret_cast<uint64_t>(renderPass),
		std::format("{0}_RenderPass_{1}", getName(), hashKey));

	getDevice()->addOwnedObjectHandle(
		getUid(),
		VK_OBJECT_TYPE_FRAMEBUFFER,
		reinterpret_cast<uint64_t>(frameBuffer),
		std::format("{0}_FrameBuffer_{1}", getName(), hashKey));
#endif

	return std::make_tuple(renderPass, frameBuffer);
}

template <>
const std::tuple<RenderPassHandle<Vk>, FramebufferHandle<Vk>>&
RenderTarget<Vk>::internalUpdateMap(const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::internalUpdateMap");

	auto [keyValIt, insertResult] = myCache.emplace(
		internalCalculateHashKey(desc),
		std::make_tuple(RenderPassHandle<Vk>{}, FramebufferHandle<Vk>{}));
	auto& [key, renderPassAndFramebuffer] = *keyValIt;

	if (insertResult)
		renderPassAndFramebuffer = internalCreateRenderPassAndFrameBuffer(key, desc);

	return renderPassAndFramebuffer;
}

template <>
void RenderTarget<Vk>::internalUpdateRenderPasses(const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::internalUpdateRenderPasses");
}

template <>
void RenderTarget<Vk>::internalUpdateAttachments(const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::internalUpdateAttachments");

	uint32_t attachmentIt = 0ul;
	for (; attachmentIt < desc.colorImages.size(); attachmentIt++)
	{
		auto& colorAttachment = myAttachmentDescs[attachmentIt];

		if (auto layout = getColorImageLayout(attachmentIt);
			layout != colorAttachment.initialLayout)
			colorAttachment.initialLayout = layout;
	}

	if (desc.depthStencilImage)
	{
		auto& depthStencilAttachment = myAttachmentDescs[attachmentIt];

		if (auto layout = getDepthStencilImageLayout();
			layout != depthStencilAttachment.initialLayout)
			depthStencilAttachment.initialLayout = layout;
	}
}

template <>
void RenderTarget<Vk>::blit(
	CommandBufferHandle<Vk> cmd,
	const IRenderTarget<Vk>& srcRenderTarget,
	const ImageSubresourceLayers<Vk>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<Vk>& dstSubresource,
	uint32_t dstIndex,
	Filter<Vk> filter)
{
	ZoneScopedN("RenderTarget::blit");

	const auto& srcDesc = srcRenderTarget.getRenderTargetDesc();

	VkOffset3D blitSize;
	blitSize.x = srcDesc.extent.width;
	blitSize.y = srcDesc.extent.height;
	blitSize.z = 1;

	VkImageBlit imageBlitRegion{};
	imageBlitRegion.srcSubresource = srcSubresource;
	imageBlitRegion.srcOffsets[1] = blitSize;
	imageBlitRegion.dstSubresource = dstSubresource;
	imageBlitRegion.dstOffsets[1] = blitSize;

	transitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstIndex);

	vkCmdBlitImage(
		cmd,
		srcDesc.colorImages[srcIndex],
		srcRenderTarget.getColorImageLayout(srcIndex),
		getRenderTargetDesc().colorImages[dstIndex],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&imageBlitRegion,
		filter);
}

template <>
void RenderTarget<Vk>::clearSingleAttachment(
	CommandBufferHandle<Vk> cmd, const ClearAttachment<Vk>& clearAttachment) const
{
	ZoneScopedN("RenderTarget::clearSingleAttachment");

	VkClearRect rect{
		{{0, 0}, getRenderTargetDesc().extent}, 0, getRenderTargetDesc().layerCount};
	vkCmdClearAttachments(cmd, 1, &clearAttachment, 1, &rect);
}

template <>
void RenderTarget<Vk>::clearAllAttachments(
	CommandBufferHandle<Vk> cmd,
	const ClearColorValue<Vk>& color,
	const ClearDepthStencilValue<Vk>& depthStencil) const
{
	ZoneScopedN("RenderTarget::clearAllAttachments");

	uint32_t attachmentIt = 0ul;
	VkClearRect rect{
		{{0, 0}, getRenderTargetDesc().extent}, 0, getRenderTargetDesc().layerCount};
	std::vector<VkClearAttachment> clearAttachments(
		myAttachments.size(), {VK_IMAGE_ASPECT_COLOR_BIT, attachmentIt, {.color = color}});

	for (auto& attachment : clearAttachments)
		attachment.colorAttachment = attachmentIt++;

	if (getRenderTargetDesc().depthStencilImage)
		clearAttachments.emplace_back(VkClearAttachment{
			VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
			attachmentIt++,
			{.depthStencil = depthStencil}});

	vkCmdClearAttachments(cmd, clearAttachments.size(), clearAttachments.data(), 1, &rect);
}

template <>
void RenderTarget<Vk>::clearColor(
	CommandBufferHandle<Vk> cmd, const ClearColorValue<Vk>& color, uint32_t index)
{
	ZoneScopedN("RenderTarget::clearColor");

	transitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, index);

	static const VkImageSubresourceRange colorRange{
		VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};

	vkCmdClearColorImage(
		cmd,
		getRenderTargetDesc().colorImages[index],
		getColorImageLayout(index),
		&color,
		1,
		&colorRange);
}

template <>
void RenderTarget<Vk>::clearDepthStencil(
	CommandBufferHandle<Vk> cmd, const ClearDepthStencilValue<Vk>& depthStencil)
{
	ZoneScopedN("RenderTarget::clearDepthStencil");

	transitionDepthStencil(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	static const VkImageSubresourceRange depthStencilRange{
		VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		0,
		VK_REMAINING_MIP_LEVELS,
		0,
		VK_REMAINING_ARRAY_LAYERS};

	vkCmdClearDepthStencilImage(
		cmd,
		getRenderTargetDesc().depthStencilImage,
		getDepthStencilImageLayout(),
		&depthStencil,
		1,
		&depthStencilRange);
}

template <>
void RenderTarget<Vk>::nextSubpass(CommandBufferHandle<Vk> cmd, SubpassContents<Vk> contents)
{
	ZoneScopedN("RenderTarget::nextSubpass");

	vkCmdNextSubpass(cmd, contents);

	(*myCurrentSubpass)++;
}

template <>
const std::tuple<RenderPassHandle<Vk>, FramebufferHandle<Vk>>& RenderTarget<Vk>::internalGetValues()
{
	internalUpdateAttachments(getRenderTargetDesc());
	internalUpdateRenderPasses(getRenderTargetDesc());

	return internalUpdateMap(getRenderTargetDesc());
}

template <>
const std::optional<RenderPassBeginInfo<Vk>>&
RenderTarget<Vk>::begin(CommandBufferHandle<Vk> cmd, SubpassContents<Vk> contents)
{
	ZoneScopedN("RenderTarget::begin");

	assert(myCurrentPass == std::nullopt);

	const auto& [renderPass, frameBuffer] = internalGetValues();

	myCurrentPass = std::make_optional(RenderPassBeginInfo<Vk>{
		VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		nullptr,
		renderPass,
		frameBuffer,
		{{0, 0}, {getRenderTargetDesc().extent.width, getRenderTargetDesc().extent.height}}});

	{
		ZoneScopedN("RenderTarget::begin::vkCmdBeginRenderPass");

		vkCmdBeginRenderPass(cmd, &myCurrentPass.value(), contents);
	}

	return myCurrentPass;
}

template <>
void RenderTarget<Vk>::end(CommandBufferHandle<Vk> cmd)
{
	ZoneScopedN("RenderTarget::end");

	assert(myCurrentPass != std::nullopt);

	vkCmdEndRenderPass(cmd);

	myCurrentPass = std::nullopt;
}

template <>
RenderTarget<Vk>::RenderTarget(
	const std::shared_ptr<Device<Vk>>& device, const RenderTargetCreateDesc<Vk>& desc)
	: DeviceObject(device, {})
{
	ZoneScopedN("RenderTarget()");

	internalInitializeAttachments(desc);

	if (desc.useDefaultInitialization)
	{
		internalInitializeDefaultRenderPass(desc);
		internalUpdateMap(desc);
	}
}

template <>
RenderTarget<Vk>::RenderTarget(RenderTarget&& other) noexcept
	: DeviceObject(std::forward<RenderTarget>(other))
	, myAttachments(std::exchange(other.myAttachments, {}))
	, myAttachmentDescs(std::exchange(other.myAttachmentDescs, {}))
	, myAttachmentsReferences(std::exchange(other.myAttachmentsReferences, {}))
	, mySubPassDescs(std::exchange(other.mySubPassDescs, {}))
	, mySubPassDependencies(std::exchange(other.mySubPassDependencies, {}))
	, myCache(std::exchange(other.myCache, {}))
	, myCurrentPass(std::exchange(other.myCurrentPass, {}))
	, myCurrentSubpass(std::exchange(other.myCurrentSubpass, {}))
{}

template <>
RenderTarget<Vk>::~RenderTarget()
{
	ZoneScopedN("~RenderTarget()");

	for (const auto& entry : myCache)
	{
		vkDestroyRenderPass(
			*getDevice(),
			std::get<0>(entry.second),
			&getDevice()->getInstance()->getHostAllocationCallbacks());
		vkDestroyFramebuffer(
			*getDevice(),
			std::get<1>(entry.second),
			&getDevice()->getInstance()->getHostAllocationCallbacks());
	}

	for (const auto& colorView : myAttachments)
		vkDestroyImageView(
			*getDevice(),
			colorView,
			&getDevice()->getInstance()->getHostAllocationCallbacks());
}

template <>
RenderTarget<Vk>& RenderTarget<Vk>::operator=(RenderTarget&& other) noexcept
{
	DeviceObject::operator=(std::forward<RenderTarget>(other));
	myAttachments = std::exchange(other.myAttachments, {});
	myAttachmentDescs = std::exchange(other.myAttachmentDescs, {});
	myAttachmentsReferences = std::exchange(other.myAttachmentsReferences, {});
	mySubPassDescs = std::exchange(other.mySubPassDescs, {});
	mySubPassDependencies = std::exchange(other.mySubPassDependencies, {});
	myCache = std::exchange(other.myCache, {});
	myCurrentPass = std::exchange(other.myCurrentPass, {});
	myCurrentSubpass = std::exchange(other.myCurrentSubpass, {});
	return *this;
}

template <>
void RenderTarget<Vk>::swap(RenderTarget& rhs) noexcept
{
	DeviceObject::swap(rhs);
	std::swap(myAttachments, rhs.myAttachments);
	std::swap(myAttachmentDescs, rhs.myAttachmentDescs);
	std::swap(myAttachmentsReferences, rhs.myAttachmentsReferences);
	std::swap(mySubPassDescs, rhs.mySubPassDescs);
	std::swap(mySubPassDependencies, rhs.mySubPassDependencies);
	std::swap(myCache, rhs.myCache);
	std::swap(myCurrentPass, rhs.myCurrentPass);
	std::swap(myCurrentSubpass, rhs.myCurrentSubpass);
}

template <>
RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::RenderTargetImpl(
	const std::shared_ptr<Device<Vk>>& device, RenderTargetCreateDesc<Vk>&& desc)
	: RenderTarget(device, desc)
	, myDesc(std::forward<RenderTargetCreateDesc<Vk>>(desc))
{}

template <>
RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::RenderTargetImpl(
	RenderTargetImpl&& other) noexcept
	: RenderTarget(std::forward<RenderTargetImpl>(other))
	, myDesc(std::exchange(other.myDesc, {}))
{}

template <>
RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::~RenderTargetImpl()
{}

template <>
RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>&
RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::operator=(RenderTargetImpl&& other) noexcept
{
	RenderTarget::operator=(std::forward<RenderTargetImpl>(other));
	myDesc = std::exchange(other.myDesc, {});
	return *this;
}

template <>
void RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::swap(RenderTargetImpl& rhs) noexcept
{
	RenderTarget::swap(rhs);
	std::swap(myDesc, rhs.myDesc);
}
