#pragma once

#include "future.h"
#include "memorypool.h"
#include "task.h"
#include "utils.h"

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
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

	// b will start after a has finished on any thread in the thread pool
	void addDependency(TaskHandle a, TaskHandle b);

	template <
		typename F,
		typename CallableType = std::decay_t<F>,
		typename... Args,
		typename ReturnType = std::invoke_result_t<CallableType, Args...>>
	requires std::invocable<F&, Args...>
	std::pair<TaskHandle, Future<ReturnType>> createTask(F&& f, Args&&... args);

	template <typename ReturnType>
	std::optional<typename Future<ReturnType>::value_t> join(Future<ReturnType>&& future);

	// call task in current thread. dependency chain(s) will be executed in thread pool
	template <typename T, typename... Ts>
	void call(T&& first, Ts&&... rest);

	// submit task + dependency chain(s) to be executed in thread pool
	template <typename T, typename... Ts>
	void submit(T&& first, Ts&&... rest);

private:
	void internalCall(TaskHandle handle);
	void internalSubmit(TaskHandle handle);
	void internalSubmit(ProducerToken& readyProducerToken, TaskHandle handle);
	
	void scheduleAdjacent(Task& task);
	void scheduleAdjacent(ProducerToken& readyProducerToken, Task& task);

	void processReadyQueue();
	void processReadyQueue(ProducerToken& readyProducerToken, ConsumerToken& readyConsumerToken);
	template <typename ReturnType>
	std::optional<typename Future<ReturnType>::value_t> processReadyQueue(Future<ReturnType>&& future);

	void purgeDeletionQueue();

	void threadMain(uint32_t threadId);

	std::vector<std::tuple<std::thread, std::exception_ptr>> myThreads;
	std::counting_semaphore<> mySignal;
	std::atomic_bool myStopSource;
	ConcurrentQueue<TaskHandle> myReadyQueue;
	ConcurrentQueue<TaskHandle> myDeletionQueue;
	static constexpr uint32_t TaskPoolSize = (1 << 15);
	static MemoryPool<Task, TaskPoolSize> ourTaskPool;
	static std::mutex ourTaskPoolMutex;
};

#include "taskexecutor.inl"
