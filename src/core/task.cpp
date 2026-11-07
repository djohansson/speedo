#include "task.h"
#include "memorypool.h"

namespace core
{

namespace detail
{

using TaskPool = MemoryPool<Task, kTaskPoolChunkSize, kTaskPoolMaxChunks>;
static_assert(std::is_same_v<TaskPool::Handle, TaskHandle>);
static TaskPool gTaskPool;

Task* InternalHandleToPtr(TaskHandle handle) noexcept
{
	ENSURE(!!handle);

	return gTaskPool.GetPointer(handle);
}

TaskHandle InternalAllocate() noexcept
{
	TaskHandle handle = gTaskPool.Allocate();

	ENSURE(!!handle);

	return handle;
}

void InternalFree(TaskHandle handle) noexcept
{
	ENSURE(!!handle);

	gTaskPool.Free(handle);
}

} // namespace detail

Task::~Task() noexcept
{
	myDeleteFcn(myCallableMemory.data(), myArgsMemory.data());
	std::atomic_store(&InternalState(), {});
}

Task::operator bool() const noexcept
{
	return !!InternalState();
}

void Task::AddDependency(Task& other, TaskHandle otherHandle, bool isContinuation) noexcept
{
	ENSURE(*this);
	ENSURE(other);
	ENSURE(this != &other);

	TaskState& aState = *InternalState();
	TaskState& bState = *other.InternalState();

	ENSUREF(aState.adjacenciesCount < aState.adjacencies.size(), "a task with more than {} dependents",
		aState.adjacencies.size());
	aState.adjacencies[aState.adjacenciesCount++] = otherHandle;
	std::atomic_ref(bState.latch).fetch_add(1, std::memory_order_relaxed);
	bState.continuation = isContinuation;
}

void AddDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation) noexcept
{
	ENSURE(!!aTaskHandle);
	ENSURE(!!bTaskHandle);
	ENSURE(aTaskHandle != bTaskHandle);

	Task& aTask = *core::detail::InternalHandleToPtr(aTaskHandle);
	Task& bTask = *core::detail::InternalHandleToPtr(bTaskHandle);

	aTask.AddDependency(bTask, bTaskHandle, isContinuation);
}

TaskPoolStatistics GetTaskPoolStatistics() noexcept
{
	return {
		.used = core::detail::gTaskPool.Size(),
		.peak = core::detail::gTaskPool.Peak(),
		.capacity = core::detail::gTaskPool.Capacity(),
		.maxCapacity = core::detail::TaskPool::MaxCapacity()};
}

} // namespace core
