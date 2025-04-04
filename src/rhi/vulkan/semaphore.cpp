#include "../semaphore.h"
#include "utils.h"

template <>
Semaphore<kVk>::Semaphore(
	const std::shared_ptr<Device<kVk>>& device,
	SemaphoreHandle<kVk>&& handle,
	SemaphoreCreateDesc<kVk>&& desc)
	: DeviceObject(
		  device,
		  {"_Semaphore"},
		  1,
		  VK_OBJECT_TYPE_SEMAPHORE,
		  reinterpret_cast<uint64_t*>(&handle),
		  uuids::uuid_system_generator{}())
	, mySemaphore(std::forward<SemaphoreHandle<kVk>>(handle))
	, myDesc(std::forward<SemaphoreCreateDesc<kVk>>(desc))
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

		SemaphoreHandle<kVk> handle;
		VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		createInfo.pNext = &typeCreateInfo;
		createInfo.flags = desc.flags;

		VK_ENSURE(vkCreateSemaphore(
			*device,
			&createInfo,
			&device->GetInstance()->GetHostAllocationCallbacks(),
			&handle));
		return handle;
	}(), std::forward<SemaphoreCreateDesc<kVk>>(desc))
{}

template <>
Semaphore<kVk>::Semaphore(Semaphore<kVk>&& other) noexcept
	: DeviceObject(std::forward<Semaphore<kVk>>(other))
	, mySemaphore(std::exchange(other.mySemaphore, {}))
	, myDesc(std::exchange(other.myDesc, {}))
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
	myDesc = std::exchange(other.myDesc, {});
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
	VK_ENSURE(vkGetSemaphoreCounterValue(*InternalGetDevice(), mySemaphore, &value));

	return value;
}

template <>
bool Semaphore<kVk>::Wait(uint64_t timelineValue, uint64_t timeout) const
{
	ZoneScopedN("Semaphore::wait");

	VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	waitInfo.flags = {};
	waitInfo.semaphoreCount = 1;
	waitInfo.pSemaphores = &mySemaphore;
	if (GetDesc().type == VK_SEMAPHORE_TYPE_TIMELINE)
		waitInfo.pValues = &timelineValue;

	auto result = vkWaitSemaphores(*InternalGetDevice(), &waitInfo, timeout);
	if (result == VK_TIMEOUT)
		return false;

	VK_ENSURE(result);

	return true;
}

template <>
void Semaphore<kVk>::Wait(
	DeviceHandle<kVk> device,
	std::span<const SemaphoreHandle<kVk>> semaphores,
	std::span<const uint64_t> semaphoreValues,
	uint64_t timeout)
{
	ZoneScopedN("Semaphore::wait");

	VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	waitInfo.flags = {};
	waitInfo.semaphoreCount = semaphores.size();
	waitInfo.pSemaphores = semaphores.data();
	waitInfo.pValues = semaphoreValues.data();

	VK_ENSURE(vkWaitSemaphores(device, &waitInfo, timeout));
}

