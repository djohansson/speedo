#include "../fence.h"
#include "utils.h"

template <>
Fence<kVk>::Fence(
	const std::shared_ptr<Device<kVk>>& device,
	FenceHandle<kVk>&& fence,
	FenceCreateDesc<kVk>&& desc)
	: DeviceObject(
		  device,
		  [&desc]{ return DeviceObjectCreateDesc{ desc.name.data() }; }(),
		  1,
		  VK_OBJECT_TYPE_FENCE,
		  reinterpret_cast<uint64_t*>(&fence),
		  uuids::uuid_system_generator{}())
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
		VK_ENSURE(vkCreateFence(*device, &createInfo, &device->GetInstance()->GetHostAllocationCallbacks(), &fence));
		return fence;
	}(), std::forward<FenceCreateDesc<kVk>>(desc))
{}

template <>
Fence<kVk>::Fence(Fence<kVk>&& other) noexcept
	: DeviceObject(std::forward<Fence<kVk>>(other))
{
	std::swap(myFence, other.myFence);
}

template <>
Fence<kVk>::~Fence()
{
	if (myFence != nullptr)
		vkDestroyFence(*InternalGetDevice(), myFence, &InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
Fence<kVk>& Fence<kVk>::operator=(Fence<kVk>&& other) noexcept
{
	DeviceObject<kVk>::operator=(std::forward<Fence<kVk>>(other));
	std::swap(myFence, other.myFence);
	return *this;
}

template <>
void Fence<kVk>::Swap(Fence& rhs) noexcept
{
	DeviceObject<kVk>::Swap(rhs);
	std::swap(myFence, rhs.myFence);
}

template <>
bool Fence<kVk>::Wait(uint64_t timeout) const
{
	ZoneScopedN("Fence::Wait");

	auto result = vkWaitForFences(*InternalGetDevice(), 1, &myFence, true, timeout);
	if (result == VK_SUCCESS)
		return true;

	VK_ENSURE_RESULT(result, VK_TIMEOUT);

	return false;
}

template <>
bool Fence<kVk>::Wait(
	DeviceHandle<kVk> device,
	std::span<const FenceHandle<kVk>> fences,
	bool waitAll,
	uint64_t timeout)
{
	ZoneScopedN("Fence::Wait");

	auto result = vkWaitForFences(device, static_cast<uint32_t>(fences.size()), fences.data(), waitAll, timeout);
	if (result == VK_SUCCESS)
		return true;

	VK_ENSURE_RESULT(result, VK_TIMEOUT);

	return false;
}
