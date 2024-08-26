#include "task.h"

#include <mutex>

static TaskPool gTaskPool;

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

Task::~Task()
{
	myDeleteFcn(myCallableMemory.data(), myArgsMemory.data());
}

Task::operator bool() const noexcept
{
	return !!InternalState();
}

void AddDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation)
{
	ASSERT(!!aTaskHandle);
	ASSERT(!!bTaskHandle);
	ASSERT(aTaskHandle != bTaskHandle);

	Task& aTask = *InternalHandleToPtr(aTaskHandle);
	Task& bTask = *InternalHandleToPtr(bTaskHandle);

	ASSERTF(aTask.InternalState(), "Task state is not valid!");
	ASSERTF(bTask.InternalState(), "Task state is not valid!");

	TaskState& aState = *aTask.InternalState();
	TaskState& bState = *bTask.InternalState();

	std::scoped_lock lock(aState.mutex, bState.mutex);

	aState.adjacencies.emplace_back(bTaskHandle);
	bState.latch.fetch_add(1, std::memory_order_relaxed);
	bState.continuation = isContinuation;
}

