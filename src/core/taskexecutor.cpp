#include "taskexecutor.h"
#include "assert.h"//NOLINT(modernize-deprecated-headers)

#include <shared_mutex>

MemoryPool<Task, TaskExecutor::kTaskPoolSize> TaskExecutor::gTaskPool{};

TaskExecutor::TaskExecutor(uint32_t threadCount)
	: mySignal(threadCount)
{
	ZoneScopedN("TaskExecutor()");

	ASSERTF(threadCount > 0, "Thread count must be nonzero");

	myThreads.reserve(threadCount);

	for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
		myThreads.emplace_back(std::thread(&TaskExecutor::ThreadMain, this, threadIt), nullptr);
}

TaskExecutor::~TaskExecutor()
{
	ZoneScopedN("~TaskExecutor()");

	ASSERT(myReadyQueue.size_approx() == 0);
	ASSERT(myDeletionQueue.size_approx() == 0);

	myStopSource.store(true, std::memory_order_release);
	mySignal.release(static_cast<ptrdiff_t>(myThreads.size()));

	for (auto& [thread, exception] : myThreads)
		thread.detach();
}

void TaskExecutor::AddDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation)
{
	ZoneScopedN("TaskExecutor::AddDependency");

	ASSERT(aTaskHandle != bTaskHandle);

	Task& aTask = *HandleToTaskPtr(aTaskHandle);
	Task& bTask = *HandleToTaskPtr(bTaskHandle);

	ASSERTF(aTask.State(), "Task state is not valid!");
	ASSERTF(bTask.State(), "Task state is not valid!");

	TaskState& aState = *aTask.State();
	TaskState& bState = *bTask.State();

	std::scoped_lock lock(aState.mutex, bState.mutex);

	if (isContinuation)
		bState.isContinuation = true;

	aState.adjacencies.emplace_back(bTaskHandle);
	bState.latch.fetch_add(1, std::memory_order_relaxed);
}

Task* TaskExecutor::HandleToTaskPtr(TaskHandle handle) noexcept
{
	ASSERT(handle);
	ASSERT(handle.value < kTaskPoolSize);
	
	Task* ptr = gTaskPool.GetPointer(handle);

	ASSERT(ptr != nullptr);
	
	return ptr;
}

void TaskExecutor::InternalSubmit(TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::internalSubmit");

	Task& task = *HandleToTaskPtr(handle);

	ASSERT(task.State()->latch.load(std::memory_order_relaxed) == 1);

	myReadyQueue.enqueue(handle);

	if (!task.State()->isContinuation)
		mySignal.release();
}

void TaskExecutor::InternalSubmit(ProducerToken& readyProducerToken, TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::internalSubmit");

	Task& task = *HandleToTaskPtr(handle);

	ASSERT(task.State()->latch.load(std::memory_order_relaxed) == 1);

	myReadyQueue.enqueue(readyProducerToken, handle);

	if (!task.State()->isContinuation)
		mySignal.release();
}

void TaskExecutor::InternalTryDelete(TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::internalTryDelete");

	Task& task = *HandleToTaskPtr(handle);

	if (task.State()->latch.load(std::memory_order_relaxed) == 0)
	{
		task.~Task();
		gTaskPool.Free(handle);
	}
}

void TaskExecutor::ScheduleAdjacent(ProducerToken& readyProducerToken, Task& task)
{
	ZoneScopedN("TaskExecutor::scheduleAdjacent");

	std::shared_lock lock(task.State()->mutex);

	for (auto& adjacentHandle : task.State()->adjacencies)
	{
		if (!adjacentHandle) // this is ok, means that another thread has claimed it
			continue;

		Task& adjacent = *HandleToTaskPtr(adjacentHandle);

		ASSERTF(adjacent.State(), "Task has no return state!");
		ASSERTF(adjacent.State()->latch, "Latch needs to have been constructed!");

		if (adjacent.State()->latch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
		{
			InternalSubmit(readyProducerToken, adjacentHandle);
			adjacentHandle = {};
		}
	}
}

void TaskExecutor::ScheduleAdjacent(Task& task)
{
	ZoneScopedN("TaskExecutor::scheduleAdjacent");

	std::shared_lock lock(task.State()->mutex);

	for (auto& adjacentHandle : task.State()->adjacencies)
	{
		if (!adjacentHandle) // this is ok, means that another thread has claimed it
			continue;

		Task& adjacent = *HandleToTaskPtr(adjacentHandle);

		ASSERTF(adjacent.State(), "Task has no return state!");
		ASSERTF(adjacent.State()->latch, "Latch needs to have been constructed!");

		if (adjacent.State()->latch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
		{
			InternalSubmit(adjacentHandle);
			adjacentHandle = {};
		}
	}
}

void TaskExecutor::ProcessReadyQueue()
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	TaskHandle handle;
	while (myReadyQueue.try_dequeue(handle))
	{
		InternalCall(handle);
		PurgeDeletionQueue();
	}
}

void TaskExecutor::ProcessReadyQueue(ProducerToken& readyProducerToken, ConsumerToken& readyConsumerToken)
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	TaskHandle handle;
	while (myReadyQueue.try_dequeue(handle))
	{
		InternalCall(readyProducerToken, handle);
		PurgeDeletionQueue();
	}
}

void TaskExecutor::PurgeDeletionQueue()
{
	ZoneScopedN("TaskExecutor::purgeDeletionQueue");

	TaskHandle handle;
	while (myDeletionQueue.try_dequeue(handle))
		InternalTryDelete(handle);
}

void TaskExecutor::ThreadMain(uint32_t threadId)
{
	try
	{
		ProducerToken readyProducerToken(myReadyQueue);
		ConsumerToken readyConsumerToken(myReadyQueue);

		while (!myStopSource.load(std::memory_order_acquire))
		{
			ProcessReadyQueue(readyProducerToken, readyConsumerToken);
			mySignal.acquire();
		}
	}
	catch (...)
	{
		std::get<1>(myThreads[threadId]) = std::current_exception();
	}
}
