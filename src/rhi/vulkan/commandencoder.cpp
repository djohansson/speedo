#include <rhi/buffer.h>
#include <rhi/commandencoder.h>
#include <rhi/image.h>
#include <rhi/vulkan/utils.h>
#include <rhi/vulkan/convert.h>

namespace rhi
{

template <>
void CommandEncoder<kVk>::SetViewport(const Viewport& viewport) const
{
	auto vkViewport = vk::ToVk(viewport);
	vkCmdSetViewport(myCmd, 0, 1, &vkViewport);
}

template <>
void CommandEncoder<kVk>::SetScissor(const Rect& rect) const
{
	auto vkRect = vk::ToVk(rect);
	vkCmdSetScissor(myCmd, 0, 1, &vkRect);
}

template <>
void CommandEncoder<kVk>::SetCullMode(CullMode mode) const
{
	gVkCmdSetCullModeEXT(myCmd, vk::ToVk(mode));
}

template <>
void CommandEncoder<kVk>::SetFrontFace(FrontFace face) const
{
	gVkCmdSetFrontFaceEXT(myCmd, vk::ToVk(face));
}

template <>
void CommandEncoder<kVk>::BindIndexBuffer(const Buffer<kVk>& buffer, uint64_t offset, IndexType type) const
{
	vkCmdBindIndexBuffer(myCmd, buffer, offset, vk::ToVk(type));
}

template <>
void CommandEncoder<kVk>::DrawIndexed(
	uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) const
{
	vkCmdDrawIndexed(myCmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

template <>
void CommandEncoder<kVk>::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) const
{
	vkCmdDispatch(myCmd, groupCountX, groupCountY, groupCountZ);
}

template <>
void CommandEncoder<kVk>::UpdateBuffer(const Buffer<kVk>& buffer, uint64_t offset, std::span<const std::byte> data) const
{
	vkCmdUpdateBuffer(myCmd, buffer, offset, data.size(), data.data());
}

template <>
void CommandEncoder<kVk>::Barrier(PipelineStage srcStages, Access srcAccess, PipelineStage dstStages, Access dstAccess) const
{
	VkMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = vk::ToVk(srcAccess),
		.dstAccessMask = vk::ToVk(dstAccess)};
	vkCmdPipelineBarrier(myCmd, vk::ToVk(srcStages), vk::ToVk(dstStages), 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

namespace commandencoder
{

// the two halves of an ownership transfer: the release makes srcAccess available, the acquire makes it visible to
// dstAccess. neither waits for or blocks anything else (the semaphore between them orders them).
struct OwnershipBarrier
{
	VkPipelineStageFlags srcStages;
	VkAccessFlags srcAccess;
	VkPipelineStageFlags dstStages;
	VkAccessFlags dstAccess;
};

static OwnershipBarrier Release(PipelineStage srcStages, Access srcAccess)
{
	return {vk::ToVk(srcStages), vk::ToVk(srcAccess), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0};
}

static OwnershipBarrier Acquire(PipelineStage dstStages, Access dstAccess)
{
	return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, vk::ToVk(dstStages), vk::ToVk(dstAccess)};
}

static void TransferOwnership(
	VkCommandBuffer cmd, const Buffer<kVk>& buffer, uint32_t srcFamily, uint32_t dstFamily, const OwnershipBarrier& barrier)
{
	if (srcFamily == dstFamily)
		return;

	VkBufferMemoryBarrier bufferBarrier{
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = barrier.srcAccess,
		.dstAccessMask = barrier.dstAccess,
		.srcQueueFamilyIndex = srcFamily,
		.dstQueueFamilyIndex = dstFamily,
		.buffer = buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE};
	vkCmdPipelineBarrier(cmd, barrier.srcStages, barrier.dstStages, 0, 0, nullptr, 1, &bufferBarrier, 0, nullptr);
}

static void TransferOwnership(
	VkCommandBuffer cmd, const Image<kVk>& image, uint32_t srcFamily, uint32_t dstFamily, const OwnershipBarrier& barrier)
{
	if (srcFamily == dstFamily)
		return;

	const auto& desc = image.GetDesc();
	auto layout = vk::ToVk(desc.layout);
	VkImageMemoryBarrier imageBarrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = barrier.srcAccess,
		.dstAccessMask = barrier.dstAccess,
		.oldLayout = layout,
		.newLayout = layout,
		.srcQueueFamilyIndex = srcFamily,
		.dstQueueFamilyIndex = dstFamily,
		.image = image,
		.subresourceRange = {
			.aspectMask = vk::ToVk(AspectOf(desc.format)),
			.baseMipLevel = 0,
			.levelCount = VK_REMAINING_MIP_LEVELS,
			.baseArrayLayer = 0,
			.layerCount = VK_REMAINING_ARRAY_LAYERS}};
	vkCmdPipelineBarrier(cmd, barrier.srcStages, barrier.dstStages, 0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
}

} // namespace commandencoder

template <>
void CommandEncoder<kVk>::ReleaseOwnership(
	const Buffer<kVk>& buffer, uint32_t srcFamily, uint32_t dstFamily, PipelineStage srcStages, Access srcAccess) const
{
	commandencoder::TransferOwnership(myCmd, buffer, srcFamily, dstFamily, commandencoder::Release(srcStages, srcAccess));
}

template <>
void CommandEncoder<kVk>::AcquireOwnership(
	const Buffer<kVk>& buffer, uint32_t srcFamily, uint32_t dstFamily, PipelineStage dstStages, Access dstAccess) const
{
	commandencoder::TransferOwnership(myCmd, buffer, srcFamily, dstFamily, commandencoder::Acquire(dstStages, dstAccess));
}

template <>
void CommandEncoder<kVk>::ReleaseOwnership(
	const Image<kVk>& image, uint32_t srcFamily, uint32_t dstFamily, PipelineStage srcStages, Access srcAccess) const
{
	commandencoder::TransferOwnership(myCmd, image, srcFamily, dstFamily, commandencoder::Release(srcStages, srcAccess));
}

template <>
void CommandEncoder<kVk>::AcquireOwnership(
	const Image<kVk>& image, uint32_t srcFamily, uint32_t dstFamily, PipelineStage dstStages, Access dstAccess) const
{
	commandencoder::TransferOwnership(myCmd, image, srcFamily, dstFamily, commandencoder::Acquire(dstStages, dstAccess));
}

} // namespace rhi
