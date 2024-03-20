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

class TaskExecutor
{
	using TaskUniquePtr = std::unique_ptr<Task, std::function<void(Task*)>>;

public:
	
	TaskExecutor(uint32_t threadCount);
	~TaskExecutor();

	// b will start after a has finished
	// isContinuation == true : b will most likely start on the same thread as a, but may start on any thread in the thread pool
	// isContinuation == false: b will start on any thread in the thread pool
	void addDependency(TaskHandle a, TaskHandle b, bool isContinuation = false);

	template <
		typename F,
		typename CallableType = std::decay_t<F>,
		typename... Args,
		typename ReturnType = std::invoke_result_t<CallableType, Args...>>
	requires std::invocable<F&, Args...>
	std::pair<TaskHandle, Future<ReturnType>> createTask(F&& f, Args&&... args);

	// wait for task to finish while helping out processing the thread pools ready queue
	// as soon as the task is ready, the function will stop processing the ready queue and return
	template <typename ReturnType>
	std::optional<typename Future<ReturnType>::value_t> join(Future<ReturnType>&& future);

	// call task(s) in current thread. dependency chain(s) will be executed in thread pool
	template <typename T, typename... Ts>
	void call(T&& first, Ts&&... rest);

	// task(s) + dependency chain(s) will be executed in thread pool
	template <typename T, typename... Ts>
	void submit(T&& first, Ts&&... rest);

private:
	Task* handleToTaskPtr(TaskHandle handle) noexcept;
	
	void internalCall(TaskHandle handle);
	void internalCall(ProducerToken& readyProducerToken, TaskHandle handle);
	void internalSubmit(TaskHandle handle);
	void internalSubmit(ProducerToken& readyProducerToken, TaskHandle handle);
	void internalTryDelete(TaskHandle handle);
	
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
};

#include "taskexecutor.inl"
