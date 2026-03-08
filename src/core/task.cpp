#include "task.h"
#include "memorypool.h"

namespace core
{
namespace detail
{

static MemoryPool<Task, kTaskPoolSize> gTaskPool;

Task* InternalHandleToPtr(TaskHandle handle) noexcept
{
	ENSURE(!!handle);
	ENSURE(handle.value < kTaskPoolSize);
	
	Task* ptr = gTaskPool.GetPointer(handle);

	ENSURE(ptr != nullptr);
	
	return ptr;
}

TaskHandle InternalPtrToHandle(Task* ptr) noexcept
{
	ENSURE(ptr != nullptr);

	auto handle = gTaskPool.GetHandle(ptr);

	ENSURE(!!handle);
	ENSURE(handle.value < kTaskPoolSize);

	return handle;
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
	ENSURE(handle.value < kTaskPoolSize);

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
	ENSURE(*this);
	ENSURE(other);
	ENSURE(this != &other);

	TaskState& aState = *InternalState();
	TaskState& bState = *other.InternalState();

	aState.adjacencies[aState.adjacenciesCount++] = core::detail::InternalPtrToHandle(&other);
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

	aTask.AddDependency(bTask, isContinuation);
}

