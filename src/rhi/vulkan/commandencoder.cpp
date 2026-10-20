#include <rhi/buffer.h>
#include <rhi/command.h>
#include <rhi/commandencoder.h>
#include <rhi/image.h>
#include <rhi/vulkan/utils.h>
#include <rhi/vulkan/convert.h>

#include <algorithm>

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
void CommandEncoder<kVk>::SetStencilReference(uint32_t reference) const
{
	vkCmdSetStencilReference(myCmd, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
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
void CommandEncoder<kVk>::DrawIndexedIndirect(const Buffer<kVk>& buffer, uint64_t offset, uint32_t drawCount, uint32_t stride) const
{
	vkCmdDrawIndexedIndirect(myCmd, buffer, offset, drawCount, stride);
}

template <>
void CommandEncoder<kVk>::DrawIndexedIndirectCount(
	const Buffer<kVk>& buffer, uint64_t offset, const Buffer<kVk>& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride) const
{
	vkCmdDrawIndexedIndirectCount(myCmd, buffer, offset, countBuffer, countOffset, maxDrawCount, stride);
}

template <>
void CommandEncoder<kVk>::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) const
{
	vkCmdDispatch(myCmd, groupCountX, groupCountY, groupCountZ);
}

template <>
void CommandEncoder<kVk>::DispatchThreads(const ComputeLaunchParameters& launch, uint32_t threadsX, uint32_t threadsY, uint32_t threadsZ) const
{
	auto groups = [](uint32_t threads, uint64_t size) { return static_cast<uint32_t>((threads + std::max<uint64_t>(size, 1) - 1) / std::max<uint64_t>(size, 1)); };
	Dispatch(groups(threadsX, launch.threadGroupSize[0]), groups(threadsY, launch.threadGroupSize[1]), groups(threadsZ, launch.threadGroupSize[2]));
}

template <>
void CommandEncoder<kVk>::UpdateBuffer(const Buffer<kVk>& buffer, uint64_t offset, std::span<const std::byte> data) const
{
	vkCmdUpdateBuffer(myCmd, buffer, offset, data.size(), data.data());
}

template <>
void CommandEncoder<kVk>::ExecuteCommands(std::vector<CommandBuffer<kVk>>&& secondaries) const
{
	ZoneScopedN("CommandEncoder::ExecuteCommands");

	if (secondaries.empty())
		return;

	std::vector<CommandBufferHandle<kVk>> handles;
	handles.reserve(secondaries.size());
	for (auto& secondary : secondaries)
	{
		ENSURE(secondary.IsSecondary());
		handles.push_back(secondary.Release());
	}
	vkCmdExecuteCommands(myCmd, static_cast<uint32_t>(handles.size()), handles.data());
}

template <>
void CommandEncoder<kVk>::FillBuffer(const Buffer<kVk>& buffer, uint64_t offset, uint64_t size, uint32_t value) const
{
	vkCmdFillBuffer(myCmd, buffer, offset, size == 0 ? VK_WHOLE_SIZE : size, value);
}

template <>
void CommandEncoder<kVk>::CopyImageToBuffer(ImageHandle<kVk> image, Extent2d extent, const Buffer<kVk>& buffer) const
{
	VkBufferImageCopy region{
		.bufferOffset = 0,
		.bufferRowLength = 0,
		.bufferImageHeight = 0,
		.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
		.imageOffset = {.x = 0, .y = 0, .z = 0},
		.imageExtent = {.width = extent.width, .height = extent.height, .depth = 1}};
	vkCmdCopyImageToBuffer(myCmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
}

template <>
void CommandEncoder<kVk>::CopyBuffer(const Buffer<kVk>& source, const Buffer<kVk>& destination, uint64_t size, uint64_t offset) const
{
	VkBufferCopy region{.srcOffset = offset, .dstOffset = offset, .size = size};
	vkCmdCopyBuffer(myCmd, source, destination, 1, &region);
}

template <>
void CommandEncoder<kVk>::CopyImageToBuffer(const Image<kVk>& image, const Buffer<kVk>& buffer) const
{
	CopyImageToBuffer(static_cast<ImageHandle<kVk>>(image), image.GetDesc().mipLevels.front().extent, buffer);
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
	return {.srcStages=vk::ToVk(srcStages), .srcAccess=vk::ToVk(srcAccess), .dstStages=VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, .dstAccess=0};
}

static OwnershipBarrier Acquire(PipelineStage dstStages, Access dstAccess)
{
	return {.srcStages=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, .srcAccess=0, .dstStages=vk::ToVk(dstStages), .dstAccess=vk::ToVk(dstAccess)};
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
