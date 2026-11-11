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

	// how many threads run its tasks
	[[nodiscard]] uint32_t GetThreadCount() const noexcept { return static_cast<uint32_t>(myThreads.size()); }

	// wait for task to finish while helping out processing the thread pools ready queue
	// as soon as the task is ready, the function will stop processing the ready queue and return
	template <typename R>
	[[maybe_unused]] std::optional<typename Future<R>::value_t> Join(Future<R>&& future);

	// calls body(index) for each index below count, on the calling thread and worker tasks (one per thread in all, at most
	// count: made for the threads idle at the call, at least one, and each recruiting the next as it starts while indices
	// are left) taking the indices in turn, and returns once all are done. body runs on any thread, concurrently with itself;
	// to stop early, it can skip the indices left. it may be called from within a task: the caller takes indices as the
	// workers do, then waits for the ones the workers are running, and never runs another task from the ready queue, since
	// the caller may hold what that task needs: the draw thread holds the graphics queue's lock for its frame, and a texture
	// load its ForEach took while waiting (Model::Animate placing what moves) asked for that lock and waited on itself
	template <typename Fn>
	void ForEach(size_t count, Fn&& body);

	// help out processing the thread pools ready queue one task at a time: false if there was none to run
	[[maybe_unused]] bool JoinOne();

	// help out processing the thread pools ready queue until it is empty and no task is executing on any thread
	// (including tasks submitted by other tasks while waiting). must not be called from within a task.
	void JoinAll();

	// blocking call in current thread. dependency chain(s) will be executed asynchrounously in thread pool
	void Call(TaskHandle handle) { InternalCall(handle); }

	// async call. task + dependency chain(s) will be executed in thread pool.
	// if wakeThreads is false, the task will be enqueued but not executed until it is picked up by a running thread.
	void Submit(std::span<const TaskHandle> handles, bool wakeThreads = true);

private:
	void InternalCall(TaskHandle handle);

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

	// the threads' stacks: a main thread's on macOS and Linux (a std::thread's on macOS is 512 KB). a thread waiting in Join
	// runs whatever is ready meanwhile, so nested waits stack task upon task: the BVH builder's spatial splits wait on the
	// partitions they spawned, and while waiting run another partition, which waits in turn, and the ui's tick (PrepareDraw's
	// frame, big in a debug build) landed on top of rungholt's and ran past the 512 KB (ForEach then ran whatever was ready too)
	static constexpr size_t kThreadStackByteCount = size_t{8} << 20;

	std::vector<std_extra::jthread> myThreads;
	std::stop_source myStopSource;
	// idle threads wait for this to change: it is bumped after work is submitted (with wakeThreads) and on stop. an
	// atomic rather than a condition variable, so submitting takes no lock, and a thread can't miss a wake between
	// finding the ready queue empty and starting to wait (see InternalThreadMain).
	std::atomic_uint32_t myWakeCount = 0;
	// threads waiting for work, which ForEach makes its first workers for (see ForEach)
	std::atomic_uint32_t myIdleThreadCount = 0;
	// the tasks ready to run: moodycamel's queue keeps one per producing thread, and a dequeue without a consumer token
	// takes from the fullest of the first three non-empty ones, newest first, a tie to the first, so chains resubmitting
	// themselves into their threads' queues (the client's tick and server connection) won every time over the draw chain
	// and the shader compile the main thread had submitted, which never ran on 2 threads (a 4-core machine, 2026-11-11).
	// the executor's threads dequeue with tokens of their own (myConsumerTokens), which rotate over the producers' queues
	// after every task (ReadyQueueTraits), as the library's fairness; other threads (JoinAll's) without
	struct ReadyQueueTraits : moodycamel::ConcurrentQueueDefaultTraits
	{
		static constexpr std::uint32_t EXPLICIT_CONSUMER_CONSUMPTION_QUOTA_BEFORE_ROTATE = 1; // NOLINT(readability-identifier-naming): the library's name
	};
	mutable moodycamel::ConcurrentQueue<TaskHandle, ReadyQueueTraits> myReadyQueue;
	std::vector<moodycamel::ConsumerToken> myConsumerTokens; // a token for each thread, made before the threads start
	uint64_t myReadyQueueSize = 0;
	uint64_t myActiveTaskCount = 0; // tasks dequeued (or about to be) and not yet finished, see InternalTryCallOne
	mutable ConcurrentQueue<TaskHandle> myDeletionQueue;
};

} // namespace core

#include "taskexecutor.inl"
