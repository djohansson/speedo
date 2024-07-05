#include "taskexecutor.h"
#include "assert.h"//NOLINT(modernize-deprecated-headers)

#include <iostream>
#include <shared_mutex>

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

	Task& task = *Task::InternalHandleToPtr(handle);

	if (task.InternalState()->latch.load(std::memory_order_relaxed) == 0 && task.InternalState()->mutex.try_lock())
	{
		auto state = task.InternalState();
		std::destroy_at(&task);
		state->mutex.unlock();
		Task::InternalFree(handle);
		return true;
	}

	return false;
}

void TaskExecutor::InternalScheduleAdjacent(Task& task)
{
	ZoneScopedN("TaskExecutor::InternalScheduleAdjacent");

	std::shared_lock lock(task.InternalState()->mutex);

	for (auto& adjacentHandle : task.InternalState()->adjacencies)
	{
		if (!adjacentHandle) // this is ok, means that another thread has claimed it
			continue;

		Task& adjacent = *Task::InternalHandleToPtr(adjacentHandle);

		ASSERTF(adjacent.InternalState(), "Task has no return state!");
		ASSERTF(adjacent.InternalState()->latch, "Latch needs to have been constructed!");

		if (adjacent.InternalState()->latch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
		{
			auto handle = std::exchange(adjacentHandle, {});
			Submit({&handle, 1});
		}
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

	TaskHandle handle;
	while (myDeletionQueue.try_dequeue(handle))
	{
		if (!InternalTryDelete(handle))
			myDeletionQueue.enqueue(handle);
	}
}

void TaskExecutor::InternalThreadMain(uint32_t threadId)
{
	std::shared_lock lock(myMutex);
	
	auto stopToken = myStopSource.get_token();
	while (!stopToken.stop_requested())
	{
		if (myCV.wait(lock, stopToken, [this]{ return myReadyQueue.size_approx() > 0; }))
			InternalProcessReadyQueue();
	}
}

void TaskExecutor::InternalSubmit(std::span<TaskHandle> handles)
{
	ZoneScopedN("TaskExecutor::InternalSubmit");

	CHECK(myReadyQueue.enqueue_bulk(handles.data(), handles.size()));
}
