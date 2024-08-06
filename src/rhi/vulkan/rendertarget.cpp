#include "../rendertarget.h"

#include "utils.h"

#include <xxhash.h>

#include <format>
#include <memory>

template <>
void RenderTarget<kVk>::InternalInitializeAttachments(const RenderTargetCreateDesc<kVk>& desc)
{
	ZoneScopedN("RenderTarget::InternalInitializeAttachments");

	myAttachments.clear();

	uint32_t attachmentIt = 0UL;
	for (; attachmentIt < desc.colorImages.size(); attachmentIt++)
	{
		myAttachments.emplace_back(CreateImageView2D(
			*InternalGetDevice(),
			&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks(),
			0,
			desc.colorImages[attachmentIt],
			desc.colorImageFormats[attachmentIt],
			VK_IMAGE_ASPECT_COLOR_BIT,
			1));

	#if (GRAPHICS_VALIDATION_LEVEL > 0)
		InternalGetDevice()->AddOwnedObjectHandle(
			GetUid(),
			VK_OBJECT_TYPE_IMAGE_VIEW,
			reinterpret_cast<uint64_t>(myAttachments.back()),
			std::format("{0}_ColorImageView_{1}", GetName(), attachmentIt));
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
		if (HasStencilComponent(desc.depthStencilImageFormat))
			depthAspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;

		myAttachments.emplace_back(CreateImageView2D(
			*InternalGetDevice(),
			&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks(),
			0,
			desc.depthStencilImage,
			desc.depthStencilImageFormat,
			depthAspectFlags,
			1));

	#if (GRAPHICS_VALIDATION_LEVEL > 0)
		InternalGetDevice()->AddOwnedObjectHandle(
			GetUid(),
			VK_OBJECT_TYPE_IMAGE_VIEW,
			reinterpret_cast<uint64_t>(myAttachments.back()),
			std::format("{0}_DepthImageView", GetName()));
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
void RenderTarget<kVk>::InternalInitializeDefaultRenderPass(
	const RenderTargetCreateDesc<kVk>& desc)
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
RenderTarget<kVk>::InternalCalculateHashKey(const RenderTargetCreateDesc<kVk>& desc) const
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
RenderTargetHandle<kVk> RenderTarget<kVk>::InternalCreateRenderPassAndFrameBuffer(
	uint64_t hashKey, const RenderTargetCreateDesc<kVk>& desc)
{
	ZoneScopedN("RenderTarget::InternalCreateRenderPassAndFrameBuffer");

	auto* renderPass = CreateRenderPass(
		*InternalGetDevice(),
		&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks(),
		myAttachmentDescs,
		mySubPassDescs,
		mySubPassDependencies);

	auto* frameBuffer = CreateFramebuffer(
		*InternalGetDevice(),
		&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks(),
		renderPass,
		myAttachments.size(),
		myAttachments.data(),
		desc.extent.width,
		desc.extent.height,
		desc.layerCount);

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	InternalGetDevice()->AddOwnedObjectHandle(
		GetUid(),
		VK_OBJECT_TYPE_RENDER_PASS,
		reinterpret_cast<uint64_t>(renderPass),
		std::format("{0}_RenderPass_{1}", GetName(), hashKey));

	InternalGetDevice()->AddOwnedObjectHandle(
		GetUid(),
		VK_OBJECT_TYPE_FRAMEBUFFER,
		reinterpret_cast<uint64_t>(frameBuffer),
		std::format("{0}_FrameBuffer_{1}", GetName(), hashKey));
#endif

	return std::make_tuple(renderPass, frameBuffer);
}

template <>
const RenderTargetHandle<kVk>&
RenderTarget<kVk>::InternalUpdateMap(const RenderTargetCreateDesc<kVk>& desc)
{
	ZoneScopedN("RenderTarget::InternalUpdateMap");

	auto [keyValIt, insertResult] = myCache.emplace(
		InternalCalculateHashKey(desc),
		std::make_tuple(RenderPassHandle<kVk>{}, FramebufferHandle<kVk>{}));
	auto& [key, renderPassAndFramebuffer] = *keyValIt;

	if (insertResult)
		renderPassAndFramebuffer = InternalCreateRenderPassAndFrameBuffer(key, desc);

	return renderPassAndFramebuffer;
}

template <>
void RenderTarget<kVk>::InternalUpdateRenderPasses(const RenderTargetCreateDesc<kVk>& desc)
{
	ZoneScopedN("RenderTarget::InternalUpdateRenderPasses");
}

template <>
void RenderTarget<kVk>::InternalUpdateAttachments(const RenderTargetCreateDesc<kVk>& desc)
{
	ZoneScopedN("RenderTarget::InternalUpdateAttachments");

	uint32_t attachmentIt = 0UL;
	for (; attachmentIt < desc.colorImages.size(); attachmentIt++)
	{
		auto& colorAttachment = myAttachmentDescs[attachmentIt];

		if (auto layout = GetColorLayout(attachmentIt);
			layout != colorAttachment.initialLayout)
			colorAttachment.initialLayout = layout;
	}

	if (desc.depthStencilImage != nullptr)
	{
		auto& depthStencilAttachment = myAttachmentDescs[attachmentIt];

		if (auto layout = GetDepthStencilLayout();
			layout != depthStencilAttachment.initialLayout)
			depthStencilAttachment.initialLayout = layout;
	}
}

template <>
void RenderTarget<kVk>::Blit(
	CommandBufferHandle<kVk> cmd,
	const IRenderTarget<kVk>& srcRenderTarget,
	const ImageSubresourceLayers<kVk>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kVk>& dstSubresource,
	uint32_t dstIndex,
	Filter<kVk> filter)
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
		srcRenderTarget.GetColorLayout(srcIndex),
		GetRenderTargetDesc().colorImages[dstIndex],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&imageBlitRegion,
		filter);
}

template <>
void RenderTarget<kVk>::ClearSingleAttachment(
	CommandBufferHandle<kVk> cmd, const ClearAttachment<kVk>& clearAttachment) const
{
	ZoneScopedN("RenderTarget::ClearSingleAttachment");

	VkClearRect rect{
		{{0, 0}, GetRenderTargetDesc().extent}, 0, GetRenderTargetDesc().layerCount};
	vkCmdClearAttachments(cmd, 1, &clearAttachment, 1, &rect);
}

template <>
void RenderTarget<kVk>::ClearAllAttachments(
	CommandBufferHandle<kVk> cmd,
	const ClearColorValue<kVk>& color,
	const ClearDepthStencilValue<kVk>& depthStencil) const
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
void RenderTarget<kVk>::ClearColor(
	CommandBufferHandle<kVk> cmd, const ClearColorValue<kVk>& color, uint32_t index)
{
	ZoneScopedN("RenderTarget::ClearColor");

	TransitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, index);

	static const VkImageSubresourceRange kColorRange{
		VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};

	vkCmdClearColorImage(
		cmd,
		GetRenderTargetDesc().colorImages[index],
		GetColorLayout(index),
		&color,
		1,
		&kColorRange);
}

template <>
void RenderTarget<kVk>::ClearDepthStencil(
	CommandBufferHandle<kVk> cmd, const ClearDepthStencilValue<kVk>& depthStencil)
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
		GetDepthStencilLayout(),
		&depthStencil,
		1,
		&kDepthStencilRange);
}

template <>
void RenderTarget<kVk>::NextSubpass(CommandBufferHandle<kVk> cmd, SubpassContents<kVk> contents)
{
	ZoneScopedN("RenderTarget::NextSubpass");

	vkCmdNextSubpass(cmd, contents);
}

template <>
const RenderTargetHandle<kVk>& RenderTarget<kVk>::InternalGetValues()
{
	InternalUpdateAttachments(GetRenderTargetDesc());
	InternalUpdateRenderPasses(GetRenderTargetDesc());

	return InternalUpdateMap(GetRenderTargetDesc());
}

template <>
RenderInfo<kVk> RenderTarget<kVk>::Begin(CommandBufferHandle<kVk> cmd, SubpassContents<kVk> contents, std::span<const VkClearValue> clearValues)
{
	ZoneScopedN("RenderTarget::Begin");

	if (GetRenderTargetDesc().useDynamicRendering)
	{
		// todo: implement dynamic rendering fully. this is just placeholder code
		VkRenderingAttachmentInfoKHR colorAttachment
		{
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
			.pNext = nullptr,
			.imageView = GetColor(0),
			.imageLayout = GetColorLayout(0),
			.loadOp = GetColorLoadOp(0),
			.storeOp = GetColorStoreOp(0),
		};

		VkRenderingInfoKHR renderingInfo
		{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
			.pNext = nullptr,
			.flags = 0,
			.renderArea = {0, 0, GetRenderTargetDesc().extent.width, GetRenderTargetDesc().extent.height},
			.layerCount = 1,
			.viewMask = 0,
			.colorAttachmentCount = 1,
			.pColorAttachments = &colorAttachment,
		};

		static auto vkCmdBeginRenderingKHR = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
			vkGetInstanceProcAddr(
				*InternalGetDevice()->GetInstance(),
				"vkCmdBeginRenderingKHR"));
		ASSERT(vkCmdBeginRenderingKHR != nullptr);
		
		vkCmdBeginRenderingKHR(cmd, &renderingInfo);

		return renderingInfo;
	}
	else
	{
		CHECK(clearValues.size() <= myAttachments.size());

		const auto& [renderPass, frameBuffer] = InternalGetValues();

		auto info = VkRenderPassBeginInfo{
			VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			nullptr,
			renderPass,
			frameBuffer,
			{{0, 0}, {GetRenderTargetDesc().extent.width, GetRenderTargetDesc().extent.height}},
			static_cast<uint32_t>(clearValues.size()),
			clearValues.data()};

		{
			ZoneScopedN("RenderTarget::Begin::vkCmdBeginRenderPass");

			vkCmdBeginRenderPass(cmd, &info, contents);
		}

		return info;
	}

	return {};
}

template <>
void RenderTarget<kVk>::End(CommandBufferHandle<kVk> cmd)
{
	if (GetRenderTargetDesc().useDynamicRendering)
	{
		static auto vkCmdEndRenderingKHR = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
				vkGetInstanceProcAddr(
					*InternalGetDevice()->GetInstance(),
					"vkCmdEndRenderingKHR"));
		ASSERT(vkCmdEndRenderingKHR != nullptr);

		vkCmdEndRenderingKHR(cmd);
	}
	else
	{
		ZoneScopedN("RenderTarget::end");

		vkCmdEndRenderPass(cmd);
	}
}

template <>
RenderTarget<kVk>::RenderTarget(
	const std::shared_ptr<Device<kVk>>& device, const RenderTargetCreateDesc<kVk>& desc)
	: DeviceObject(device, {})
{
	ZoneScopedN("RenderTarget()");

	InternalInitializeAttachments(desc);

	if (!desc.useDynamicRendering)
	{
		InternalInitializeDefaultRenderPass(desc);
		InternalUpdateMap(desc);
	}
}

template <>
RenderTarget<kVk>::RenderTarget(RenderTarget&& other) noexcept
	: DeviceObject(std::forward<RenderTarget>(other))
	, myAttachments(std::exchange(other.myAttachments, {}))
	, myAttachmentDescs(std::exchange(other.myAttachmentDescs, {}))
	, myAttachmentsReferences(std::exchange(other.myAttachmentsReferences, {}))
	, mySubPassDescs(std::exchange(other.mySubPassDescs, {}))
	, mySubPassDependencies(std::exchange(other.mySubPassDependencies, {}))
	, myCache(std::exchange(other.myCache, {}))
{}

template <>
RenderTarget<kVk>::~RenderTarget()
{
	ZoneScopedN("~RenderTarget()");

	for (const auto& entry : myCache)
	{
		vkDestroyRenderPass(
			*InternalGetDevice(),
			std::get<0>(entry.second),
			&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
		vkDestroyFramebuffer(
			*InternalGetDevice(),
			std::get<1>(entry.second),
			&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
	}

	for (const auto& colorView : myAttachments)
		vkDestroyImageView(
			*InternalGetDevice(),
			colorView,
			&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
RenderTarget<kVk>& RenderTarget<kVk>::operator=(RenderTarget&& other) noexcept
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
void RenderTarget<kVk>::Swap(RenderTarget& rhs) noexcept
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
RenderTargetImpl<RenderTargetCreateDesc<kVk>, kVk>::RenderTargetImpl(
	const std::shared_ptr<Device<kVk>>& device, RenderTargetCreateDesc<kVk>&& desc)
	: RenderTarget(device, desc)
	, myDesc(std::forward<RenderTargetCreateDesc<kVk>>(desc))
{}

template <>
RenderTargetImpl<RenderTargetCreateDesc<kVk>, kVk>::RenderTargetImpl(
	RenderTargetImpl&& other) noexcept
	: RenderTarget(std::forward<RenderTargetImpl>(other))
	, myDesc(std::exchange(other.myDesc, {}))
{}

template <>
RenderTargetImpl<RenderTargetCreateDesc<kVk>, kVk>::~RenderTargetImpl() = default;

template <>
RenderTargetImpl<RenderTargetCreateDesc<kVk>, kVk>&
RenderTargetImpl<RenderTargetCreateDesc<kVk>, kVk>::operator=(RenderTargetImpl&& other) noexcept
{
	RenderTarget::operator=(std::forward<RenderTargetImpl>(other));
	myDesc = std::exchange(other.myDesc, {});
	return *this;
}

template <>
void RenderTargetImpl<RenderTargetCreateDesc<kVk>, kVk>::Swap(RenderTargetImpl& rhs) noexcept
{
	RenderTarget::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
}
