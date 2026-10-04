#pragma once

#include <core/task.h>
#include <core/utils.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>
#include <span>
#include <stop_token>
#include <thread>

namespace core
{

class TaskExecutor
{
public:
	explicit TaskExecutor(uint32_t threadCount);
	~TaskExecutor();

	// wait for task to finish while helping out processing the thread pools ready queue
	// as soon as the task is ready, the function will stop processing the ready queue and return
	template <typename R>
	[[maybe_unused]] std::optional<typename Future<R>::value_t> Join(Future<R>&& future);

	// help out processing the thread pools ready queue one task at a time
	void JoinOne();

	// help out processing the thread pools ready queue until it is empty and no task is executing on any thread
	// (including tasks submitted by other tasks while waiting). must not be called from within a task.
	void JoinAll();

	// blocking call in current thread. dependency chain(s) will be executed asynchrounously in thread pool
	template <typename... Params>
	void Call(TaskHandle handle, Params&&... params) { InternalCall(handle, params...); }

	// async call. task + dependency chain(s) will be executed in thread pool.
	// if wakeThreads is false, the task will be enqueued but not executed until it is picked up by a running thread.
	void Submit(std::span<const TaskHandle> handles, bool wakeThreads = true);

private:
	template <typename... Params>
	void InternalCall(TaskHandle handle, Params&&... params);

	void InternalSubmit(std::span<const TaskHandle> handles);

	[[nodiscard]] static bool InternalTryDelete(TaskHandle handle);

	void InternalScheduleAdjacent(Task& task);

	// dequeues and executes a single ready task, if any. returns false if the ready queue was empty.
	bool InternalTryCallOne();

	void InternalProcessReadyQueue();
	template <typename R>
	[[nodiscard]] std::optional<typename Future<R>::value_t> InternalProcessReadyQueue(Future<R>&& future);

	void InternalPurgeDeletionQueue();

	void InternalThreadMain(uint32_t threadIndex);

	std::vector<std::jthread> myThreads;
	std::stop_source myStopSource;
	// idle threads wait for this to change: it is bumped after work is submitted (with wakeThreads) and on stop. an
	// atomic rather than a condition variable, so submitting takes no lock, and a thread can't miss a wake between
	// finding the ready queue empty and starting to wait (see InternalThreadMain).
	std::atomic_uint32_t myWakeCount = 0;
	mutable ConcurrentQueue<TaskHandle> myReadyQueue;
	uint64_t myReadyQueueSize = 0;
	uint64_t myActiveTaskCount = 0; // tasks dequeued (or about to be) and not yet finished, see InternalTryCallOne
	mutable ConcurrentQueue<TaskHandle> myDeletionQueue;
};

} // namespace core

#include "taskexecutor.inl"
