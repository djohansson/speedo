#include "task.h"
#include "assert.h"//NOLINT(modernize-deprecated-headers)

static constexpr uint32_t kTaskPoolSize = (1 << 10); // todo: make this configurable
static MemoryPool<Task, kTaskPoolSize> gTaskPool;

Task::~Task()
{
	if (myDeleteFcnPtr != nullptr)
		myDeleteFcnPtr(myCallableMemory.data(), myArgsMemory.data());
}

Task::operator bool() const noexcept
{
	return (myInvokeFcnPtr != nullptr) && (myCopyFcnPtr != nullptr) &&
		   (myDeleteFcnPtr != nullptr) && myState;
}

void Task::AddDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation)
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

Task* Task::InternalHandleToPtr(TaskHandle handle) noexcept
{
	ASSERT(!!handle);
	ASSERT(handle.value < kTaskPoolSize);
	
	Task* ptr = gTaskPool.GetPointer(handle);

	ASSERT(ptr != nullptr);
	
	return ptr;
}

TaskHandle Task::InternalPtrToHandle(Task* ptr) noexcept
{
	ASSERT(ptr != nullptr);

	auto handle = gTaskPool.GetHandle(ptr);

	ASSERT(!!handle);
	ASSERT(handle.value < kTaskPoolSize);

	return handle;
}

TaskHandle Task::InternalAllocate() noexcept
{
	TaskHandle handle = gTaskPool.Allocate();

	ASSERT(!!handle);

	return handle;
}

void Task::InternalFree(TaskHandle handle) noexcept
{
	ASSERT(!!handle);
	ASSERT(handle.value < kTaskPoolSize);

	gTaskPool.Free(handle);
}
