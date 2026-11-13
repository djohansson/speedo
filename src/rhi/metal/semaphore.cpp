// the Metal backend's semaphores and fences: shared events, a semaphore's value its timeline's (binary semaphores, which
// only the swapchain uses, come with it, step 6), a fence's a value its event reaches
#include <rhi/metal/backend.h>

#include <rhi/device.h>
#include <rhi/fence.h>
#include <rhi/rhi.h>
#include <rhi/semaphore.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Semaphore<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Semaphore<kMetal>);

template <>
Semaphore<kMetal>::Semaphore(CreateDescType&& desc, SemaphoreHandle<kMetal>&& handle)
	: DeviceObject<Semaphore<kMetal>>(std::forward<CreateDescType>(desc))
	, mySemaphore(std::forward<SemaphoreHandle<kMetal>>(handle))
{}

template <>
Semaphore<kMetal>::Semaphore(CreateDescType&& desc)
	: Semaphore(
		std::forward<CreateDescType>(desc),
		[&desc]
		{
			ENSUREF(desc.type == SemaphoreType::kTimeline, "the Metal backend's semaphores are timelines (binary ones come with the swapchain)");
			MTL::SharedEvent* event = desc.device->newSharedEvent();
			ENSUREF(event != nullptr, "Failed to create a Metal shared event");
			event->setLabel(NS::String::string(desc.name.c_str(), NS::UTF8StringEncoding));
			return event;
		}())
{}

template <>
void Semaphore<kMetal>::Swap(Semaphore& rhs) noexcept
{
	DeviceObject<Semaphore<kMetal>>::Swap(rhs);
	std::swap(mySemaphore, rhs.mySemaphore);
}

template <>
Semaphore<kMetal>::Semaphore(Semaphore<kMetal>&& other) noexcept
{
	Swap(other);
}

template <>
Semaphore<kMetal>& Semaphore<kMetal>::operator=(Semaphore<kMetal>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Semaphore<kMetal>::~Semaphore()
{
	if (IsValid())
		mySemaphore->release();
}

template <>
uint64_t Semaphore<kMetal>::GetValue() const
{
	return mySemaphore->signaledValue();
}

template <>
bool Semaphore<kMetal>::Wait(uint64_t timelineValue, uint64_t timeout) const
{
	ZoneScopedN("Semaphore::Wait");

	// timeouts in nanoseconds, as Vulkan's; waitUntilSignaledValue's in milliseconds (~0: forever)
	uint64_t milliseconds = timeout == ~0ULL ? ~0ULL : std::max<uint64_t>(timeout / 1'000'000ULL, 1);
	return mySemaphore->waitUntilSignaledValue(timelineValue, milliseconds);
}

template <>
bool Semaphore<kMetal>::Wait(
	DeviceHandle<kMetal> /*device*/,
	std::span<const SemaphoreHandle<kMetal>> semaphores,
	std::span<const uint64_t> semaphoreValues,
	uint64_t timeout)
{
	ZoneScopedN("Semaphore::Wait");

	uint64_t milliseconds = timeout == ~0ULL ? ~0ULL : std::max<uint64_t>(timeout / 1'000'000ULL, 1);
	for (size_t semaphoreIt = 0; semaphoreIt < semaphores.size(); semaphoreIt++)
		if (!semaphores[semaphoreIt]->waitUntilSignaledValue(semaphoreValues[semaphoreIt], milliseconds))
			return false;
	return true;
}

IMPLEMENT_OBJECT_GETINSTANCE(Fence<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Fence<kMetal>);

template <>
void Fence<kMetal>::Swap(Fence& rhs) noexcept
{
	DeviceObject<Fence<kMetal>>::Swap(rhs);
	std::swap(myFence, rhs.myFence);
}

template <>
Fence<kMetal>::Fence(Fence<kMetal>&& other) noexcept
{
	Swap(other);
}

template <>
Fence<kMetal>& Fence<kMetal>::operator=(Fence<kMetal>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Fence<kMetal>::~Fence()
{
	if (!IsValid() || myFence == nullptr)
		return;
	if (myFence->event != nullptr)
		myFence->event->release();
	delete myFence; //NOLINT(cppcoreguidelines-owning-memory)
}

} // namespace rhi
