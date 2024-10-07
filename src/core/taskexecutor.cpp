#include "taskexecutor.h"
#include "assert.h"//NOLINT(modernize-deprecated-headers)

#include <iostream>
#include <shared_mutex>

// #if !defined(__cpp_lib_atomic_shared_ptr) || __cpp_lib_atomic_shared_ptr < 201711L
// static_assert(false, "std::atomic<std::shared_ptr> is not supported by the standard library!");
// #endif

TaskExecutor::TaskExecutor(uint32_t threadCount)
{
	ZoneScopedN("TaskExecutor()");

	ASSERTF(threadCount > 0, "Thread count must be nonzero");

	myThreads.reserve(threadCount);

	for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
		myThreads.emplace_back(std::jthread(std::bind_front(&TaskExecutor::InternalThreadMain, this), threadIt));
}

TaskExecutor::~TaskExecutor()
{
	ZoneScopedN("~TaskExecutor()");

	ASSERT(myReadyQueue.size_approx() == 0);
	ASSERT(myDeletionQueue.size_approx() == 0);

	myStopSource.request_stop();

	for (auto& thread : myThreads)
		thread.join();
}

bool TaskExecutor::InternalTryDelete(TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::InternalTryDelete");

	Task& task = *core::detail::InternalHandleToPtr(handle);
	ASSERT(task);
	auto state = std::atomic_load(&task.InternalState());
	
	if (state->Latch().load(std::memory_order_relaxed) == 0)
	{
		std::destroy_at(&task);
		core::detail::InternalFree(handle);
		return true;
	}

	return false;
}

void TaskExecutor::InternalScheduleAdjacent(Task& task)
{
	ZoneScopedN("TaskExecutor::InternalScheduleAdjacent");

	auto state = std::atomic_load(&task.InternalState());

	for (auto adjIt = 0; adjIt < state->adjacenciesCount; adjIt++)
	{
		TaskHandle adjacentHandle = state->adjacencies[adjIt];
		Task& adjacent = *core::detail::InternalHandleToPtr(adjacentHandle);
		ASSERT(adjacent);
		auto adjacentState = std::atomic_load(&adjacent.InternalState());
		auto latch = adjacentState->Latch();
		ASSERTF(latch, "Latch needs to have been constructed!");

		if (latch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
			Submit({&adjacentHandle, 1}, !adjacentState->continuation);
	}
}

void TaskExecutor::InternalProcessReadyQueue()
{
	ZoneScopedN("TaskExecutor::InternalProcessReadyQueue");

	TaskHandle handle;
	while (myReadyQueue.try_dequeue(handle))
	{
		InternalCall(handle);
		InternalPurgeDeletionQueue();
	}
}

void TaskExecutor::InternalPurgeDeletionQueue()
{
	ZoneScopedN("TaskExecutor::InternalPurgeDeletionQueue");

	static thread_local std::vector<TaskHandle> nextDeletionQueue;
	nextDeletionQueue.reserve(std::max(myDeletionQueue.size_approx(), nextDeletionQueue.size()));
	nextDeletionQueue.clear();

	TaskHandle handle;
	while (myDeletionQueue.try_dequeue(handle))
		if (!InternalTryDelete(handle))
			nextDeletionQueue.emplace_back(handle);

	myDeletionQueue.enqueue_bulk(nextDeletionQueue.begin(), nextDeletionQueue.size());
}

void TaskExecutor::InternalThreadMain(uint32_t threadId)
{
	std::shared_lock lock(myMutex);
	auto stopToken = myStopSource.get_token();

	while (!stopToken.stop_requested())
		if (myCV.wait(lock, stopToken, [this]{ return myReadyQueue.size_approx() > 0; }))
			InternalProcessReadyQueue();
}

void TaskExecutor::InternalSubmit(std::span<TaskHandle> handles)
{
	CHECK(myReadyQueue.enqueue_bulk(handles.data(), handles.size()));
}

void TaskExecutor::Submit(std::span<TaskHandle> handles, bool wakeThreads)
{
	ZoneScopedN("TaskExecutor::Submit");

	InternalSubmit(handles);
	
	if (auto count = handles.size(); wakeThreads && count > 0)
	{
		if (count >= myThreads.size())
			myCV.notify_all();
		else while (count--)
			myCV.notify_one();
	}
}