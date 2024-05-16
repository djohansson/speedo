#include "../fence.h"
#include "utils.h"

template <>
Fence<kVk>::Fence(
	const std::shared_ptr<Device<kVk>>& device,
	FenceHandle<kVk>&& fence)
	: DeviceObject(
		  device,
		  {"_Fence"},
		  1,
		  VK_OBJECT_TYPE_FENCE,
		  reinterpret_cast<uint64_t*>(&fence))
	, myFence(std::forward<FenceHandle<kVk>>(fence))
{}

template <>
Fence<kVk>::Fence(
	const std::shared_ptr<Device<kVk>>& device,
	FenceCreateDesc<kVk>&& desc)
	: Fence(device, [&device, &desc]
	{
		FenceHandle<kVk> fence;
		VkFenceCreateInfo createInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		createInfo.flags = desc.flags;
		VK_CHECK(vkCreateFence(*device, &createInfo, &device->GetInstance()->GetHostAllocationCallbacks(), &fence));
		return fence;
	}())
{}

template <>
Fence<kVk>::Fence(Fence<kVk>&& other) noexcept
	: DeviceObject(std::forward<Fence<kVk>>(other))
	, myFence(std::exchange(other.myFence, {}))
{}

template <>
Fence<kVk>::~Fence()
{
	if (myFence != nullptr)
		vkDestroyFence(*GetDevice(), myFence, &GetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
Fence<kVk>& Fence<kVk>::operator=(Fence<kVk>&& other) noexcept
{
	DeviceObject<kVk>::operator=(std::forward<Fence<kVk>>(other));
	myFence = std::exchange(other.myFence, {});
	return *this;
}

template <>
void Fence<kVk>::Swap(Fence& rhs) noexcept
{
	DeviceObject<kVk>::Swap(rhs);
	std::swap(myFence, rhs.myFence);
}

template <>
void Fence<kVk>::Wait(bool waitAll, uint64_t timeout) const
{
	ZoneScopedN("Fence::wait");

	VK_CHECK(vkWaitForFences(*GetDevice(), 1, &myFence, waitAll, timeout));
}

template <>
void Fence<kVk>::Wait(
	const std::shared_ptr<Device<kVk>>& device,
	std::span<FenceHandle<kVk>> fences,
	bool waitAll,
	uint64_t timeout)
{
	ZoneScopedN("Fence::wait");

	VK_CHECK(vkWaitForFences(*device, static_cast<uint32_t>(fences.size()), fences.data(), waitAll, timeout));
}
