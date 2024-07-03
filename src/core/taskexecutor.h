#pragma once

#include "future.h"
#include "memorypool.h"
#include "task.h"
#include "utils.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <stop_token>
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
	template <typename... TaskHandles>
	void Submit(TaskHandles&&... handles) { InternalSubmit(handles...); }

private:
	template <typename... Params>
	void InternalCall(TaskHandle handle, Params&&... params);
	template <typename... Params>
	void InternalCall(ProducerToken& readyProducerToken, TaskHandle handle, Params&&... params);

	template <typename... TaskHandles>
	void InternalSubmit(TaskHandles&&... handles);
	template <typename... TaskHandles>
	void InternalSubmit(ProducerToken& readyProducerToken, TaskHandles&&... handles);
	
	[[nodiscard]] static bool InternalTryDelete(TaskHandle handle);

	void InternalScheduleAdjacent(Task& task);
	void InternalScheduleAdjacent(ProducerToken& readyProducerToken, Task& task);

	void InternalProcessReadyQueue();
	void InternalProcessReadyQueue(ProducerToken& readyProducerToken, ConsumerToken& readyConsumerToken);
	template <typename R>
	[[nodiscard]] std::optional<typename Future<R>::value_t> InternalProcessReadyQueue(Future<R>&& future);

	void InternalPurgeDeletionQueue();

	void InternalThreadMain(uint32_t threadId);

	std::vector<std::jthread> myThreads;
	std::stop_source myStopSource;
	UpgradableSharedMutex myMutex;
	std::condition_variable_any myCV;
	std::atomic_uint32_t mySignal;
	ConcurrentQueue<TaskHandle> myReadyQueue;
	ConcurrentQueue<TaskHandle> myDeletionQueue;
};

#include "taskexecutor.inl"
