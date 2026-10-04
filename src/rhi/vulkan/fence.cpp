#include <rhi/fence.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Fence<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Fence<kVk>);

template <>
Fence<kVk>::Fence(
	CreateDescType&& desc,
	FenceHandle<kVk>&& fence)
	: DeviceObject<Fence<kVk>>(std::forward<CreateDescType>(desc))
	, myFence(std::forward<FenceHandle<kVk>>(fence))
{}

template <>
Fence<kVk>::Fence(CreateDescType&& desc)
	: Fence(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[this, &desc]
		{
			FenceHandle<kVk> fence;
			VkFenceCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			createInfo.flags = desc.flags;
			VK_CHECK(vkCreateFence(desc.device, &createInfo, &GetInstance().GetHostAllocationCallbacks(), &fence));
			Track(desc.device, VK_OBJECT_TYPE_FENCE, fence, GetDebugName(desc));
			return fence;
		}())
{}

template <>
void Fence<kVk>::Swap(Fence& rhs) noexcept
{
	DeviceObject<Fence<kVk>>::Swap(rhs);
	std::swap(myFence, rhs.myFence);
}

template <>
Fence<kVk>::Fence(Fence<kVk>&& other) noexcept
{
	Swap(other);
}

template <>
Fence<kVk>::~Fence()
{
	if (!IsValid())
		return;

	Untrack(VK_OBJECT_TYPE_FENCE, myFence);
	vkDestroyFence(
		GetDevice(),
		myFence,
		&GetInstance().GetHostAllocationCallbacks());
}

template <>
Fence<kVk>& Fence<kVk>::operator=(Fence<kVk>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
bool Fence<kVk>::Wait(uint64_t timeout) const
{
	ZoneScopedN("Fence::Wait");

	auto result = vkWaitForFences(GetDevice(), 1, &myFence, true, timeout);
	if (result == VK_SUCCESS)
		return true;

	VK_CHECK_RESULT(result, VK_TIMEOUT);

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

	VK_CHECK_RESULT(result, VK_TIMEOUT);

	return false;
}

} // namespace rhi
