#include "taskexecutor.h"

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
}

void TaskExecutor::submit(TaskGraph&& graph)
{
	ZoneScopedN("TaskExecutor::submit");

	removeFinishedGraphs();

	graph.finalize();

	for (auto& [task, adjacencies, dependencies] : graph.myNodes)
		if (dependencies == 0)
			myReadyQueue.enqueue(std::move(task));

	myWaitingQueue.enqueue(std::forward<TaskGraph>(graph));

	mySignal.release();
}

void TaskExecutor::scheduleAdjacent(moodycamel::ProducerToken& readyProducerToken, const Task& task)
{
	ZoneScopedN("TaskExecutor::scheduleAdjacent");

	std::vector<Task> next;

	for (auto& adjacent : task.state()->adjacencies)
	{
		auto adjacentPtr = adjacent.load(std::memory_order_relaxed);
		if (!adjacentPtr)
			continue;

		assertf(adjacentPtr->state(), "Task has no return state!");
		assertf(adjacentPtr->state()->latch, "Latch needs to have been constructed!");

		auto counter = adjacentPtr->state()->latch.value().fetch_sub(1, std::memory_order_relaxed) - 1;

		if (counter == 1)
		{
			myReadyQueue.enqueue(readyProducerToken, std::move(*adjacentPtr));
			adjacent.store(nullptr, std::memory_order_release);
		}
	}

	if (!next.empty())
	{
		myReadyQueue.enqueue_bulk(
			readyProducerToken, std::make_move_iterator(next.begin()), next.size());
		
		mySignal.release();
	}
}

void TaskExecutor::scheduleAdjacent(const Task& task)
{
	ZoneScopedN("TaskExecutor::scheduleAdjacent");

	std::vector<Task> next;

	for (auto& adjacent : task.state()->adjacencies)
	{
		auto adjacentPtr = adjacent.load(std::memory_order_relaxed);
		if (!adjacentPtr)
			continue;

		assertf(adjacentPtr->state(), "Task has no return state!");
		assertf(adjacentPtr->state()->latch, "Latch needs to have been constructed!");

		auto counter = adjacentPtr->state()->latch.value().fetch_sub(1, std::memory_order_relaxed) - 1;

		if (counter == 1)
		{
			myReadyQueue.enqueue(std::move(*adjacentPtr));
			adjacent.store(nullptr, std::memory_order_release);
		}
	}

	if (!next.empty())
	{
		myReadyQueue.enqueue_bulk(std::make_move_iterator(next.begin()), next.size());
		
		mySignal.release();
	}
}

void TaskExecutor::processReadyQueue()
{
	Task task;
	while (myReadyQueue.try_dequeue(task))
	{
		task();

		scheduleAdjacent(task);
	}
}

void TaskExecutor::processReadyQueue(
	moodycamel::ProducerToken& readyProducerToken, moodycamel::ConsumerToken& readyConsumerToken)
{
	Task task;
	while (myReadyQueue.try_dequeue(readyConsumerToken, task))
	{
		task();

		scheduleAdjacent(readyProducerToken, task);
	}
}

void TaskExecutor::removeFinishedGraphs()
{
	ZoneScopedN("TaskExecutor::removeFinishedGraphs");

	std::vector<TaskGraph> notFinished;
	TaskGraph graph;
	while (myWaitingQueue.try_dequeue(graph))
	{
		for (auto& [task, adjacencies, dependencies] : graph.myNodes)
		{
			if (task)
			{
				notFinished.emplace_back(std::move(graph));
				
				break;
			}
		}
	}

	if (!notFinished.empty())
	{
		myWaitingQueue.enqueue_bulk(
			std::make_move_iterator(notFinished.begin()), notFinished.size());
	}
}

void TaskExecutor::threadMain(uint32_t threadId)
{
	try
	{
		moodycamel::ProducerToken readyProducerToken(myReadyQueue);
		moodycamel::ConsumerToken readyConsumerToken(myReadyQueue);

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
