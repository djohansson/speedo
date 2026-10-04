#include <rhi/buffer.h>
#include <rhi/commandencoder.h>
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

} // namespace rhi
