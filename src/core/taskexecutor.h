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

	// wait for task to finish while helping out processing the thread pools ready queue
	// as soon as the task is ready, the function will stop processing the ready queue and return
	template <typename R>
	std::optional<typename Future<R>::value_t> Join(Future<R>&& future);

	// call task in current thread. dependency chain(s) will be executed in thread pool
	template <typename... TaskParams>
	void Call(TaskHandle handle, TaskParams&&... params);

	// task + dependency chain(s) will be executed in thread pool
	void Submit(TaskHandle handle) { InternalSubmit(handle); }

private:
	template <typename... TaskParams>
	void InternalCall(TaskHandle handle, TaskParams&&... params);
	template <typename... TaskParams>
	void InternalCall(ProducerToken& readyProducerToken, TaskHandle handle, TaskParams&&... params);

	void InternalSubmit(TaskHandle handle);
	void InternalSubmit(ProducerToken& readyProducerToken, TaskHandle handle);
	static void InternalTryDelete(TaskHandle handle);

	void ScheduleAdjacent(Task& task);
	void ScheduleAdjacent(ProducerToken& readyProducerToken, Task& task);

	void ProcessReadyQueue();
	void ProcessReadyQueue(ProducerToken& readyProducerToken, ConsumerToken& readyConsumerToken);
	template <typename R>
	std::optional<typename Future<R>::value_t> ProcessReadyQueue(Future<R>&& future);

	void PurgeDeletionQueue();

	void ThreadMain(uint32_t threadId);

	std::vector<std::tuple<std::thread, std::exception_ptr>> myThreads;
	std::counting_semaphore<> mySignal;
	std::atomic_bool myStopSource;
	ConcurrentQueue<TaskHandle> myReadyQueue;
	ConcurrentQueue<TaskHandle> myDeletionQueue;
};

#include "taskexecutor.inl"
