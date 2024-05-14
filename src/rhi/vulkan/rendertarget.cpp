#include "../rendertarget.h"

#include "utils.h"

#include <xxhash.h>

#include <format>
#include <memory>

template <>
void RenderTarget<Vk>::InternalInitializeAttachments(const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::InternalInitializeAttachments");

	myAttachments.clear();

	uint32_t attachmentIt = 0UL;
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

	#if (GRAPHICS_VALIDATION_LEVEL > 0)
		getDevice()->AddOwnedObjectHandle(
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
		colorAttachmentRef.attachment = 0UL;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	if (desc.depthStencilImage != nullptr)
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

	#if (GRAPHICS_VALIDATION_LEVEL > 0)
		getDevice()->AddOwnedObjectHandle(
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

	ASSERT(attachmentIt == myAttachmentsReferences.size());
}

template <>
void RenderTarget<Vk>::InternalInitializeDefaultRenderPass(
	const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::InternalInitializeDefaultRenderPass");

	uint32_t subPassIt = 0UL;

	if (desc.depthStencilImage != nullptr)
	{
		VkSubpassDescription colorAndDepth{};
		colorAndDepth.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		colorAndDepth.colorAttachmentCount = myAttachmentsReferences.size() - 1;
		colorAndDepth.pColorAttachments = myAttachmentsReferences.data();
		colorAndDepth.pDepthStencilAttachment = &myAttachmentsReferences.back();
		AddSubpassDescription(std::move(colorAndDepth));

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
		AddSubpassDependency(std::move(dep1));
	}
	else
	{
		VkSubpassDescription color{};
		color.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		color.colorAttachmentCount = myAttachmentsReferences.size();
		color.pColorAttachments = myAttachmentsReferences.data();
		color.pDepthStencilAttachment = nullptr;
		AddSubpassDescription(std::move(color));

		VkSubpassDependency dep0{};
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

template <>
uint64_t
RenderTarget<Vk>::InternalCalculateHashKey(const RenderTargetCreateDesc<Vk>& desc) const
{
	ZoneScopedN("RenderTarget::InternalCalculateHashKey");

	thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode (*)(XXH3_state_t*)> gthreadXxhState{
		XXH3_createState(), XXH3_freeState};

	auto result = XXH3_64bits_reset(gthreadXxhState.get());
	(void)result;
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gthreadXxhState.get(),
		myAttachments.data(),
		myAttachments.size() * sizeof(myAttachments.front()));
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gthreadXxhState.get(),
		myAttachmentDescs.data(),
		myAttachmentDescs.size() * sizeof(myAttachmentDescs.front()));
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gthreadXxhState.get(),
		myAttachmentsReferences.data(),
		myAttachmentsReferences.size() * sizeof(myAttachmentsReferences.front()));
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gthreadXxhState.get(),
		mySubPassDescs.data(),
		mySubPassDescs.size() * sizeof(mySubPassDescs.front()));
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gthreadXxhState.get(),
		mySubPassDependencies.data(),
		mySubPassDependencies.size() * sizeof(mySubPassDependencies.front()));
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(gthreadXxhState.get(), &desc.extent, sizeof(desc.extent));
	ASSERT(result != XXH_ERROR);

	return XXH3_64bits_digest(gthreadXxhState.get());
}

template <>
RenderTargetHandle<Vk> RenderTarget<Vk>::InternalCreateRenderPassAndFrameBuffer(
	uint64_t hashKey, const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::InternalCreateRenderPassAndFrameBuffer");

	auto* renderPass = createRenderPass(
		*getDevice(),
		&getDevice()->getInstance()->getHostAllocationCallbacks(),
		myAttachmentDescs,
		mySubPassDescs,
		mySubPassDependencies);

	auto* frameBuffer = createFramebuffer(
		*getDevice(),
		&getDevice()->getInstance()->getHostAllocationCallbacks(),
		renderPass,
		myAttachments.size(),
		myAttachments.data(),
		desc.extent.width,
		desc.extent.height,
		desc.layerCount);

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	getDevice()->AddOwnedObjectHandle(
		getUid(),
		VK_OBJECT_TYPE_RENDER_PASS,
		reinterpret_cast<uint64_t>(renderPass),
		std::format("{0}_RenderPass_{1}", getName(), hashKey));

	getDevice()->AddOwnedObjectHandle(
		getUid(),
		VK_OBJECT_TYPE_FRAMEBUFFER,
		reinterpret_cast<uint64_t>(frameBuffer),
		std::format("{0}_FrameBuffer_{1}", getName(), hashKey));
#endif

	return std::make_tuple(renderPass, frameBuffer);
}

template <>
const RenderTargetHandle<Vk>&
RenderTarget<Vk>::InternalUpdateMap(const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::InternalUpdateMap");

	auto [keyValIt, insertResult] = myCache.emplace(
		InternalCalculateHashKey(desc),
		std::make_tuple(RenderPassHandle<Vk>{}, FramebufferHandle<Vk>{}));
	auto& [key, renderPassAndFramebuffer] = *keyValIt;

	if (insertResult)
		renderPassAndFramebuffer = InternalCreateRenderPassAndFrameBuffer(key, desc);

	return renderPassAndFramebuffer;
}

template <>
void RenderTarget<Vk>::InternalUpdateRenderPasses(const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::InternalUpdateRenderPasses");
}

template <>
void RenderTarget<Vk>::InternalUpdateAttachments(const RenderTargetCreateDesc<Vk>& desc)
{
	ZoneScopedN("RenderTarget::InternalUpdateAttachments");

	uint32_t attachmentIt = 0UL;
	for (; attachmentIt < desc.colorImages.size(); attachmentIt++)
	{
		auto& colorAttachment = myAttachmentDescs[attachmentIt];

		if (auto layout = GetColorImageLayout(attachmentIt);
			layout != colorAttachment.initialLayout)
			colorAttachment.initialLayout = layout;
	}

	if (desc.depthStencilImage != nullptr)
	{
		auto& depthStencilAttachment = myAttachmentDescs[attachmentIt];

		if (auto layout = GetDepthStencilImageLayout();
			layout != depthStencilAttachment.initialLayout)
			depthStencilAttachment.initialLayout = layout;
	}
}

template <>
void RenderTarget<Vk>::Blit(
	CommandBufferHandle<Vk> cmd,
	const IRenderTarget<Vk>& srcRenderTarget,
	const ImageSubresourceLayers<Vk>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<Vk>& dstSubresource,
	uint32_t dstIndex,
	Filter<Vk> filter)
{
	ZoneScopedN("RenderTarget::blit");

	const auto& srcDesc = srcRenderTarget.GetRenderTargetDesc();

	VkOffset3D blitSize;
	blitSize.x = srcDesc.extent.width;
	blitSize.y = srcDesc.extent.height;
	blitSize.z = 1;

	VkImageBlit imageBlitRegion{};
	imageBlitRegion.srcSubresource = srcSubresource;
	imageBlitRegion.srcOffsets[1] = blitSize;
	imageBlitRegion.dstSubresource = dstSubresource;
	imageBlitRegion.dstOffsets[1] = blitSize;

	TransitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstIndex);

	vkCmdBlitImage(
		cmd,
		srcDesc.colorImages[srcIndex],
		srcRenderTarget.GetColorImageLayout(srcIndex),
		GetRenderTargetDesc().colorImages[dstIndex],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&imageBlitRegion,
		filter);
}

template <>
void RenderTarget<Vk>::ClearSingleAttachment(
	CommandBufferHandle<Vk> cmd, const ClearAttachment<Vk>& clearAttachment) const
{
	ZoneScopedN("RenderTarget::ClearSingleAttachment");

	VkClearRect rect{
		{{0, 0}, GetRenderTargetDesc().extent}, 0, GetRenderTargetDesc().layerCount};
	vkCmdClearAttachments(cmd, 1, &clearAttachment, 1, &rect);
}

template <>
void RenderTarget<Vk>::ClearAllAttachments(
	CommandBufferHandle<Vk> cmd,
	const ClearColorValue<Vk>& color,
	const ClearDepthStencilValue<Vk>& depthStencil) const
{
	ZoneScopedN("RenderTarget::ClearAllAttachments");

	uint32_t attachmentIt = 0UL;
	VkClearRect rect{
		{{0, 0}, GetRenderTargetDesc().extent}, 0, GetRenderTargetDesc().layerCount};
	std::vector<VkClearAttachment> clearAttachments(
		myAttachments.size(), {VK_IMAGE_ASPECT_COLOR_BIT, attachmentIt, {.color = color}});

	for (auto& attachment : clearAttachments)
		attachment.colorAttachment = attachmentIt++;

	if (GetRenderTargetDesc().depthStencilImage != nullptr)
		clearAttachments.emplace_back(VkClearAttachment{
			VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
			attachmentIt++,
			{.depthStencil = depthStencil}});

	vkCmdClearAttachments(cmd, clearAttachments.size(), clearAttachments.data(), 1, &rect);
}

template <>
void RenderTarget<Vk>::ClearColor(
	CommandBufferHandle<Vk> cmd, const ClearColorValue<Vk>& color, uint32_t index)
{
	ZoneScopedN("RenderTarget::ClearColor");

	TransitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, index);

	static const VkImageSubresourceRange kColorRange{
		VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};

	vkCmdClearColorImage(
		cmd,
		GetRenderTargetDesc().colorImages[index],
		GetColorImageLayout(index),
		&color,
		1,
		&kColorRange);
}

template <>
void RenderTarget<Vk>::ClearDepthStencil(
	CommandBufferHandle<Vk> cmd, const ClearDepthStencilValue<Vk>& depthStencil)
{
	ZoneScopedN("RenderTarget::ClearDepthStencil");

	TransitionDepthStencil(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	static const VkImageSubresourceRange kDepthStencilRange{
		VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		0,
		VK_REMAINING_MIP_LEVELS,
		0,
		VK_REMAINING_ARRAY_LAYERS};

	vkCmdClearDepthStencilImage(
		cmd,
		GetRenderTargetDesc().depthStencilImage,
		GetDepthStencilImageLayout(),
		&depthStencil,
		1,
		&kDepthStencilRange);
}

template <>
void RenderTarget<Vk>::NextSubpass(CommandBufferHandle<Vk> cmd, SubpassContents<Vk> contents)
{
	ZoneScopedN("RenderTarget::NextSubpass");

	vkCmdNextSubpass(cmd, contents);
}

template <>
const RenderTargetHandle<Vk>& RenderTarget<Vk>::InternalGetValues()
{
	InternalUpdateAttachments(GetRenderTargetDesc());
	InternalUpdateRenderPasses(GetRenderTargetDesc());

	return InternalUpdateMap(GetRenderTargetDesc());
}

template <>
RenderPassBeginInfo<Vk> RenderTarget<Vk>::Begin(CommandBufferHandle<Vk> cmd, SubpassContents<Vk> contents)
{
	ZoneScopedN("RenderTarget::begin");

	const auto& [renderPass, frameBuffer] = InternalGetValues();

	auto info = RenderPassBeginInfo<Vk>{
		VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		nullptr,
		renderPass,
		frameBuffer,
		{{0, 0}, {GetRenderTargetDesc().extent.width, GetRenderTargetDesc().extent.height}}};

	{
		ZoneScopedN("RenderTarget::begin::vkCmdBeginRenderPass");

		vkCmdBeginRenderPass(cmd, &info, contents);
	}

	return info;
}

template <>
void RenderTarget<Vk>::End(CommandBufferHandle<Vk> cmd)
{
	ZoneScopedN("RenderTarget::end");

	vkCmdEndRenderPass(cmd);
}

template <>
RenderTarget<Vk>::RenderTarget(
	const std::shared_ptr<Device<Vk>>& device, const RenderTargetCreateDesc<Vk>& desc)
	: DeviceObject(device, {})
{
	ZoneScopedN("RenderTarget()");

	InternalInitializeAttachments(desc);

	if (desc.useDefaultInitialization)
	{
		InternalInitializeDefaultRenderPass(desc);
		InternalUpdateMap(desc);
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
	return *this;
}

template <>
void RenderTarget<Vk>::Swap(RenderTarget& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myAttachments, rhs.myAttachments);
	std::swap(myAttachmentDescs, rhs.myAttachmentDescs);
	std::swap(myAttachmentsReferences, rhs.myAttachmentsReferences);
	std::swap(mySubPassDescs, rhs.mySubPassDescs);
	std::swap(mySubPassDependencies, rhs.mySubPassDependencies);
	std::swap(myCache, rhs.myCache);
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
RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::~RenderTargetImpl() = default;

template <>
RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>&
RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::operator=(RenderTargetImpl&& other) noexcept
{
	RenderTarget::operator=(std::forward<RenderTargetImpl>(other));
	myDesc = std::exchange(other.myDesc, {});
	return *this;
}

template <>
void RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::Swap(RenderTargetImpl& rhs) noexcept
{
	RenderTarget::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
}
