#include "task.h"
#include "memorypool.h"

#include <mutex>

namespace core
{
namespace detail
{

static MemoryPool<Task, kTaskPoolSize> gTaskPool;

Task* InternalHandleToPtr(TaskHandle handle) noexcept
{
	ASSERT(!!handle);
	ASSERT(handle.value < kTaskPoolSize);
	
	Task* ptr = gTaskPool.GetPointer(handle);

	ASSERT(ptr != nullptr);
	
	return ptr;
}

TaskHandle InternalPtrToHandle(Task* ptr) noexcept
{
	ASSERT(ptr != nullptr);

	auto handle = gTaskPool.GetHandle(ptr);

	ASSERT(!!handle);
	ASSERT(handle.value < kTaskPoolSize);

	return handle;
}

TaskHandle InternalAllocate() noexcept
{
	TaskHandle handle = gTaskPool.Allocate();

	ASSERT(!!handle);

	return handle;
}

void InternalFree(TaskHandle handle) noexcept
{
	ASSERT(!!handle);
	ASSERT(handle.value < kTaskPoolSize);

	gTaskPool.Free(handle);
}

} // namespace detail

} // namespace core

Task::~Task() noexcept
{
	myDeleteFcn(myCallableMemory.data(), myArgsMemory.data());
	std::atomic_store(&InternalState(), {});
}

Task::operator bool() const noexcept
{
	return !!InternalState();
}

void Task::AddDependency(Task& other, bool isContinuation) noexcept
{
	ASSERT(*this);
	ASSERT(other);
	ASSERT(this != &other);

	TaskState& aState = *InternalState();
	TaskState& bState = *other.InternalState();

	aState.adjacencies[aState.adjacenciesCount++] = core::detail::InternalPtrToHandle(&other);
	std::atomic_ref(bState.latch).fetch_add(1, std::memory_order_relaxed);
	bState.continuation = isContinuation;
}

void AddDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation) noexcept
{
	ASSERT(!!aTaskHandle);
	ASSERT(!!bTaskHandle);
	ASSERT(aTaskHandle != bTaskHandle);

	Task& aTask = *core::detail::InternalHandleToPtr(aTaskHandle);
	Task& bTask = *core::detail::InternalHandleToPtr(bTaskHandle);

	aTask.AddDependency(bTask, isContinuation);
}

