#include "concurrency-utils.h"

Task::Task(Task&& other) noexcept
{
	*this = std::forward<Task>(other);
}

Task::~Task()
{
	if (myDeleteFcnPtr)
		myDeleteFcnPtr(myCallableMemory.data(), myArgsMemory.data());
}

Task& Task::operator=(Task&& other) noexcept
{
	if (myDeleteFcnPtr)
		myDeleteFcnPtr(myCallableMemory.data(), myArgsMemory.data());

	myState = std::exchange(other.myState, {});
	myInvokeFcnPtr = std::exchange(other.myInvokeFcnPtr, nullptr);
	myCopyFcnPtr = std::exchange(other.myCopyFcnPtr, nullptr);
	myDeleteFcnPtr = std::exchange(other.myDeleteFcnPtr, nullptr);

	if (myCopyFcnPtr)
		myCopyFcnPtr(
			myCallableMemory.data(),
			other.myCallableMemory.data(),
			myArgsMemory.data(),
			other.myArgsMemory.data());

	if (myDeleteFcnPtr)
		myDeleteFcnPtr(other.myCallableMemory.data(), other.myArgsMemory.data());

	return *this;
}

Task::operator bool() const noexcept
{
	return myInvokeFcnPtr && myCopyFcnPtr && myDeleteFcnPtr && myState;
}

bool Task::operator!() const noexcept
{
	return !static_cast<bool>(*this);
}

TaskGraph::TaskGraph(TaskGraph&& other) noexcept
{
	*this = std::forward<TaskGraph>(other);
}

TaskGraph& TaskGraph::operator=(TaskGraph&& other) noexcept
{
	myNodes = std::exchange(other.myNodes, {});

	return *this;
}

void TaskGraph::addDependency(TaskNodeHandle a, TaskNodeHandle b)
{
	auto& [taskA, adjacenciesA, dependenciesA] = myNodes[a];
	auto& [taskB, adjacenciesB, dependenciesB] = myNodes[b];

	adjacenciesA.push_back(b);
	++dependenciesB;
}

void TaskGraph::depthFirstSearch(
	size_t v,
	std::vector<bool>& visited,
	std::vector<size_t>& departure,
	std::vector<size_t>& sorted,
	size_t& time) const
{
	visited[v] = true;

	const auto& [task, adjacencies, dependencies] = myNodes[v];

	for (auto u : adjacencies)
		if (!visited[u])
			depthFirstSearch(u, visited, departure, sorted, time);

	sorted.emplace_back(v);
	departure[v] = time++;
};

void TaskGraph::finalize()
{
	ZoneScopedN("TaskGraph::finalize");

	auto n = myNodes.size();

	std::vector<bool> visited(n);
	std::vector<size_t> departure(n);
	std::vector<size_t> sorted;

	sorted.reserve(n);

	size_t time = 0;

	for (size_t i = 0; i < n; i++)
		if (!visited[i])
			depthFirstSearch(i, visited, departure, sorted, time);

	for (size_t u = 0; u < n; u++)
	{
		const auto& [task, adjacencies, dependencies] = myNodes[u];

		for (auto v : adjacencies)
			if (departure[u] <= departure[v])
				throw std::runtime_error("Graph is not a DAG!");
	}

	decltype(myNodes) sortedNodes(n);

	for (size_t i = 0; i < sorted.size(); i++)
		//sortedNodes[i] = std::move(myNodes[sorted[i]]);
		sortedNodes[i] = std::move(myNodes[myNodes.size() - sorted[i] - 1]);

	myNodes = std::move(sortedNodes);

	for (auto& [task, adjacencies, dependencies] : myNodes)
	{
		task.state()->latch.emplace(dependencies + 1); // deps + self

		for (auto adjacent : adjacencies)
			task.state()->adjacencies.emplace_back(&std::get<0>(myNodes[adjacent]));
	}
}

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

	//myStopSource.request_stop();
	myStopSource.store(true, std::memory_order_relaxed);
	//mySignal.notify_all();
	mySignal.release(mySignal.max());

	// workaround for the following problem in msvc implementation of jthread in <mutex>:
	// TRANSITION, ABI: Due to the unsynchronized delivery of notify_all by _Stoken,
	// this implementation cannot tolerate *this destruction while an interruptible wait
	// is outstanding. A future ABI should store both the internal CV and internal mutex
	// in the reference counted block to allow this.
	{
		ZoneScopedN("~TaskExecutor()::join");

		for (auto& [thread, exception] : myThreads)
			thread.join();
	}
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

void TaskExecutor::join()
{
	ZoneScopedN("TaskExecutor::join");

	processReadyQueue();
}

void TaskExecutor::scheduleAdjacent(moodycamel::ProducerToken& readyProducerToken, const Task& task)
{
	ZoneScopedN("TaskExecutor::scheduleAdjacent");

	std::vector<Task> next;

	for (auto& adjacent : task.state()->adjacencies)
	{
		auto adjacentPtr = adjacent.load();
		if (!adjacentPtr)
			continue;

		assertf(adjacentPtr->state(), "Task has no return state!");
		assertf(adjacentPtr->state()->latch, "Latch needs to have been constructed!");

		auto counter =
			adjacentPtr->state()->latch.value().fetch_sub(1, std::memory_order_release) - 1;

		if (counter == 1)
		{
			next.emplace_back(std::move(*adjacentPtr));
			adjacent.store(nullptr);
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
		auto adjacentPtr = adjacent.load();
		if (!adjacentPtr)
			continue;

		assertf(adjacentPtr->state(), "Task has no return state!");
		assertf(adjacentPtr->state()->latch, "Latch needs to have been constructed!");

		auto counter =
			adjacentPtr->state()->latch.value().fetch_sub(1, std::memory_order_release) - 1;

		if (counter == 1)
		{
			next.emplace_back(std::move(*adjacentPtr));
			adjacent.store(nullptr);
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
