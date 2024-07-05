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
#include <span>
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
	void Submit(std::span<TaskHandle> handles, bool wakeThreads = true)
	{
		InternalSubmit(handles);
		
		if (auto count = handles.size(); wakeThreads && count > 0)
		{
			if (count >= myThreads.size())
				myCV.notify_all();
			else while (count--)
				myCV.notify_one();
		}
	}

private:
	template <typename... Params>
	void InternalCall(TaskHandle handle, Params&&... params);

	void InternalSubmit(std::span<TaskHandle> handles);

	[[nodiscard]] static bool InternalTryDelete(TaskHandle handle);

	void InternalScheduleAdjacent(Task& task);

	void InternalProcessReadyQueue();

	template <typename R>
	[[nodiscard]] std::optional<typename Future<R>::value_t> InternalProcessReadyQueue(Future<R>&& future);

	void InternalPurgeDeletionQueue();

	void InternalThreadMain(uint32_t threadId);

	std::vector<std::jthread> myThreads;
	std::stop_source myStopSource;
	UpgradableSharedMutex myMutex;
	std::condition_variable_any myCV;
	ConcurrentQueue<TaskHandle> myReadyQueue;
	ConcurrentQueue<TaskHandle> myDeletionQueue;
};

#include "taskexecutor.inl"
