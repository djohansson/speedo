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

void Task::operator()()
{
	ENSURE(*this);

	myFunction(*myState);

	auto counter = myState->latch.fetch_sub(1, std::memory_order_release) - 1;
	ENSUREF(counter == 0, "a task called with {} tasks it waits on unfinished", counter);
	myState->latch.notify_all();
}

void Task::AddDependency(Task& other, TaskHandle otherHandle, bool isContinuation) noexcept
{
	ENSURE(*this);
	ENSURE(other);
	ENSURE(this != &other);

	ENSUREF(myDependentCount < myDependents.size(), "a task with more than {} dependents", myDependents.size());
	myDependents[myDependentCount++] = otherHandle;
	other.myState->latch.fetch_add(1, std::memory_order_relaxed);
	other.myContinuation = isContinuation;
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
