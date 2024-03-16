#pragma once

#include "future.h"
#include "memorypool.h"
#include "task.h"
#include "utils.h"

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <semaphore>
#include <vector>
#include <thread>
#include <tuple>

using TaskHandleVector = std::vector<TaskHandle>;

class TaskExecutor
{
	using TaskUniquePtr = std::unique_ptr<Task, std::function<void(Task*)>>;

public:
	
	TaskExecutor(uint32_t threadCount);
	~TaskExecutor();

	//void addDependency(TaskHandle a, TaskHandle b); // b will start after a has finished

	template <
		typename F,
		typename CallableType = std::decay_t<F>,
		typename... Args,
		typename ReturnType = std::invoke_result_t<CallableType, Args...>>
	requires std::invocable<F&, Args...>
	std::pair<TaskHandle, Future<ReturnType>> createTask(F&& f, Args&&... args);

	template <typename ReturnType>
	std::optional<typename Future<ReturnType>::value_t> join(Future<ReturnType>&& future);

	void submit(TaskHandle handle);

private:

	void initializeGraph();
	void scheduleAdjacent(TaskHandle task);
	void scheduleAdjacent(ProducerToken& readyProducerToken, TaskHandle task);

	template <typename ReturnType>
	std::optional<typename Future<ReturnType>::value_t> processReadyQueue(Future<ReturnType>&& future);

	void processReadyQueue();
	void processReadyQueue(ProducerToken& readyProducerToken, ConsumerToken& readyConsumerToken);

	void threadMain(uint32_t threadId);

	std::vector<std::tuple<std::thread, std::exception_ptr>> myThreads;
	std::counting_semaphore<> mySignal;
	std::atomic_bool myStopSource;
	ConcurrentQueue<TaskHandle> myReadyQueue;
	ConcurrentQueue<TaskHandle> myDeletionQueue;
	UnorderedMap<TaskHandle, TaskUniquePtr> myTasks;
	static constexpr uint32_t TaskPoolSize = (1 << 15);
	static MemoryPool<Task, TaskPoolSize> ourTaskPool;
};

#include "taskexecutor.inl"
