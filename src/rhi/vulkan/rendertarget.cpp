#include "../rendertarget.h"

#include "utils.h"

#include <cstdint>
#include <format>
#include <memory>

#include <xxhash.h>

template <>
void RenderTarget<kVk>::InternalInitializeAttachments(const RenderTargetCreateDesc<kVk>& desc)
{
	ZoneScopedN("RenderTarget::InternalInitializeAttachments");

	myAttachments.clear();

	uint32_t attachmentIt = 0UL;
	for (; attachmentIt < desc.images.size(); attachmentIt++)
	{
		VkImageLayout finalLayout = HasColorComponent(desc.imageFormats[attachmentIt]) ?
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		myAttachments.emplace_back(CreateImageView2D(
			*InternalGetDevice(),
			&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks(),
			0,
			desc.images[attachmentIt],
			desc.imageFormats[attachmentIt],
			desc.imageAspectFlags[attachmentIt],
			1));

	#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		InternalGetDevice()->AddOwnedObjectHandle(
			GetUuid(),
			VK_OBJECT_TYPE_IMAGE_VIEW,
			reinterpret_cast<uint64_t>(myAttachments.back()),
			std::format("{}_ColorImageView_{}", GetName(), attachmentIt));
	#endif

		auto& attachment = myAttachmentDescs.emplace_back();
		attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
		attachment.format = desc.imageFormats[attachmentIt];
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = desc.imageLayouts[attachmentIt];
		attachment.finalLayout = finalLayout;

		auto& attachmentRef = myAttachmentsReferences.emplace_back();
		attachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
		attachmentRef.attachment = attachmentIt;
		attachmentRef.layout = finalLayout;

		auto aspectMask = desc.imageAspectFlags[attachmentIt];

		if (aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
			aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

		attachmentRef.aspectMask = aspectMask;
	}

	ASSERT(attachmentIt == myAttachmentsReferences.size());
}

template <>
void RenderTarget<kVk>::InternalInitializeDefaultRenderPass(
	const RenderTargetCreateDesc<kVk>& desc)
{
	ZoneScopedN("RenderTarget::InternalInitializeDefaultRenderPass");

	bool hasDepth = false;
	bool hasStencil = false;
	for (const auto& format : desc.imageFormats)
	{
		if (HasDepthComponent(format))
			hasDepth |= true;
		if (HasStencilComponent(format))
			hasStencil |= true;
	}

	uint32_t subPassIt = 0UL;

	if (hasDepth && hasStencil)
	{
		VkSubpassDescription2 colorAndDepth{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
		colorAndDepth.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		colorAndDepth.colorAttachmentCount = myAttachmentsReferences.size() - 1;
		colorAndDepth.pColorAttachments = myAttachmentsReferences.data();
		colorAndDepth.pDepthStencilAttachment = &myAttachmentsReferences.back();
		AddSubpassDescription(std::move(colorAndDepth));

		VkSubpassDependency2 dep1{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
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
		VkSubpassDescription2 color{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
		color.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		color.colorAttachmentCount = myAttachmentsReferences.size();
		color.pColorAttachments = myAttachmentsReferences.data();
		color.pDepthStencilAttachment = nullptr;
		AddSubpassDescription(std::move(color));

		VkSubpassDependency2 dep0{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
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

	thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode (*)(XXH3_state_t*)> gThreadXxhState{
		XXH3_createState(), XXH3_freeState};

	auto result = XXH3_64bits_reset(gThreadXxhState.get());
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gThreadXxhState.get(),
		myAttachments.data(),
		myAttachments.size() * sizeof(myAttachments.front()));
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gThreadXxhState.get(),
		myAttachmentDescs.data(),
		myAttachmentDescs.size() * sizeof(myAttachmentDescs.front()));
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gThreadXxhState.get(),
		myAttachmentsReferences.data(),
		myAttachmentsReferences.size() * sizeof(myAttachmentsReferences.front()));
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gThreadXxhState.get(),
		mySubPassDescs.data(),
		mySubPassDescs.size() * sizeof(mySubPassDescs.front()));
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(
		gThreadXxhState.get(),
		mySubPassDependencies.data(),
		mySubPassDependencies.size() * sizeof(mySubPassDependencies.front()));
	ASSERT(result != XXH_ERROR);

	result = XXH3_64bits_update(gThreadXxhState.get(), &desc.extent, sizeof(desc.extent));
	ASSERT(result != XXH_ERROR);

	return XXH3_64bits_digest(gThreadXxhState.get());
}

template <>
RenderTargetPassHandle<kVk> RenderTarget<kVk>::InternalCreateRenderPassAndFrameBuffer(
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

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	InternalGetDevice()->AddOwnedObjectHandle(
		GetUuid(),
		VK_OBJECT_TYPE_RENDER_PASS,
		reinterpret_cast<uint64_t>(renderPass),
		std::format("{}_RenderPass_{}", GetName(), hashKey));

	InternalGetDevice()->AddOwnedObjectHandle(
		GetUuid(),
		VK_OBJECT_TYPE_FRAMEBUFFER,
		reinterpret_cast<uint64_t>(frameBuffer),
		std::format("{}_FrameBuffer_{}", GetName(), hashKey));
#endif

	return std::make_tuple(renderPass, frameBuffer);
}

template <>
const RenderTargetPassHandle<kVk>&
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
	for (; attachmentIt < desc.images.size(); attachmentIt++)
	{
		auto& attachmentDesc = myAttachmentDescs[attachmentIt];
		auto& attachmentRef = myAttachmentsReferences[attachmentIt];

		if (auto layout = GetLayout(attachmentIt); layout != attachmentDesc.initialLayout)
			attachmentDesc.initialLayout = layout;

		if (auto aspectMask = desc.imageAspectFlags[attachmentIt]; aspectMask != attachmentRef.aspectMask)
		{
			if (aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
				aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

			// todo: investigate if we need to push this on the timeline
			attachmentRef.aspectMask = aspectMask;
			myAttachments[attachmentIt] = CreateImageView2D(
				*InternalGetDevice(),
				&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks(),
				0,
				desc.images[attachmentIt],
				desc.imageFormats[attachmentIt],
				aspectMask,
				1);
		}
	}

	if (desc.useDynamicRendering)
	{
		myColorAttachmentInfos.clear();
		myColorAttachmentInfos.reserve(myAttachments.size());
		myDepthAttachmentInfo.reset();
		myStencilAttachmentInfo.reset();
		myColorAttachmentFormats.clear();
		myColorAttachmentFormats.reserve(myAttachments.size());
		myDepthAttachmentFormat.reset();
		myStencilAttachmentFormat.reset();

		ENSURE(desc.clearValues.size() == myAttachments.size());
			
		for (uint32_t i = 0; i < myAttachments.size(); i++)
		{
			const auto& attachment = myAttachments[i];
			const auto& attachmentDesc = myAttachmentDescs[i];
			const auto& clearValue = desc.clearValues[i];

			if (HasColorComponent(attachmentDesc.format))
			{
				myColorAttachmentInfos.emplace_back(VkRenderingAttachmentInfoKHR
				{
					.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
					.pNext = nullptr,
					.imageView = attachment,
					.imageLayout = GetLayout(i),
					.loadOp = attachmentDesc.loadOp,
					.storeOp = attachmentDesc.storeOp,
					.clearValue = clearValue,
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
					.imageLayout = GetLayout(i),
					.loadOp = attachmentDesc.loadOp,
					.storeOp = attachmentDesc.storeOp,
					.clearValue = clearValue,
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
					.imageLayout = GetLayout(i),
					.loadOp = attachmentDesc.loadOp,
					.storeOp = attachmentDesc.storeOp,
					.clearValue = clearValue,
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
			.depthAttachmentFormat = myDepthAttachmentFormat.value_or(Format<kVk>{}),
			.stencilAttachmentFormat = myStencilAttachmentFormat.value_or(Format<kVk>{}),
		};
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

	VkImageBlit imageBlit{};
	imageBlit.srcSubresource = srcSubresource;
	imageBlit.srcOffsets[1] = { .x = static_cast<int32_t>(srcDesc.extent.width), .y = static_cast<int32_t>(srcDesc.extent.height), .z = 1 };
	imageBlit.dstSubresource = dstSubresource;
	imageBlit.dstOffsets[1] = { .x = static_cast<int32_t>(GetRenderTargetDesc().extent.width), .y = static_cast<int32_t>(GetRenderTargetDesc().extent.height), .z = 1 };

	VkImageAspectFlags aspectFlags{};
	if (HasColorComponent(GetRenderTargetDesc().imageFormats[srcIndex]))
	{
		aspectFlags = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	else 
	{
		if (HasDepthComponent(GetRenderTargetDesc().imageFormats[srcIndex]))
			aspectFlags |= VK_IMAGE_ASPECT_DEPTH_BIT;
		if (HasStencilComponent(GetRenderTargetDesc().imageFormats[srcIndex]))
			aspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}

	Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspectFlags, dstIndex);

	vkCmdBlitImage(
		cmd,
		srcDesc.images[srcIndex],
		srcRenderTarget.GetLayout(srcIndex),
		GetRenderTargetDesc().images[dstIndex],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&imageBlit,
		filter);
}

template <>
void RenderTarget<kVk>::Copy(
	CommandBufferHandle<kVk> cmd,
	const IRenderTarget<kVk>& srcRenderTarget,
	const ImageSubresourceLayers<kVk>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kVk>& dstSubresource,
	uint32_t dstIndex)
{
	ZoneScopedN("RenderTarget::Copy");

	const auto& srcDesc = srcRenderTarget.GetRenderTargetDesc();

	VkImageCopy imageCopy{};
	imageCopy.srcSubresource = srcSubresource;
	imageCopy.srcOffset = { .x = 0, .y = 0, .z = 0 };
	imageCopy.dstSubresource = dstSubresource;
	imageCopy.dstOffset = { .x = 0, .y = 0, .z = 0 };
	imageCopy.extent = { .width = srcDesc.extent.width, .height = srcDesc.extent.height, .depth = 1 };

	VkImageAspectFlags aspectFlags{};
	if (HasColorComponent(GetRenderTargetDesc().imageFormats[srcIndex]))
	{
		aspectFlags = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	else 
	{
		if (HasDepthComponent(GetRenderTargetDesc().imageFormats[srcIndex]))
			aspectFlags |= VK_IMAGE_ASPECT_DEPTH_BIT;
		if (HasStencilComponent(GetRenderTargetDesc().imageFormats[srcIndex]))
			aspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}

	Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspectFlags, dstIndex);

	vkCmdCopyImage(
		cmd,
		srcDesc.images[srcIndex],
		srcRenderTarget.GetLayout(srcIndex),
		GetRenderTargetDesc().images[dstIndex],
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1,
		&imageCopy);
}

template <>
void RenderTarget<kVk>::ClearAll(
	CommandBufferHandle<kVk> cmd,
	std::span<const ClearValue<kVk>> values) const
{
	ZoneScopedN("RenderTarget::ClearAll");

	uint32_t attachmentIt = 0UL;
	VkClearRect rect{
		{{0, 0}, GetRenderTargetDesc().extent}, 0, GetRenderTargetDesc().layerCount};
	
	std::vector<VkClearAttachment> clearAttachments(GetRenderTargetDesc().images.size());
	for (auto& attachment : clearAttachments)
	{
		auto format = GetRenderTargetDesc().imageFormats[attachmentIt];
		if (HasColorComponent(format))
		{
			attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		}
		else 
		{
			if (HasDepthComponent(format))
				attachment.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
			if (HasStencilComponent(format))
				attachment.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}

		attachment.colorAttachment = attachmentIt;
		attachment.clearValue = values[attachmentIt++];
	}

	vkCmdClearAttachments(cmd, clearAttachments.size(), clearAttachments.data(), 1, &rect);
}

template <>
void RenderTarget<kVk>::Clear(
	CommandBufferHandle<kVk> cmd, const ClearValue<kVk>& value, uint32_t index)
{
	ZoneScopedN("RenderTarget::Clear");

	VkImageAspectFlags aspectFlags{};
	if (HasColorComponent(GetRenderTargetDesc().imageFormats[index]))
	{
		aspectFlags = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	else 
	{
		if (HasDepthComponent(GetRenderTargetDesc().imageFormats[index]))
			aspectFlags |= VK_IMAGE_ASPECT_DEPTH_BIT;
		if (HasStencilComponent(GetRenderTargetDesc().imageFormats[index]))
			aspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}

	Transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspectFlags, index);

	VkImageSubresourceRange range{aspectFlags, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};

	if (HasColorComponent(GetRenderTargetDesc().imageFormats[index]))
	{
		vkCmdClearColorImage(
			cmd,
			GetRenderTargetDesc().images[index],
			GetLayout(index),
			&value.color,
			1,
			&range);
	}
	else
	{
		vkCmdClearDepthStencilImage(
			cmd,
			GetRenderTargetDesc().images[index],
			GetLayout(index),
			&value.depthStencil,
			1,
			&range);
	}
}

template <>
void RenderTarget<kVk>::SetLoadOp(AttachmentLoadOp<kVk> loadOp, uint32_t index, AttachmentLoadOp<kVk> stencilLoadOp)
{
	myAttachmentDescs[index].loadOp = loadOp;
	if (HasStencilComponent(GetRenderTargetDesc().imageFormats[index]))
		myAttachmentDescs[index].stencilLoadOp = stencilLoadOp;
}

template <>
void RenderTarget<kVk>::SetStoreOp(AttachmentStoreOp<kVk> storeOp, uint32_t index, AttachmentStoreOp<kVk> stencilStoreOp)
{
	myAttachmentDescs[index].storeOp = storeOp;
	if (HasStencilComponent(GetRenderTargetDesc().imageFormats[index]))
		myAttachmentDescs[index].stencilStoreOp = stencilStoreOp;
}

template <>
void RenderTarget<kVk>::NextSubpass(CommandBufferHandle<kVk> cmd, SubpassContents<kVk> contents)
{
	ZoneScopedN("RenderTarget::NextSubpass");

	vkCmdNextSubpass(cmd, contents);
}

template <>
const RenderTargetPassHandle<kVk>& RenderTarget<kVk>::InternalGetValues()
{
	InternalUpdateAttachments(GetRenderTargetDesc());
	
	if (GetRenderTargetDesc().useDynamicRendering)
	{
		static const RenderTargetPassHandle<kVk> kEmptyRTHandle{};
		return kEmptyRTHandle;
	}

	InternalUpdateRenderPasses(GetRenderTargetDesc());

	return InternalUpdateMap(GetRenderTargetDesc());
}

template <>
const RenderTargetBeginInfo<kVk>& RenderTarget<kVk>::Begin(CommandBufferHandle<kVk> cmd, SubpassContents<kVk> contents)
{
	ZoneScopedN("RenderTarget::Begin");

	ASSERT(!myRenderTargetBeginInfo.has_value());

	const auto& desc = GetRenderTargetDesc();

	if (desc.useDynamicRendering)
	{
		static auto vkCmdBeginRenderingKHR = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
			vkGetInstanceProcAddr(
				*InternalGetDevice()->GetInstance(),
				"vkCmdBeginRenderingKHR"));
		ASSERT(vkCmdBeginRenderingKHR != nullptr);

		myRenderTargetBeginInfo = DynamicRenderingInfo<kVk>
		{
			.renderInfo = RenderingInfo<kVk>
			{
				.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
				.pNext = nullptr,
				.flags = contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0u,
				.renderArea = {0, 0, desc.extent.width, desc.extent.height},
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
				.flags = contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0u,
				.viewMask = 0,
				.colorAttachmentCount = static_cast<uint32_t>(myColorAttachmentFormats.size()),
				.pColorAttachmentFormats = myColorAttachmentFormats.data(),
				.depthAttachmentFormat = myDepthAttachmentFormat.value_or(Format<kVk>{}),
				.stencilAttachmentFormat = myStencilAttachmentFormat.value_or(Format<kVk>{}),
				.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
			}
		};

		vkCmdBeginRenderingKHR(cmd, &std::get<DynamicRenderingInfo<kVk>>(myRenderTargetBeginInfo.value()).renderInfo);
	}
	else
	{
		ENSURE(desc.clearValues.size() == myAttachments.size());

		const auto& [renderPass, frameBuffer] = InternalGetValues();

		myRenderTargetBeginInfo = RenderPassBeginInfo<kVk>{
			VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			nullptr,
			renderPass,
			frameBuffer,
			{{0, 0}, {desc.extent.width, desc.extent.height}},
			static_cast<uint32_t>(desc.clearValues.size()),
			desc.clearValues.data()};

		vkCmdBeginRenderPass(cmd, &std::get<VkRenderPassBeginInfo>(myRenderTargetBeginInfo.value()), contents);
	}

	return myRenderTargetBeginInfo.value();
}

template <>
void RenderTarget<kVk>::End(CommandBufferHandle<kVk> cmd)
{
	ASSERT(myRenderTargetBeginInfo.has_value());

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

	myRenderTargetBeginInfo = {};
}

template <>
RenderTarget<kVk>::RenderTarget(
	const std::shared_ptr<Device<kVk>>& device, const RenderTargetCreateDesc<kVk>& desc)
	: DeviceObject(device, {}, uuids::uuid_system_generator{}())
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

	ASSERT(!myRenderTargetBeginInfo.has_value());

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
	ASSERT(!myRenderTargetBeginInfo.has_value());

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
	ASSERT(!myRenderTargetBeginInfo.has_value());

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
