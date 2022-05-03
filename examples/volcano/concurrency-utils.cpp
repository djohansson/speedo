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

void TaskGraph::addDependency(TaskNodeHandle a, TaskNodeHandle b)
{
	// TODO:
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

void TaskGraph::initialize()
{
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
		sortedNodes[i] = std::move(myNodes[sorted[i]]);

	myNodes = std::move(sortedNodes);

	for (auto& [task, adjacencies, dependencies] : myNodes)
	{
		task.state()->latch.emplace(dependencies + 1); // deps + self
		task.state()->adjacencies.reserve(adjacencies.size());

		for (auto adjacent : adjacencies)
			task.state()->adjacencies.push_back(&std::get<0>(myNodes[adjacent]));
	}
}

TaskExecutor::TaskExecutor(uint32_t threadCount)
	: mySignal(threadCount)
{
	ZoneScopedN("TaskExecutor()");

	assertf(threadCount > 0, "Thread count must be nonzero");

	myThreads.reserve(threadCount);
	for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
		myThreads.emplace_back(&TaskExecutor::threadMain, this);
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

		for (auto& thread : myThreads)
			thread.join();
	}
}

void TaskExecutor::submit(TaskGraph&& graph)
{
	ZoneScopedN("TaskExecutor::submit");

	graph.initialize();

	for (auto& [task, adjacencies, dependencies] : graph.myNodes)
		if (dependencies == 0)
			myReadyQueue.enqueue(std::move(task));

	//myWaitingQueue.enqueue(std::forward<TaskGraph>(graph));

	mySignal.release();
}

void TaskExecutor::join()
{
	ZoneScopedN("TaskExecutor::join");

	processQueues();
}

void TaskExecutor::scheduleAdjacent(const Task& task)
{
	for (auto adjacent : task.state()->adjacencies)
	{
		assertf(adjacent->state(), "Task has no return state!");
		assertf(adjacent->state()->latch, "Latch needs to have been constructed!");

		auto counter = adjacent->state()->latch.value().fetch_sub(1, std::memory_order_release) - 1;

		if (counter == 1)
			myReadyQueue.enqueue(std::move(*adjacent));
	}
}

void TaskExecutor::processQueues()
{
	Task task;
	while (myReadyQueue.try_dequeue(task))
	{
		task();
		scheduleAdjacent(task);
	}
}

void TaskExecutor::processQueues(
	moodycamel::ProducerToken& readyProducerToken,
	moodycamel::ConsumerToken& readyConsumerToken,
	moodycamel::ProducerToken& waitingProducerToken,
	moodycamel::ConsumerToken& waitingConsumerToken)
{
	Task task;
	while (myReadyQueue.try_dequeue(readyConsumerToken, task))
	{
		task();
		scheduleAdjacent(task);
	}
}

void TaskExecutor::threadMain(/*std::stop_token& stopToken*/)
{
	try
	{
		moodycamel::ProducerToken readyProducerToken(myReadyQueue);
		moodycamel::ConsumerToken readyConsumerToken(myReadyQueue);
		moodycamel::ProducerToken waitingProducerToken(myWaitingQueue);
		moodycamel::ConsumerToken waitingConsumerToken(myWaitingQueue);

		//while (!stopToken.stop_requested())
		while (!myStopSource.load(std::memory_order_relaxed))
		{
			processQueues(
				readyProducerToken,
				readyConsumerToken,
				waitingProducerToken,
				waitingConsumerToken);

			//auto lock = std::shared_lock(myMutex);

			//mySignal.wait(lock, stopToken, [&queue = myReadyQueue](){ return !queue.empty(); });
			//mySignal.wait(lock, [&stopSource = myStopSource, &queue = myReadyQueue](){ return stopSource.load(std::memory_order_relaxed) || queue.size_approx(); });
			mySignal.acquire();
		}
	}
	catch (const std::runtime_error& ex)
	{
		std::cerr << "Error! - ";
		std::cerr << ex.what() << "\n";

		throw;
	}
	catch (...)
	{
		std::cerr << "Error! Unhandled exception";

		throw;
	}
}
