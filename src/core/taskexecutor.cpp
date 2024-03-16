#include "taskexecutor.h"

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

	myStopSource.store(true, std::memory_order_relaxed);
	mySignal.release(mySignal.max());

	processReadyQueue();

	for (auto& [thread, exception] : myThreads)
		thread.join();
}

// void TaskExecutor::addDependency(TaskHandle a, TaskHandle b)
// {
// 	handleToTaskRef(a).state()->adjacencies.emplace_back(b);
// 	handleToTaskRef(b).state()->latch.fetch_add(1, std::memory_order_relaxed);
// }

void TaskExecutor::submit(TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::submit");

	auto dependencies = handleToTaskRef(handle).state()->latch.fetch_add(1, std::memory_order_relaxed);// self
	if (dependencies == 0)
		myReadyQueue.enqueue(handle);

	while (myDeletionQueue.try_dequeue(handle))
	{
		if (handleToTaskRef(handle).state()->latch.load(std::memory_order_relaxed) == 0)
			myTasks.erase(handle);
	}

	mySignal.release();
}

void TaskExecutor::scheduleAdjacent(ProducerToken& readyProducerToken, TaskHandle task)
{
	ZoneScopedN("TaskExecutor::scheduleAdjacent");

	for (auto& adjacentAtomic : handleToTaskRef(task).state()->adjacencies)
	{
		auto adjacentPtr = adjacentAtomic.load(std::memory_order_relaxed);
		if (!adjacentPtr)
			continue;

		assertf(adjacentPtr->state(), "Task has no return state!");
		assertf(adjacentPtr->state()->latch, "Latch needs to have been constructed!");

		Task& adjacent = *adjacentPtr;

		auto counter = adjacent.state()->latch.fetch_sub(1, std::memory_order_relaxed) - 1;

		if (counter == 1)
		{
			myReadyQueue.enqueue(readyProducerToken, taskRefToHandle(adjacent));
			adjacentAtomic.store(nullptr, std::memory_order_release);
		}
	}
}

void TaskExecutor::scheduleAdjacent(TaskHandle task)
{
	ZoneScopedN("TaskExecutor::scheduleAdjacent");

	for (auto& adjacentAtomic : handleToTaskRef(task).state()->adjacencies)
	{
		auto adjacentPtr = adjacentAtomic.load(std::memory_order_relaxed);
		if (!adjacentPtr)
			continue;

		assertf(adjacentPtr->state(), "Task has no return state!");
		assertf(adjacentPtr->state()->latch, "Latch needs to have been constructed!");

		Task& adjacent = *adjacentPtr;

		auto counter = adjacent.state()->latch.fetch_sub(1, std::memory_order_relaxed) - 1;

		if (counter == 1)
		{
			myReadyQueue.enqueue(taskRefToHandle(adjacent));
			adjacentAtomic.store(nullptr, std::memory_order_release);
		}
	}
}

void TaskExecutor::processReadyQueue()
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	TaskHandle task;
	while (myReadyQueue.try_dequeue(task))
	{
		handleToTaskRef(task)();
		scheduleAdjacent(task);
		myDeletionQueue.enqueue(task);
	}
}

void TaskExecutor::processReadyQueue(ProducerToken& readyProducerToken, ConsumerToken& readyConsumerToken)
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	TaskHandle task;
	while (myReadyQueue.try_dequeue(readyConsumerToken, task))
	{
		handleToTaskRef(task)();
		scheduleAdjacent(readyProducerToken, task);
		myDeletionQueue.enqueue(task);
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
