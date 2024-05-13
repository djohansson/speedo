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
public:
	
	TaskExecutor(uint32_t threadCount);
	~TaskExecutor();

	// b will start after a has finished
	// isContinuation == true : b will most likely start on the same thread as a, but may start on any thread in the thread pool
	// isContinuation == false: b will start on any thread in the thread pool
	void addDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation = false);

	template <
		typename... Params,
		typename... Args,
		typename F,
		typename C = std::decay_t<F>,
		typename ArgsTuple = std::tuple<Args...>,
		typename ParamsTuple = std::tuple<Params...>,
		typename R = std_extra::apply_result_t<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>>
	requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
	std::pair<TaskHandle, Future<R>> createTask(F&& callable, Args&&... args);

	// wait for task to finish while helping out processing the thread pools ready queue
	// as soon as the task is ready, the function will stop processing the ready queue and return
	template <typename R>
	std::optional<typename Future<R>::value_t> join(Future<R>&& future);

	// call task in current thread. dependency chain(s) will be executed in thread pool
	template <typename... TaskParams>
	void call(TaskHandle handle, TaskParams&&... params);

	// task + dependency chain(s) will be executed in thread pool
	void submit(TaskHandle handle) { internalSubmit(handle); }

private:
	Task* handleToTaskPtr(TaskHandle handle) noexcept;

	template <typename... TaskParams>
	void internalCall(TaskHandle handle, TaskParams&&... params);
	template <typename... TaskParams>
	void internalCall(ProducerToken& readyProducerToken, TaskHandle handle, TaskParams&&... params);

	void internalSubmit(TaskHandle handle);
	void internalSubmit(ProducerToken& readyProducerToken, TaskHandle handle);
	void internalTryDelete(TaskHandle handle);
	
	void scheduleAdjacent(Task& task);
	void scheduleAdjacent(ProducerToken& readyProducerToken, Task& task);

	void processReadyQueue();
	void processReadyQueue(ProducerToken& readyProducerToken, ConsumerToken& readyConsumerToken);
	template <typename R>
	std::optional<typename Future<R>::value_t> processReadyQueue(Future<R>&& future);

	void purgeDeletionQueue();

	void threadMain(uint32_t threadId);

	std::vector<std::tuple<std::thread, std::exception_ptr>> myThreads;
	std::counting_semaphore<> mySignal;
	std::atomic_bool myStopSource;
	ConcurrentQueue<TaskHandle> myReadyQueue;
	ConcurrentQueue<TaskHandle> myDeletionQueue;
	static constexpr uint32_t TaskPoolSize = (1 << 10); // todo: make this configurable
	static MemoryPool<Task, TaskPoolSize> gTaskPool;
};

#include "taskexecutor.inl"
