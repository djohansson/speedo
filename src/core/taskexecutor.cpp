#include "taskexecutor.h"

MemoryPool<Task, TaskExecutor::TaskPoolSize> TaskExecutor::ourTaskPool{};
std::mutex TaskExecutor::ourTaskPoolMutex{};

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

	myStopSource.store(true, std::memory_order_relaxed);
	mySignal.release(mySignal.max());

	processReadyQueue();

	for (auto& [thread, exception] : myThreads)
		thread.join();
}

void TaskExecutor::addDependency(TaskHandle a, TaskHandle b)
{
	handleToTaskRef(a).state()->adjacencies.emplace_back(b);
	handleToTaskRef(b).state()->latch.fetch_add(1, std::memory_order_relaxed);
}

void TaskExecutor::internalCall(TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::internalCall");

	assert(handle);

	Task& task = handleToTaskRef(handle);

	assert(task.state()->latch.load(std::memory_order_relaxed) == 1);
	
	task();

	for (auto& adjacentAtomic : handleToTaskRef(handle).state()->adjacencies)
	{
		auto adjacentHandle = adjacentAtomic.load(std::memory_order_relaxed);
		
		assert(adjacentHandle);

		Task& adjacent = handleToTaskRef(adjacentHandle);

		assertf(adjacent.state(), "Task has no return state!");
		assertf(adjacent.state()->latch, "Latch needs to have been constructed!");

		if (adjacent.state()->latch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
		{
			adjacent();
			adjacentAtomic.store(nullptr, std::memory_order_release);
		}
	}
}

void TaskExecutor::internalSubmit(TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::internalSubmit");

	assert(handle);

	Task& task = handleToTaskRef(handle);

	assert(task.state()->latch.load(std::memory_order_relaxed) == 1);
	
	task();

	while (myDeletionQueue.try_dequeue(handle))
	{
		if (handleToTaskRef(handle).state()->latch.load(std::memory_order_relaxed) == 0)
		{
			ourTaskPoolMutex.lock();
			Task* taskPtr = &handleToTaskRef(handle);
			taskPtr->~Task();
			ourTaskPool.free(taskPtr);
			ourTaskPoolMutex.unlock();
		}
	}
}

void TaskExecutor::scheduleAdjacent(ProducerToken& readyProducerToken, Task& task)
{
	ZoneScopedN("TaskExecutor::scheduleAdjacent");

	for (auto& adjacentAtomic : task.state()->adjacencies)
	{
		auto adjacentHandle = adjacentAtomic.load(std::memory_order_relaxed);
		
		assert(adjacentHandle);

		Task& adjacent = handleToTaskRef(adjacentHandle);

		assertf(adjacent.state(), "Task has no return state!");
		assertf(adjacent.state()->latch, "Latch needs to have been constructed!");

		if (adjacent.state()->latch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
		{
			myReadyQueue.enqueue(readyProducerToken, taskRefToHandle(adjacent));
			adjacentAtomic.store(nullptr, std::memory_order_release);
		}
	}
}

void TaskExecutor::scheduleAdjacent(Task& task)
{
	ZoneScopedN("TaskExecutor::scheduleAdjacent");

	for (auto& adjacentAtomic : task.state()->adjacencies)
	{
		auto adjacentHandle = adjacentAtomic.load(std::memory_order_relaxed);
		
		assert(adjacentHandle);

		Task& adjacent = handleToTaskRef(adjacentHandle);

		assertf(adjacent.state(), "Task has no return state!");
		assertf(adjacent.state()->latch, "Latch needs to have been constructed!");

		if (adjacent.state()->latch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
		{
			myReadyQueue.enqueue(taskRefToHandle(adjacent));
			adjacentAtomic.store(nullptr, std::memory_order_release);
		}
	}
}

void TaskExecutor::processReadyQueue()
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	TaskHandle handle;
	while (myReadyQueue.try_dequeue(handle))
	{
		Task& task = handleToTaskRef(handle);
		task();
		scheduleAdjacent(task);
		myDeletionQueue.enqueue(handle);
	}
}

void TaskExecutor::processReadyQueue(ProducerToken& readyProducerToken, ConsumerToken& readyConsumerToken)
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	TaskHandle handle;
	while (myReadyQueue.try_dequeue(handle))
	{
		Task& task = handleToTaskRef(handle);
		task();
		scheduleAdjacent(readyProducerToken, task);
		myDeletionQueue.enqueue(handle);
	}
}

void TaskExecutor::threadMain(uint32_t threadId)
{
	try
	{
		ProducerToken readyProducerToken(myReadyQueue);
		ConsumerToken readyConsumerToken(myReadyQueue);

		while (!myStopSource.load(std::memory_order_relaxed))
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
