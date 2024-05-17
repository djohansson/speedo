#include "../semaphore.h"
#include "utils.h"

template <>
Semaphore<kVk>::Semaphore(
	const std::shared_ptr<Device<kVk>>& device,
	SemaphoreHandle<kVk>&& semaphore)
	: DeviceObject(
		  device,
		  {"_Semaphore"},
		  1,
		  VK_OBJECT_TYPE_SEMAPHORE,
		  reinterpret_cast<uint64_t*>(&semaphore))
	, mySemaphore(std::forward<SemaphoreHandle<kVk>>(semaphore))
{}

template <>
Semaphore<kVk>::Semaphore(
	const std::shared_ptr<Device<kVk>>& device,
	SemaphoreCreateDesc<kVk>&& desc)
	: Semaphore(device, [&device, &desc]
	{
		VkSemaphoreTypeCreateInfo typeCreateInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
		typeCreateInfo.semaphoreType = desc.type;
		typeCreateInfo.initialValue = 0ULL;

		SemaphoreHandle<kVk> semaphore;
		VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		createInfo.pNext = &typeCreateInfo;
		createInfo.flags = desc.flags;

		VK_CHECK(vkCreateSemaphore(
			*device,
			&createInfo,
			&device->GetInstance()->GetHostAllocationCallbacks(),
			&semaphore));
		return semaphore;
	}())
{}

template <>
Semaphore<kVk>::Semaphore(Semaphore<kVk>&& other) noexcept
	: DeviceObject(std::forward<Semaphore<kVk>>(other))
	, mySemaphore(std::exchange(other.mySemaphore, {}))
{}

template <>
Semaphore<kVk>::~Semaphore()
{
	if (mySemaphore != nullptr)
		vkDestroySemaphore(
			*InternalGetDevice(),
			mySemaphore,
			&InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
Semaphore<kVk>& Semaphore<kVk>::operator=(Semaphore<kVk>&& other) noexcept
{
	DeviceObject<kVk>::operator=(std::forward<Semaphore<kVk>>(other));
	mySemaphore = std::exchange(other.mySemaphore, {});
	return *this;
}

template <>
void Semaphore<kVk>::Swap(Semaphore& rhs) noexcept
{
	DeviceObject<kVk>::Swap(rhs);
	std::swap(mySemaphore, rhs.mySemaphore);
}

template <>
uint64_t Semaphore<kVk>::GetValue() const
{
	ZoneScopedN("Semaphore::getValue");

	uint64_t value;
	VK_CHECK(vkGetSemaphoreCounterValue(*InternalGetDevice(), mySemaphore, &value));

	return value;
}

template <>
void Semaphore<kVk>::Wait(uint64_t timelineValue, uint64_t timeout) const
{
	ZoneScopedN("Semaphore::wait");

	VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	waitInfo.flags = {};
	waitInfo.semaphoreCount = 1;
	waitInfo.pSemaphores = &mySemaphore;
	waitInfo.pValues = &timelineValue;

	VK_CHECK(vkWaitSemaphores(*InternalGetDevice(), &waitInfo, timeout));
}

template <>
void Semaphore<kVk>::Wait(
	const std::shared_ptr<Device<kVk>>& device,
	std::span<SemaphoreHandle<kVk>> semaphores,
	std::span<uint64_t> timelineValues,
	uint64_t timeout)
{
	ZoneScopedN("Semaphore::wait");

	VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	waitInfo.flags = {};
	waitInfo.semaphoreCount = static_cast<uint32_t>(semaphores.size());
	waitInfo.pSemaphores = semaphores.data();
	waitInfo.pValues = timelineValues.data();

	VK_CHECK(vkWaitSemaphores(*device, &waitInfo, timeout));
}
