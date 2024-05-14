#include "../semaphore.h"
#include "utils.h"

template <>
Semaphore<Vk>::Semaphore(
	const std::shared_ptr<Device<Vk>>& device,
	SemaphoreHandle<Vk>&& semaphore)
	: DeviceObject(
		  device,
		  {"_Semaphore"},
		  1,
		  VK_OBJECT_TYPE_SEMAPHORE,
		  reinterpret_cast<uint64_t*>(&semaphore))
	, mySemaphore(std::forward<SemaphoreHandle<Vk>>(semaphore))
{}

template <>
Semaphore<Vk>::Semaphore(
	const std::shared_ptr<Device<Vk>>& device,
	SemaphoreCreateDesc<Vk>&& desc)
	: Semaphore(device, [&device, &desc]
	{
		VkSemaphoreTypeCreateInfo typeCreateInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
		typeCreateInfo.semaphoreType = desc.type;
		typeCreateInfo.initialValue = 0ULL;

		SemaphoreHandle<Vk> semaphore;
		VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		createInfo.pNext = &typeCreateInfo;
		createInfo.flags = desc.flags;

		VK_CHECK(vkCreateSemaphore(
			*device,
			&createInfo,
			&device->getInstance()->getHostAllocationCallbacks(),
			&semaphore));
		return semaphore;
	}())
{}

template <>
Semaphore<Vk>::Semaphore(Semaphore<Vk>&& other) noexcept
	: DeviceObject(std::forward<Semaphore<Vk>>(other))
	, mySemaphore(std::exchange(other.mySemaphore, {}))
{}

template <>
Semaphore<Vk>::~Semaphore()
{
	if (mySemaphore != nullptr)
		vkDestroySemaphore(
			*getDevice(),
			mySemaphore,
			&getDevice()->getInstance()->getHostAllocationCallbacks());
}

template <>
Semaphore<Vk>& Semaphore<Vk>::operator=(Semaphore<Vk>&& other) noexcept
{
	DeviceObject<Vk>::operator=(std::forward<Semaphore<Vk>>(other));
	mySemaphore = std::exchange(other.mySemaphore, {});
	return *this;
}

template <>
void Semaphore<Vk>::Swap(Semaphore& rhs) noexcept
{
	DeviceObject<Vk>::Swap(rhs);
	std::swap(mySemaphore, rhs.mySemaphore);
}

template <>
uint64_t Semaphore<Vk>::GetValue() const
{
	ZoneScopedN("Semaphore::getValue");

	uint64_t value;
	VK_CHECK(vkGetSemaphoreCounterValue(*getDevice(), mySemaphore, &value));

	return value;
}

template <>
void Semaphore<Vk>::Wait(uint64_t timelineValue, uint64_t timeout) const
{
	ZoneScopedN("Semaphore::wait");

	VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	waitInfo.flags = {};
	waitInfo.semaphoreCount = 1;
	waitInfo.pSemaphores = &mySemaphore;
	waitInfo.pValues = &timelineValue;

	VK_CHECK(vkWaitSemaphores(*getDevice(), &waitInfo, timeout));
}

template <>
void Semaphore<Vk>::Wait(
	const std::shared_ptr<Device<Vk>>& device,
	std::span<SemaphoreHandle<Vk>> semaphores,
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
