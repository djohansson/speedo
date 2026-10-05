#pragma once

#include <rhi/enums.h>

#include <array>
#include <utility>

#include <core/assert.h>

#include <vulkan/vulkan.h>

// conversions between rhi's neutral vocabulary (rhi/enums.h) and vulkan's
namespace rhi::vk
{

[[nodiscard]] constexpr VkFormat ToVk(Format format) noexcept
{
	switch (format)
	{
	case Format::kUndefined: return VK_FORMAT_UNDEFINED;
	case Format::kR8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
	case Format::kR8G8B8A8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
	case Format::kB8G8R8A8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
	case Format::kB8G8R8A8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
	case Format::kR8G8B8Unorm: return VK_FORMAT_R8G8B8_UNORM;
	case Format::kB8G8R8Unorm: return VK_FORMAT_B8G8R8_UNORM;
	case Format::kA2R10G10B10UnormPack32: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
	case Format::kA2B10G10R10UnormPack32: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	case Format::kR16G16B16A16Sfloat: return VK_FORMAT_R16G16B16A16_SFLOAT;
	case Format::kD32Sfloat: return VK_FORMAT_D32_SFLOAT;
	case Format::kD32SfloatS8Uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;
	case Format::kD24UnormS8Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
	case Format::kBC1RgbUnorm: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
	case Format::kBC1RgbSrgb: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
	case Format::kBC3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
	case Format::kBC3Srgb: return VK_FORMAT_BC3_SRGB_BLOCK;
	case Format::kBC4Unorm: return VK_FORMAT_BC4_UNORM_BLOCK;
	case Format::kBC5Unorm: return VK_FORMAT_BC5_UNORM_BLOCK;
	}
	return VK_FORMAT_UNDEFINED;
}

// the neutral format of a vulkan one, or kUndefined if it has none (e.g. a swapchain format rhi doesn't name)
[[nodiscard]] constexpr Format FromVk(VkFormat format) noexcept
{
	for (auto candidate : {
			 Format::kR8G8B8A8Unorm, Format::kR8G8B8A8Srgb, Format::kB8G8R8A8Unorm, Format::kB8G8R8A8Srgb,
			 Format::kR8G8B8Unorm, Format::kB8G8R8Unorm, Format::kA2R10G10B10UnormPack32,
			 Format::kA2B10G10R10UnormPack32, Format::kR16G16B16A16Sfloat, Format::kD32Sfloat, Format::kD32SfloatS8Uint, Format::kD24UnormS8Uint,
			 Format::kBC1RgbUnorm, Format::kBC1RgbSrgb, Format::kBC3Unorm, Format::kBC3Srgb, Format::kBC4Unorm,
			 Format::kBC5Unorm})
		if (ToVk(candidate) == format)
			return candidate;
	return Format::kUndefined;
}

[[nodiscard]] constexpr VkImageLayout ToVk(ImageLayout layout) noexcept
{
	switch (layout)
	{
	case ImageLayout::kUndefined: return VK_IMAGE_LAYOUT_UNDEFINED;
	case ImageLayout::kGeneral: return VK_IMAGE_LAYOUT_GENERAL;
	case ImageLayout::kColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	case ImageLayout::kDepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	case ImageLayout::kShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	case ImageLayout::kTransferSource: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	case ImageLayout::kTransferDestination: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	case ImageLayout::kPresent: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	}
	return VK_IMAGE_LAYOUT_UNDEFINED;
}

[[nodiscard]] constexpr ImageLayout FromVk(VkImageLayout layout) noexcept
{
	for (auto candidate : {
			 ImageLayout::kGeneral, ImageLayout::kColorAttachment, ImageLayout::kDepthStencilAttachment,
			 ImageLayout::kShaderReadOnly, ImageLayout::kTransferSource, ImageLayout::kTransferDestination,
			 ImageLayout::kPresent})
		if (ToVk(candidate) == layout)
			return candidate;
	return ImageLayout::kUndefined;
}

[[nodiscard]] constexpr VkImageTiling ToVk(ImageTiling tiling) noexcept
{
	return tiling == ImageTiling::kLinear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
}

[[nodiscard]] constexpr VkImageAspectFlags ToVk(ImageAspect aspect) noexcept
{
	VkImageAspectFlags flags = 0;
	if (Any(aspect & ImageAspect::kColor))
		flags |= VK_IMAGE_ASPECT_COLOR_BIT;
	if (Any(aspect & ImageAspect::kDepth))
		flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
	if (Any(aspect & ImageAspect::kStencil))
		flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
	return flags;
}

[[nodiscard]] constexpr VkImageUsageFlags ToVk(ImageUsage usage) noexcept
{
	VkImageUsageFlags flags = 0;
	if (Any(usage & ImageUsage::kTransferSource))
		flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (Any(usage & ImageUsage::kTransferDestination))
		flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (Any(usage & ImageUsage::kSampled))
		flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
	if (Any(usage & ImageUsage::kStorage))
		flags |= VK_IMAGE_USAGE_STORAGE_BIT;
	if (Any(usage & ImageUsage::kColorAttachment))
		flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (Any(usage & ImageUsage::kDepthStencilAttachment))
		flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	return flags;
}

[[nodiscard]] constexpr VkBufferUsageFlags ToVk(BufferUsage usage) noexcept
{
	VkBufferUsageFlags flags = 0;
	if (Any(usage & BufferUsage::kTransferSource))
		flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	if (Any(usage & BufferUsage::kTransferDestination))
		flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (Any(usage & BufferUsage::kUniform))
		flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	if (Any(usage & BufferUsage::kStorage))
		flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if (Any(usage & BufferUsage::kIndex))
		flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	if (Any(usage & BufferUsage::kVertex))
		flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	return flags;
}

[[nodiscard]] constexpr VkMemoryPropertyFlags ToVk(MemoryProperty properties) noexcept
{
	VkMemoryPropertyFlags flags = 0;
	if (Any(properties & MemoryProperty::kDeviceLocal))
		flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	if (Any(properties & MemoryProperty::kHostVisible))
		flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	if (Any(properties & MemoryProperty::kHostCoherent))
		flags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	return flags;
}

[[nodiscard]] constexpr VkFormatFeatureFlags ToVk(FormatFeature features) noexcept
{
	VkFormatFeatureFlags flags = 0;
	if (Any(features & FormatFeature::kSampled))
		flags |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	if (Any(features & FormatFeature::kColorAttachment))
		flags |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
	if (Any(features & FormatFeature::kDepthStencilAttachment))
		flags |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if (Any(features & FormatFeature::kTransferSource))
		flags |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	if (Any(features & FormatFeature::kTransferDestination))
		flags |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	return flags;
}

[[nodiscard]] constexpr VkAttachmentLoadOp ToVk(LoadOp op) noexcept
{
	switch (op)
	{
	case LoadOp::kLoad: return VK_ATTACHMENT_LOAD_OP_LOAD;
	case LoadOp::kClear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
	case LoadOp::kDontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	}
	return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

[[nodiscard]] constexpr VkAttachmentStoreOp ToVk(StoreOp op) noexcept
{
	return op == StoreOp::kStore ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

[[nodiscard]] constexpr VkSubpassContents ToVk(SubpassContents contents) noexcept
{
	return contents == SubpassContents::kSecondaryCommandBuffers ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
																   : VK_SUBPASS_CONTENTS_INLINE;
}

[[nodiscard]] constexpr VkPipelineBindPoint ToVk(PipelineBindPoint bindPoint) noexcept
{
	return bindPoint == PipelineBindPoint::kCompute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
}

[[nodiscard]] constexpr PipelineBindPoint FromVk(VkPipelineBindPoint bindPoint) noexcept
{
	return bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE ? PipelineBindPoint::kCompute : PipelineBindPoint::kGraphics;
}

[[nodiscard]] constexpr VkPipelineStageFlags ToVk(PipelineStage stages) noexcept
{
	VkPipelineStageFlags flags = 0;
	if (Any(stages & PipelineStage::kTopOfPipe))
		flags |= VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	if (Any(stages & PipelineStage::kTransfer))
		flags |= VK_PIPELINE_STAGE_TRANSFER_BIT;
	if (Any(stages & PipelineStage::kVertexShader))
		flags |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
	if (Any(stages & PipelineStage::kFragmentShader))
		flags |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	if (Any(stages & PipelineStage::kComputeShader))
		flags |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	if (Any(stages & PipelineStage::kColorAttachmentOutput))
		flags |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	if (Any(stages & PipelineStage::kAllGraphics))
		flags |= VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
	if (Any(stages & PipelineStage::kAllCommands))
		flags |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	if (Any(stages & PipelineStage::kBottomOfPipe))
		flags |= VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	return flags;
}

[[nodiscard]] constexpr VkAccessFlags ToVk(Access access) noexcept
{
	VkAccessFlags flags = 0;
	if (Any(access & Access::kShaderRead))
		flags |= VK_ACCESS_SHADER_READ_BIT;
	if (Any(access & Access::kShaderWrite))
		flags |= VK_ACCESS_SHADER_WRITE_BIT;
	if (Any(access & Access::kTransferRead))
		flags |= VK_ACCESS_TRANSFER_READ_BIT;
	if (Any(access & Access::kTransferWrite))
		flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
	if (Any(access & Access::kHostWrite))
		flags |= VK_ACCESS_HOST_WRITE_BIT;
	if (Any(access & Access::kIndexRead))
		flags |= VK_ACCESS_INDEX_READ_BIT;
	return flags;
}

[[nodiscard]] constexpr VkIndexType ToVk(IndexType type) noexcept
{
	return type == IndexType::kUint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
}

[[nodiscard]] constexpr VkSemaphoreType ToVk(SemaphoreType type) noexcept
{
	return type == SemaphoreType::kTimeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY;
}

[[nodiscard]] constexpr VkFilter ToVk(Filter filter) noexcept
{
	return filter == Filter::kNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

[[nodiscard]] constexpr VkSamplerMipmapMode ToVkMipmapMode(Filter filter) noexcept
{
	return filter == Filter::kNearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

[[nodiscard]] constexpr VkSamplerAddressMode ToVk(AddressMode mode) noexcept
{
	switch (mode)
	{
	case AddressMode::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	case AddressMode::kMirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	case AddressMode::kClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case AddressMode::kClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	}
	return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

[[nodiscard]] constexpr VkSamplerCreateInfo ToVk(const SamplerDesc& desc) noexcept
{
	return VkSamplerCreateInfo{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0U,
		.magFilter = ToVk(desc.magFilter),
		.minFilter = ToVk(desc.minFilter),
		.mipmapMode = ToVkMipmapMode(desc.mipmapFilter),
		.addressModeU = ToVk(desc.addressModeU),
		.addressModeV = ToVk(desc.addressModeV),
		.addressModeW = ToVk(desc.addressModeW),
		.mipLodBias = desc.mipLodBias,
		.anisotropyEnable = desc.maxAnisotropy > 1.0F ? VK_TRUE : VK_FALSE,
		.maxAnisotropy = desc.maxAnisotropy,
		.compareEnable = VK_FALSE,
		.compareOp = VK_COMPARE_OP_ALWAYS,
		.minLod = desc.minLod,
		.maxLod = desc.maxLod,
		.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
		.unnormalizedCoordinates = VK_FALSE};
}

[[nodiscard]] constexpr VkExtent2D ToVk(Extent2d extent) noexcept { return {.width = extent.width, .height = extent.height}; }
[[nodiscard]] constexpr Extent2d FromVk(VkExtent2D extent) noexcept { return {.width = extent.width, .height = extent.height}; }

[[nodiscard]] constexpr VkShaderStageFlags ToVk(ShaderStage stages) noexcept
{
	if (stages == ShaderStage::kAll)
		return VK_SHADER_STAGE_ALL;

	constexpr std::array<std::pair<ShaderStage, VkShaderStageFlagBits>, 12> kStages{{
		{ShaderStage::kVertex, VK_SHADER_STAGE_VERTEX_BIT},
		{ShaderStage::kTessellationControl, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT},
		{ShaderStage::kTessellationEvaluation, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT},
		{ShaderStage::kGeometry, VK_SHADER_STAGE_GEOMETRY_BIT},
		{ShaderStage::kFragment, VK_SHADER_STAGE_FRAGMENT_BIT},
		{ShaderStage::kCompute, VK_SHADER_STAGE_COMPUTE_BIT},
		{ShaderStage::kRayGeneration, VK_SHADER_STAGE_RAYGEN_BIT_KHR},
		{ShaderStage::kAnyHit, VK_SHADER_STAGE_ANY_HIT_BIT_KHR},
		{ShaderStage::kClosestHit, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
		{ShaderStage::kMiss, VK_SHADER_STAGE_MISS_BIT_KHR},
		{ShaderStage::kIntersection, VK_SHADER_STAGE_INTERSECTION_BIT_KHR},
		{ShaderStage::kCallable, VK_SHADER_STAGE_CALLABLE_BIT_KHR},
	}};

	VkShaderStageFlags flags = 0;
	for (auto [stage, vkStage] : kStages)
		if (Any(stages & stage))
			flags |= vkStage;
	return flags;
}

[[nodiscard]] constexpr VkDescriptorType ToVk(DescriptorType type) noexcept
{
	switch (type)
	{
	case DescriptorType::kSampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
	case DescriptorType::kCombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	case DescriptorType::kSampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	case DescriptorType::kStorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	case DescriptorType::kUniformTexelBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
	case DescriptorType::kStorageTexelBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
	case DescriptorType::kUniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	case DescriptorType::kStorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	case DescriptorType::kUniformBufferDynamic: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	case DescriptorType::kStorageBufferDynamic: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
	case DescriptorType::kInputAttachment: return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
	case DescriptorType::kInlineUniformBlock: return VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
	case DescriptorType::kAccelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	}
	ASSERT(false);
	return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

// the result of an acquire or present
[[nodiscard]] constexpr PresentResult ToPresentResult(VkResult result) noexcept
{
	switch (result)
	{
	case VK_SUCCESS: return PresentResult::kSuccess;
	case VK_SUBOPTIMAL_KHR: return PresentResult::kSuboptimal;
	case VK_ERROR_OUT_OF_DATE_KHR: return PresentResult::kOutOfDate;
	default: return PresentResult::kError;
	}
}

[[nodiscard]] constexpr VkViewport ToVk(const Viewport& viewport) noexcept
{
	return {viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth};
}

[[nodiscard]] constexpr VkRect2D ToVk(const Rect& rect) noexcept
{
	return {.offset = {.x = rect.x, .y = rect.y}, .extent = {.width = rect.width, .height = rect.height}};
}

// as a color, or as a depth and stencil value, by the aspect of what it clears
[[nodiscard]] inline VkClearValue ToVk(const ClearValue& value, ImageAspect aspect) noexcept
{
	VkClearValue result{};
	if (Any(aspect & (ImageAspect::kDepth | ImageAspect::kStencil)))
		result.depthStencil = {.depth = value.depth, .stencil = value.stencil};
	else
		for (size_t i = 0; i < value.color.size(); i++)
			result.color.float32[i] = value.color[i];
	return result;
}

} // namespace rhi::vk
