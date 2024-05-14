#include "task.h"
#include "assert.h"//NOLINT(modernize-deprecated-headers)

static constexpr uint32_t kTaskPoolSize = (1 << 10); // todo: make this configurable
static MemoryPool<Task, kTaskPoolSize> gTaskPool;

Task::Task(Task&& other) noexcept
{
	*this = std::forward<Task>(other);
}

Task::~Task()
{
	if (myDeleteFcnPtr != nullptr)
		myDeleteFcnPtr(myCallableMemory.data(), myArgsMemory.data());

	gTaskPool.Free(InternalPtrToHandle(this));
}

Task& Task::operator=(Task&& other) noexcept
{
	if (myDeleteFcnPtr != nullptr)
		myDeleteFcnPtr(myCallableMemory.data(), myArgsMemory.data());

	myState = std::exchange(other.myState, {});
	myInvokeFcnPtr = std::exchange(other.myInvokeFcnPtr, nullptr);
	myCopyFcnPtr = std::exchange(other.myCopyFcnPtr, nullptr);
	myDeleteFcnPtr = std::exchange(other.myDeleteFcnPtr, nullptr);

	if (myCopyFcnPtr != nullptr)
		myCopyFcnPtr(
			myCallableMemory.data(),
			other.myCallableMemory.data(),
			myArgsMemory.data(),
			other.myArgsMemory.data());

	if (myDeleteFcnPtr != nullptr)
		myDeleteFcnPtr(other.myCallableMemory.data(), other.myArgsMemory.data());

	return *this;
}

Task::operator bool() const noexcept
{
	return (myInvokeFcnPtr != nullptr) && (myCopyFcnPtr != nullptr) &&
		   (myDeleteFcnPtr != nullptr) && myState;
}

bool Task::operator!() const noexcept
{
	return !static_cast<bool>(*this);
}

void Task::AddDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation)
{
	ASSERT(aTaskHandle != bTaskHandle);

	Task& aTask = *InternalHandleToPtr(aTaskHandle);
	Task& bTask = *InternalHandleToPtr(bTaskHandle);

	ASSERTF(aTask.InternalState(), "Task state is not valid!");
	ASSERTF(bTask.InternalState(), "Task state is not valid!");

	TaskState& aState = *aTask.InternalState();
	TaskState& bState = *bTask.InternalState();

	std::scoped_lock lock(aState.mutex, bState.mutex);

	if (isContinuation)
		bState.isContinuation = true;

	aState.adjacencies.emplace_back(bTaskHandle);
	bState.latch.fetch_add(1, std::memory_order_relaxed);
}

Task* Task::InternalHandleToPtr(TaskHandle handle) noexcept
{
	ASSERT(handle != InvalidTaskHandle);
	ASSERT(handle.value < kTaskPoolSize);
	
	Task* ptr = gTaskPool.GetPointer(handle);

	ASSERT(ptr != nullptr);
	
	return ptr;
}

TaskHandle Task::InternalPtrToHandle(Task* ptr) noexcept
{
	ASSERT(ptr != nullptr);

	auto handle = gTaskPool.GetHandle(ptr);

	ASSERT(handle != InvalidTaskHandle);
	ASSERT(handle.value < kTaskPoolSize);

	return handle;
}

TaskHandle Task::InternalAllocate() noexcept
{
	TaskHandle handle = gTaskPool.Allocate();

	ASSERT(handle != InvalidTaskHandle);

	return handle;
}
