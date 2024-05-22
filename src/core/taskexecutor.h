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
	explicit TaskExecutor(uint32_t threadCount);
	~TaskExecutor();

	// wait for task to finish while helping out processing the thread pools ready queue
	// as soon as the task is ready, the function will stop processing the ready queue and return
	template <typename R>
	std::optional<typename Future<R>::value_t> Join(Future<R>&& future);

	// call task in current thread. dependency chain(s) will be executed in thread pool
	template <typename... Params>
	void Call(TaskHandle handle, Params&&... params) { InternalCall(handle, params...); }

	// task + dependency chain(s) will be executed in thread pool
	void Submit(TaskHandle handle) { InternalSubmit(handle); }

private:
	template <typename... Params>
	void InternalCall(TaskHandle handle, Params&&... params);
	template <typename... Params>
	void InternalCall(ProducerToken& readyProducerToken, TaskHandle handle, Params&&... params);

	void InternalSubmit(TaskHandle handle);
	void InternalSubmit(ProducerToken& readyProducerToken, TaskHandle handle);
	static void InternalTryDelete(TaskHandle handle);

	void InternalScheduleAdjacent(Task& task);
	void InternalScheduleAdjacent(ProducerToken& readyProducerToken, Task& task);

	void InternalProcessReadyQueue();
	void InternalProcessReadyQueue(ProducerToken& readyProducerToken, ConsumerToken& readyConsumerToken);
	template <typename R>
	[[nodiscard]] std::optional<typename Future<R>::value_t> InternalProcessReadyQueue(Future<R>&& future);

	void InternalPurgeDeletionQueue();

	void InternalThreadMain(uint32_t threadId);

	std::vector<std::tuple<std::thread, std::exception_ptr>> myThreads;
	std::counting_semaphore<> mySignal;
	std::atomic_bool myStopSource;
	ConcurrentQueue<TaskHandle> myReadyQueue;
	ConcurrentQueue<TaskHandle> myDeletionQueue;
};

#include "taskexecutor.inl"
