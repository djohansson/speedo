#include "taskexecutor.h"

#include <cassert>
#include <shared_mutex>

MemoryPool<Task, TaskExecutor::TaskPoolSize> TaskExecutor::ourTaskPool{};

TaskExecutor::TaskExecutor(uint32_t threadCount)
	: mySignal(threadCount)
{
	ZoneScopedN("TaskExecutor()");

	assertf(threadCount > 0, "Thread count must be nonzero");

	myThreads.reserve(threadCount);

	for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
		myThreads.emplace_back(std::thread(&TaskExecutor::threadMain, this, threadIt), nullptr);
}

TaskExecutor::~TaskExecutor()
{
	ZoneScopedN("~TaskExecutor()");

	assert(myReadyQueue.size_approx() == 0);
	assert(myDeletionQueue.size_approx() == 0);

	myStopSource.store(true, std::memory_order_release);
	mySignal.release(myThreads.size());

	for (auto& [thread, exception] : myThreads)
		thread.detach();
}

void TaskExecutor::addDependency(TaskHandle a, TaskHandle b, bool isContinuation)
{
	ZoneScopedN("TaskExecutor::addDependency");

	assert(a != b);

	Task& aTask = *handleToTaskPtr(a);
	Task& bTask = *handleToTaskPtr(b);

	assertf(aTask.state(), "Task state is not valid!");
	assertf(bTask.state(), "Task state is not valid!");

	TaskState& aState = *aTask.state();
	TaskState& bState = *bTask.state();

	std::scoped_lock lock(aState.mutex, bState.mutex);

	if (isContinuation)
		bState.isContinuation = true;

	aState.adjacencies.emplace_back(b);
	bState.latch.fetch_add(1, std::memory_order_relaxed);
}

Task* TaskExecutor::handleToTaskPtr(TaskHandle handle) noexcept
{
	assert(handle);
	assert(handle.value < TaskPoolSize);
	
	Task* ptr = ourTaskPool.getPointer(handle);

	assert(ptr != nullptr);
	
	return ptr;
}

void TaskExecutor::internalSubmit(TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::internalSubmit");

	Task& task = *handleToTaskPtr(handle);

	assert(task.state()->latch.load(std::memory_order_relaxed) == 1);

	myReadyQueue.enqueue(handle);

	if (!task.state()->isContinuation)
		mySignal.release();
}

void TaskExecutor::internalSubmit(ProducerToken& readyProducerToken, TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::internalSubmit");

	Task& task = *handleToTaskPtr(handle);

	assert(task.state()->latch.load(std::memory_order_relaxed) == 1);

	myReadyQueue.enqueue(readyProducerToken, handle);

	if (!task.state()->isContinuation)
		mySignal.release();
}

void TaskExecutor::internalTryDelete(TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::internalTryDelete");

	Task& task = *handleToTaskPtr(handle);

	if (task.state()->latch.load(std::memory_order_relaxed) == 0)
	{
		task.~Task();
		ourTaskPool.free(handle);
	}
}

void TaskExecutor::scheduleAdjacent(ProducerToken& readyProducerToken, Task& task)
{
	ZoneScopedN("TaskExecutor::scheduleAdjacent");

	std::shared_lock lock(task.state()->mutex);

	for (auto& adjacentHandle : task.state()->adjacencies)
	{
		if (!adjacentHandle) // this is ok, means that another thread has claimed it
			continue;

		Task& adjacent = *handleToTaskPtr(adjacentHandle);

		assertf(adjacent.state(), "Task has no return state!");
		assertf(adjacent.state()->latch, "Latch needs to have been constructed!");

		if (adjacent.state()->latch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
		{
			internalSubmit(readyProducerToken, adjacentHandle);
			adjacentHandle = {};
		}
	}
}

void TaskExecutor::scheduleAdjacent(Task& task)
{
	ZoneScopedN("TaskExecutor::scheduleAdjacent");

	std::shared_lock lock(task.state()->mutex);

	for (auto& adjacentHandle : task.state()->adjacencies)
	{
		if (!adjacentHandle) // this is ok, means that another thread has claimed it
			continue;

		Task& adjacent = *handleToTaskPtr(adjacentHandle);

		assertf(adjacent.state(), "Task has no return state!");
		assertf(adjacent.state()->latch, "Latch needs to have been constructed!");

		if (adjacent.state()->latch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
		{
			internalSubmit(adjacentHandle);
			adjacentHandle = {};
		}
	}
}

void TaskExecutor::processReadyQueue()
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	TaskHandle handle;
	while (myReadyQueue.try_dequeue(handle))
	{
		internalCall(handle);
		purgeDeletionQueue();
	}
}

void TaskExecutor::processReadyQueue(ProducerToken& readyProducerToken, ConsumerToken& readyConsumerToken)
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	TaskHandle handle;
	while (myReadyQueue.try_dequeue(handle))
	{
		internalCall(readyProducerToken, handle);
		purgeDeletionQueue();
	}
}

void TaskExecutor::purgeDeletionQueue()
{
	ZoneScopedN("TaskExecutor::purgeDeletionQueue");

	TaskHandle handle;
	while (myDeletionQueue.try_dequeue(handle))
		internalTryDelete(handle);
}

void TaskExecutor::threadMain(uint32_t threadId)
{
	try
	{
		ProducerToken readyProducerToken(myReadyQueue);
		ConsumerToken readyConsumerToken(myReadyQueue);

		while (!myStopSource.load(std::memory_order_acquire))
		{
			processReadyQueue(readyProducerToken, readyConsumerToken);
			mySignal.acquire();
		}
	}
	catch (...)
	{
		std::get<1>(myThreads[threadId]) = std::current_exception();
	}
}
