#include <rhi/semaphore.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Semaphore<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Semaphore<kVk>);

template <>
Semaphore<kVk>::Semaphore(
	CreateDescType&& desc,
	SemaphoreHandle<kVk>&& handle)
	: DeviceObject<Semaphore<kVk>>(std::forward<CreateDescType>(desc))
	, mySemaphore(std::forward<SemaphoreHandle<kVk>>(handle))
{}

template <>
Semaphore<kVk>::Semaphore(CreateDescType&& desc)
	: Semaphore(
		std::forward<CreateDescType>(desc),
		[this, &desc]
		{
			VkSemaphoreTypeCreateInfo typeCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
			typeCreateInfo.semaphoreType = desc.type;
			typeCreateInfo.initialValue = 0ULL;

			SemaphoreHandle<kVk> handle;
			VkSemaphoreCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			createInfo.pNext = &typeCreateInfo;
			createInfo.flags = desc.flags;

			VK_CHECK(vkCreateSemaphore(
				desc.device,
				&createInfo,
				&GetInstance().GetHostAllocationCallbacks(),
				&handle));

			Track(desc.device, VK_OBJECT_TYPE_SEMAPHORE, handle, GetDebugName(desc));

			return handle;
		}())
{}

template <>
void Semaphore<kVk>::Swap(Semaphore& rhs) noexcept
{
	DeviceObject<Semaphore<kVk>>::Swap(rhs);
	std::swap(mySemaphore, rhs.mySemaphore);
}

template <>
Semaphore<kVk>::Semaphore(Semaphore<kVk>&& other) noexcept
{
	Swap(other);
}

template <>
Semaphore<kVk>::~Semaphore()
{
	if (!IsValid())
		return;

	Untrack(VK_OBJECT_TYPE_SEMAPHORE, mySemaphore);
	vkDestroySemaphore(
		GetDevice(),
		mySemaphore,
		&GetInstance().GetHostAllocationCallbacks());
}

template <>
Semaphore<kVk>& Semaphore<kVk>::operator=(Semaphore<kVk>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
uint64_t Semaphore<kVk>::GetValue() const
{
	ZoneScopedN("Semaphore::GetValue");

	uint64_t value;
	VK_CHECK(vkGetSemaphoreCounterValue(GetDevice(), mySemaphore, &value));

	return value;
}

template <>
bool Semaphore<kVk>::Wait(uint64_t timelineValue, uint64_t timeout) const
{
	ZoneScopedN("Semaphore::Wait");

	VkSemaphoreWaitInfo waitInfo{.sType=VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	waitInfo.flags = {};
	waitInfo.semaphoreCount = 1;
	waitInfo.pSemaphores = &mySemaphore;
	if (GetDesc().type == VK_SEMAPHORE_TYPE_TIMELINE)
		waitInfo.pValues = &timelineValue;

	auto result = vkWaitSemaphores(GetDevice(), &waitInfo, timeout);
	if (result == VK_SUCCESS)
		return true;

	VK_CHECK_RESULT(result, VK_TIMEOUT);

	return false;
}

template <>
bool Semaphore<kVk>::Wait(
	DeviceHandle<kVk> device,
	std::span<const SemaphoreHandle<kVk>> semaphores,
	std::span<const uint64_t> semaphoreValues,
	uint64_t timeout)
{
	ZoneScopedN("Semaphore::Wait");

	VkSemaphoreWaitInfo waitInfo{.sType=VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	waitInfo.flags = {};
	waitInfo.semaphoreCount = semaphores.size();
	waitInfo.pSemaphores = semaphores.data();
	waitInfo.pValues = semaphoreValues.data();

	auto result = vkWaitSemaphores(device, &waitInfo, timeout);
	if (result == VK_SUCCESS)
		return true;

	VK_CHECK_RESULT(result, VK_TIMEOUT);

	return false;
}

} // namespace rhi
