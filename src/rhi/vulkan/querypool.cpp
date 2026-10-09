#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/querypool.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>

#include <utility>

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
			createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
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
void QueryPool<kVk>::WriteTimestamp(CommandBufferHandle<kVk> cmd, uint32_t index) const
{
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, myPool, index);
}

template <>
bool QueryPool<kVk>::Read(uint32_t first, std::span<uint64_t> timestamps) const
{
	if (timestamps.empty())
		return true;
	auto result = vkGetQueryPoolResults(
		GetDevice(),
		myPool,
		first,
		static_cast<uint32_t>(timestamps.size()),
		timestamps.size_bytes(),
		timestamps.data(),
		sizeof(uint64_t),
		VK_QUERY_RESULT_64_BIT);
	return result == VK_SUCCESS;
}

} // namespace rhi
