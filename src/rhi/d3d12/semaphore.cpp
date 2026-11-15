// the D3D12 backend's semaphores and fences: D3D12 fences, a semaphore's value its timeline's (a binary one, which the
// swapchain alone uses, none: the queue orders its acquire and present), a fence's a value its D3D12 fence reaches.
// host waits by an event each thread keeps
#include <rhi/d3d12/backend.h>

#include <rhi/device.h>
#include <rhi/fence.h>
#include <rhi/rhi.h>
#include <rhi/semaphore.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace rhi
{

namespace semaphore
{

// this thread's event to wait on (made at its first wait, kept until the thread ends)
static HANDLE ThreadEvent()
{
	thread_local struct Event
	{
		HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		Event() = default;
		Event(const Event&) = delete;
		Event& operator=(const Event&) = delete;
		~Event() { CloseHandle(handle); }
	} event;
	ENSUREF(event.handle != nullptr, "Failed to create an event to wait on");
	return event.handle;
}

// timeouts in nanoseconds, as Vulkan's; WaitForSingleObject's in milliseconds (INFINITE: forever)
static DWORD Milliseconds(uint64_t timeout)
{
	if (timeout == ~0ULL)
		return INFINITE;
	return static_cast<DWORD>(std::min<uint64_t>(timeout / 1'000'000ULL + (timeout % 1'000'000ULL != 0 ? 1 : 0), INFINITE - 1));
}

// fences reaching their values, all or any, within timeout
static bool Wait(ID3D12Device* device, std::span<ID3D12Fence* const> fences, std::span<const uint64_t> values, bool all, uint64_t timeout)
{
	bool done = all;
	for (size_t fenceIt = 0; fenceIt < fences.size(); fenceIt++)
	{
		const bool reached = fences[fenceIt]->GetCompletedValue() >= values[fenceIt];
		done = all ? done && reached : done || reached;
	}
	if (done || fences.empty())
		return true;
	if (timeout == 0)
		return false;
	HANDLE event = ThreadEvent();
	ID3D12Device1* device1 = nullptr;
	d3d12::Check(device->QueryInterface(IID_PPV_ARGS(&device1)), "QueryInterface (ID3D12Device1)");
	d3d12::Check(
		device1->SetEventOnMultipleFenceCompletion(
			fences.data(),
			values.data(),
			static_cast<UINT>(fences.size()),
			all ? D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL : D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY,
			event),
		"SetEventOnMultipleFenceCompletion");
	device1->Release();
	return WaitForSingleObject(event, Milliseconds(timeout)) == WAIT_OBJECT_0;
}

} // namespace semaphore

IMPLEMENT_OBJECT_GETINSTANCE(Semaphore<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Semaphore<kD3D12>);

template <>
Semaphore<kD3D12>::Semaphore(CreateDescType&& desc, SemaphoreHandle<kD3D12>&& handle)
	: DeviceObject<Semaphore<kD3D12>>(std::forward<CreateDescType>(desc))
	, mySemaphore(std::forward<SemaphoreHandle<kD3D12>>(handle))
{}

template <>
Semaphore<kD3D12>::Semaphore(CreateDescType&& desc)
	: Semaphore(
		std::forward<CreateDescType>(desc),
		[&desc]() -> ID3D12Fence*
		{
			if (desc.type == SemaphoreType::kBinary)
				return nullptr;
			ID3D12Fence* fence = nullptr;
			d3d12::Check(desc.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
			d3d12::SetName(fence, desc.name);
			return fence;
		}())
{}

template <>
void Semaphore<kD3D12>::Swap(Semaphore& rhs) noexcept
{
	DeviceObject<Semaphore<kD3D12>>::Swap(rhs);
	std::swap(mySemaphore, rhs.mySemaphore);
}

template <>
Semaphore<kD3D12>::Semaphore(Semaphore<kD3D12>&& other) noexcept
{
	Swap(other);
}

template <>
Semaphore<kD3D12>& Semaphore<kD3D12>::operator=(Semaphore<kD3D12>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Semaphore<kD3D12>::~Semaphore()
{
	if (IsValid() && mySemaphore != nullptr)
		mySemaphore->Release();
}

template <>
uint64_t Semaphore<kD3D12>::GetValue() const
{
	return mySemaphore->GetCompletedValue();
}

template <>
bool Semaphore<kD3D12>::Wait(uint64_t timelineValue, uint64_t timeout) const
{
	ZoneScopedN("Semaphore::Wait");

	ID3D12Fence* fence = mySemaphore;
	return semaphore::Wait(GetDesc().device, std::span(&fence, 1), std::span(&timelineValue, 1), true, timeout);
}

template <>
bool Semaphore<kD3D12>::Wait(
	DeviceHandle<kD3D12> device,
	std::span<const SemaphoreHandle<kD3D12>> semaphores,
	std::span<const uint64_t> semaphoreValues,
	uint64_t timeout)
{
	ZoneScopedN("Semaphore::Wait");

	return semaphore::Wait(device, semaphores, semaphoreValues, true, timeout);
}

IMPLEMENT_OBJECT_GETINSTANCE(Fence<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Fence<kD3D12>);

// a fence: a D3D12 fence and the value it waits for (1: unsignaled until a queue signals it so)
template <>
Fence<kD3D12>::Fence(CreateDescType&& desc)
	: DeviceObject<Fence<kD3D12>>(std::forward<CreateDescType>(desc))
	, myFence(
		[this]
		{
			ID3D12Fence* fence = nullptr;
			d3d12::Check(GetDesc().device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
			return new d3d12::FenceObject{.fence = fence, .value = 1}; //NOLINT(cppcoreguidelines-owning-memory)
		}())
{}

template <>
bool Fence<kD3D12>::Wait(uint64_t timeout) const
{
	ZoneScopedN("Fence::Wait");

	return semaphore::Wait(GetDesc().device, std::span(&myFence->fence, 1), std::span(&myFence->value, 1), true, timeout);
}

template <>
bool Fence<kD3D12>::Wait(DeviceHandle<kD3D12> device, std::span<const FenceHandle<kD3D12>> fences, bool waitAll, uint64_t timeout)
{
	ZoneScopedN("Fence::Wait");

	std::vector<ID3D12Fence*> handles;
	std::vector<uint64_t> values;
	for (const auto* fence : fences)
	{
		handles.push_back(fence->fence);
		values.push_back(fence->value);
	}
	return semaphore::Wait(device, handles, values, waitAll, timeout);
}

template <>
void Fence<kD3D12>::Swap(Fence& rhs) noexcept
{
	DeviceObject<Fence<kD3D12>>::Swap(rhs);
	std::swap(myFence, rhs.myFence);
}

template <>
Fence<kD3D12>::Fence(Fence<kD3D12>&& other) noexcept
{
	Swap(other);
}

template <>
Fence<kD3D12>& Fence<kD3D12>::operator=(Fence<kD3D12>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Fence<kD3D12>::~Fence()
{
	if (!IsValid() || myFence == nullptr)
		return;
	d3d12::Release(myFence->fence);
	delete myFence; //NOLINT(cppcoreguidelines-owning-memory)
}

} // namespace rhi
