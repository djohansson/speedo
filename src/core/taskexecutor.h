#pragma once

#include "future.h"
#include "task.h"
#include "taskgraph.h"
#include "utils.h"

#include <atomic>
#include <exception>
#include <optional>
#include <semaphore>
#include <vector>
#include <thread>
#include <tuple>

#include <concurrentqueue/concurrentqueue.h>

class TaskExecutor : public Noncopyable
{
public:
	TaskExecutor(uint32_t threadCount);
	~TaskExecutor();

	void submit(TaskGraph&& graph); // will invalidate all task handles allocated from graph

	template <typename ReturnType>
	std::optional<typename Future<ReturnType>::value_t> join(Future<ReturnType>&& future);

private:
	void scheduleAdjacent(const Task& task);
	void scheduleAdjacent(moodycamel::ProducerToken& readyProducerToken, const Task& task);

	template <typename ReturnType>
	std::optional<typename Future<ReturnType>::value_t>
	processReadyQueue(Future<ReturnType>&& future);
	void processReadyQueue();
	void processReadyQueue(
		moodycamel::ProducerToken& readyProducerToken,
		moodycamel::ConsumerToken& readyConsumerToken);

	void removeFinishedGraphs();

	void threadMain(uint32_t threadId);

	std::vector<std::tuple<std::jthread, std::exception_ptr>> myThreads;
	std::counting_semaphore<> mySignal;
	std::atomic_bool myStopSource;
	// todo: use pool allocator for tasks, and just use pointer here
	moodycamel::ConcurrentQueue<Task> myReadyQueue;
	moodycamel::ConcurrentQueue<TaskGraph> myWaitingQueue;
};

#include "taskexecutor.inl"
