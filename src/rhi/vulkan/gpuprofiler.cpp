// the Vulkan backend's gpu profiler: its first timestamp (the rest is every backend's, rhi/gpuprofiler.inl)
#include <rhi/gpuprofiler.h>
#include <rhi/device.h>
#include <rhi/vulkan/utils.h>

#include <core/profiling.h>

namespace rhi
{

#if (SPEEDO_PROFILING_LEVEL > 0)

// written by setup on queue, otherwise idle, and read back
template <>
uint64_t GpuProfiler<kVk>::InternalFirstTimestamp(QueueHandle<kVk> queue, CommandBufferHandle<kVk> setup)
{
	VkCommandBufferBeginInfo beginInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VK_CHECK(vkBeginCommandBuffer(setup, &beginInfo));
	myPool.WriteTimestamp(setup, 0, PipelineStage::kTopOfPipe);
	VK_CHECK(vkEndCommandBuffer(setup));
	VkSubmitInfo submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &setup};
	VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
	VK_CHECK(vkQueueWaitIdle(queue));
	uint64_t gpuTime = 0;
	VK_CHECK(vkGetQueryPoolResults(
		myPool.GetDevice(),
		myPool,
		0,
		1,
		sizeof(gpuTime),
		&gpuTime,
		sizeof(gpuTime),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
	myPool.Reset(0, 1);
	return gpuTime;
}

#endif

} // namespace rhi

#include <rhi/gpuprofiler.inl>

template class rhi::GpuProfiler<kVk>;
