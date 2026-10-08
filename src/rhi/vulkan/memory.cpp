#include <rhi/memory.h>
#include <rhi/vulkan/convert.h>
#include <rhi/vulkan/utils.h>

namespace rhi
{

template <>
MemoryBlock<kVk>::MemoryBlock(
	AllocatorHandle<kVk> allocator, const MemoryRequirements& requirements, MemoryProperty flags, const char* name)
	: myAllocator(allocator)
	, mySize(requirements.size)
{
	VkMemoryRequirements vkRequirements{
		.size = requirements.size, .alignment = requirements.alignment, .memoryTypeBits = requirements.memoryTypeBits};

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT | VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	allocInfo.usage = Any(flags & MemoryProperty::kDeviceLocal) ? VMA_MEMORY_USAGE_GPU_ONLY : VMA_MEMORY_USAGE_UNKNOWN;
	allocInfo.requiredFlags = vk::ToVk(flags);
	allocInfo.pUserData = const_cast<char*>(name); //NOLINT(cppcoreguidelines-pro-type-const-cast)

	VK_CHECK(vmaAllocateMemory(allocator, &vkRequirements, &allocInfo, &myAllocation, nullptr));
}

template <>
MemoryBlock<kVk>::~MemoryBlock()
{
	if (myAllocation != VK_NULL_HANDLE)
		vmaFreeMemory(myAllocator, myAllocation);
}

} // namespace rhi
