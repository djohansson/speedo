#include "task.h"
#include "memorypool.h"

#include <mutex>

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

Task::~Task()
{
	myDeleteFcn(myCallableMemory.data(), myArgsMemory.data());
}

Task::operator bool() const noexcept
{
	return !!InternalState();
}

void Task::AddDependency(Task& other, bool isContinuation)
{
	ASSERT(*this);
	ASSERT(other);
	ASSERT(this != &other);

	TaskState& aState = *InternalState();
	TaskState& bState = *other.InternalState();

	std::scoped_lock lock(aState.mutex, bState.mutex);

	aState.adjacencies.emplace_back(InternalPtrToHandle(&other));
	bState.latch.fetch_add(1, std::memory_order_relaxed);
	bState.continuation = isContinuation;
}

void AddDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation)
{
	ASSERT(!!aTaskHandle);
	ASSERT(!!bTaskHandle);
	ASSERT(aTaskHandle != bTaskHandle);

	Task& aTask = *InternalHandleToPtr(aTaskHandle);
	Task& bTask = *InternalHandleToPtr(bTaskHandle);

	aTask.AddDependency(bTask, isContinuation);
}

