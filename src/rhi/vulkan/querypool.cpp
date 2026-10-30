#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/querypool.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/convert.h>
#include <rhi/vulkan/utils.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(QueryPool<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(QueryPool<kVk>);

template <>
QueryPool<kVk>::QueryPool(CreateDescType&& desc, QueryPoolHandle<kVk>&& pool)
	: DeviceObject<QueryPool<kVk>>(std::forward<CreateDescType>(desc))
	, myPool(std::forward<QueryPoolHandle<kVk>>(pool))
{}

template <>
QueryPool<kVk>::QueryPool(CreateDescType&& desc)
	: QueryPool(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[this, &desc]
		{
			VkQueryPoolCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
			createInfo.queryType = desc.type == QueryType::kTimestamp ? VK_QUERY_TYPE_TIMESTAMP : VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
			createInfo.queryCount = desc.count;
			QueryPoolHandle<kVk> pool;
			VK_CHECK(vkCreateQueryPool(desc.device, &createInfo, &GetInstance().GetHostAllocationCallbacks(), &pool));
			Track(desc.device, VK_OBJECT_TYPE_QUERY_POOL, pool, GetDebugName(desc));
			return pool;
		}())
{}

template <>
void QueryPool<kVk>::Swap(QueryPool& rhs) noexcept
{
	DeviceObject<QueryPool<kVk>>::Swap(rhs);
	std::swap(myPool, rhs.myPool);
}

template <>
QueryPool<kVk>::QueryPool(QueryPool<kVk>&& other) noexcept
{
	Swap(other);
}

template <>
QueryPool<kVk>::~QueryPool()
{
	if (!IsValid())
		return;
	Untrack(VK_OBJECT_TYPE_QUERY_POOL, myPool);
	vkDestroyQueryPool(GetDevice(), myPool, &GetInstance().GetHostAllocationCallbacks());
}

template <>
QueryPool<kVk>& QueryPool<kVk>::operator=(QueryPool<kVk>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
void QueryPool<kVk>::Reset(CommandBufferHandle<kVk> cmd, uint32_t first, uint32_t count) const
{
	if (count > 0)
		vkCmdResetQueryPool(cmd, myPool, first, count);
}

template <>
void QueryPool<kVk>::Reset(uint32_t first, uint32_t count) const
{
	if (count > 0)
		vkResetQueryPool(GetDevice(), myPool, first, count);
}

template <>
void QueryPool<kVk>::WriteTimestamp(CommandBufferHandle<kVk> cmd, uint32_t index, PipelineStage stage) const
{
	vkCmdWriteTimestamp(cmd, static_cast<VkPipelineStageFlagBits>(vk::ToVk(stage)), myPool, index);
}

template <>
void QueryPool<kVk>::Read(uint32_t first, std::span<std::optional<uint64_t>> timestamps) const
{
	std::ranges::fill(timestamps, std::nullopt);
	if (timestamps.empty())
		return;
	// each a value and its availability: VK_NOT_READY (some unwritten) still returns the ones written
	std::vector<uint64_t> results(timestamps.size() * 2);
	auto result = vkGetQueryPoolResults(
		GetDevice(),
		myPool,
		first,
		static_cast<uint32_t>(timestamps.size()),
		results.size() * sizeof(uint64_t),
		results.data(),
		2 * sizeof(uint64_t),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	if (result != VK_SUCCESS && result != VK_NOT_READY)
		return;
	for (size_t queryIt = 0; queryIt < timestamps.size(); queryIt++)
		if (results[(queryIt * 2) + 1] != 0)
			timestamps[queryIt] = results[queryIt * 2];
}

} // namespace rhi
