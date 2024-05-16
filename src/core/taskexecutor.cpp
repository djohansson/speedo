#include "taskexecutor.h"
#include "assert.h"//NOLINT(modernize-deprecated-headers)

#include <shared_mutex>

TaskExecutor::TaskExecutor(uint32_t threadCount)
	: mySignal(threadCount)
{
	ZoneScopedN("TaskExecutor()");

	ASSERTF(threadCount > 0, "Thread count must be nonzero");

	myThreads.reserve(threadCount);

	for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
		myThreads.emplace_back(std::thread(&TaskExecutor::InternalThreadMain, this, threadIt), nullptr);
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

void TaskExecutor::InternalSubmit(TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::InternalSubmit");

	Task& task = *Task::InternalHandleToPtr(handle);

	ASSERT(task.InternalState()->latch.load(std::memory_order_relaxed) == 1);

	myReadyQueue.enqueue(handle);

	if (!task.InternalState()->isContinuation)
		mySignal.release();
}

void TaskExecutor::InternalSubmit(ProducerToken& readyProducerToken, TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::InternalSubmit");

	Task& task = *Task::InternalHandleToPtr(handle);

	ASSERT(task.InternalState()->latch.load(std::memory_order_relaxed) == 1);

	myReadyQueue.enqueue(readyProducerToken, handle);

	if (!task.InternalState()->isContinuation)
		mySignal.release();
}

void TaskExecutor::InternalTryDelete(TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::InternalTryDelete");

	Task& task = *Task::InternalHandleToPtr(handle);

	if (task.InternalState()->latch.load(std::memory_order_relaxed) == 0)
	{
		std::destroy_at(&task);
	}
}

void TaskExecutor::InternalScheduleAdjacent(ProducerToken& readyProducerToken, Task& task)
{
	ZoneScopedN("TaskExecutor::InternalScheduleAdjacent");

	std::shared_lock lock(task.InternalState()->mutex);

	for (auto& adjacentHandle : task.InternalState()->adjacencies)
	{
		if (!adjacentHandle) // this is ok, means that another thread has claimed it
			continue;

		Task& adjacent = *Task::InternalHandleToPtr(adjacentHandle);

		ASSERTF(adjacent.InternalState(), "Task has no return state!");
		ASSERTF(adjacent.InternalState()->latch, "Latch needs to have been constructed!");

		if (adjacent.InternalState()->latch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
		{
			InternalSubmit(readyProducerToken, adjacentHandle);
			adjacentHandle = {};
		}
	}
}

void TaskExecutor::InternalScheduleAdjacent(Task& task)
{
	ZoneScopedN("TaskExecutor::InternalScheduleAdjacent");

	std::shared_lock lock(task.InternalState()->mutex);

	for (auto& adjacentHandle : task.InternalState()->adjacencies)
	{
		if (!adjacentHandle) // this is ok, means that another thread has claimed it
			continue;

		Task& adjacent = *Task::InternalHandleToPtr(adjacentHandle);

		ASSERTF(adjacent.InternalState(), "Task has no return state!");
		ASSERTF(adjacent.InternalState()->latch, "Latch needs to have been constructed!");

		if (adjacent.InternalState()->latch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
		{
			InternalSubmit(adjacentHandle);
			adjacentHandle = {};
		}
	}
}

void TaskExecutor::InternalProcessReadyQueue()
{
	ZoneScopedN("TaskExecutor::InternalProcessReadyQueue");

	TaskHandle handle;
	while (myReadyQueue.try_dequeue(handle))
	{
		InternalCall(handle);
		InternalPurgeDeletionQueue();
	}
}

void TaskExecutor::InternalProcessReadyQueue(ProducerToken& readyProducerToken, ConsumerToken& readyConsumerToken)
{
	ZoneScopedN("TaskExecutor::InternalProcessReadyQueue");

	TaskHandle handle;
	while (myReadyQueue.try_dequeue(handle))
	{
		InternalCall(readyProducerToken, handle);
		InternalPurgeDeletionQueue();
	}
}

void TaskExecutor::InternalPurgeDeletionQueue()
{
	ZoneScopedN("TaskExecutor::InternalPurgeDeletionQueue");

	TaskHandle handle;
	while (myDeletionQueue.try_dequeue(handle))
		InternalTryDelete(handle);
}

void TaskExecutor::InternalThreadMain(uint32_t threadId)
{
	try
	{
		ProducerToken readyProducerToken(myReadyQueue);
		ConsumerToken readyConsumerToken(myReadyQueue);

		while (!myStopSource.load(std::memory_order_acquire))
		{
			InternalProcessReadyQueue(readyProducerToken, readyConsumerToken);
			mySignal.acquire();
		}
	}
	catch (...)
	{
		std::get<1>(myThreads[threadId]) = std::current_exception();
	}
}
