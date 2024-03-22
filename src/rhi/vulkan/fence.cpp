#include "../fence.h"
#include "utils.h"

template <>
Fence<Vk>::Fence(
	const std::shared_ptr<Device<Vk>>& device,
	FenceHandle<Vk>&& fence)
	: DeviceObject(
		  device,
		  {"_Fence"},
		  1,
		  VK_OBJECT_TYPE_FENCE,
		  reinterpret_cast<uint64_t*>(&fence))
	, myFence(std::forward<FenceHandle<Vk>>(fence))
{}

template <>
Fence<Vk>::Fence(
	const std::shared_ptr<Device<Vk>>& device,
	FenceCreateDesc<Vk>&& desc)
	: Fence(device, [&device, &desc]
	{
		FenceHandle<Vk> fence;
		VkFenceCreateInfo createInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		createInfo.flags = desc.flags;
		VK_CHECK(vkCreateFence(*device, &createInfo, &device->getInstance()->getHostAllocationCallbacks(), &fence));
		return fence;
	}())
{}

template <>
Fence<Vk>::Fence(Fence<Vk>&& other) noexcept
	: DeviceObject(std::forward<Fence<Vk>>(other))
	, myFence(std::exchange(other.myFence, {}))
{}

template <>
Fence<Vk>::~Fence()
{
	if (myFence)
		vkDestroyFence(*getDevice(), myFence, &getDevice()->getInstance()->getHostAllocationCallbacks());
}

template <>
Fence<Vk>& Fence<Vk>::operator=(Fence<Vk>&& other) noexcept
{
	DeviceObject<Vk>::operator=(std::forward<Fence<Vk>>(other));
	myFence = std::exchange(other.myFence, {});
	return *this;
}

template <>
void Fence<Vk>::swap(Fence& rhs) noexcept
{
	DeviceObject<Vk>::swap(rhs);
	std::swap(myFence, rhs.myFence);
}

template <>
void Fence<Vk>::wait(bool waitAll, uint64_t timeout) const
{
	ZoneScopedN("Fence::wait");

	VK_CHECK(vkWaitForFences(*getDevice(), 1, &myFence, waitAll, timeout));
}

template <>
void Fence<Vk>::wait(const std::shared_ptr<Device<Vk>>& device, std::span<FenceHandle<Vk>> fences, bool waitAll, uint64_t timeout)
{
	ZoneScopedN("Fence::wait");

	VK_CHECK(vkWaitForFences(*device, static_cast<uint32_t>(fences.size()), fences.data(), waitAll, timeout));
}
