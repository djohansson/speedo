#include "profiling.h"

template <typename R>
std::optional<typename Future<R>::value_t> TaskExecutor::Join(Future<R>&& future)
{
	ZoneScopedN("TaskExecutor::Join");

	return InternalProcessReadyQueue(std::forward<Future<R>>(future));
}

template <typename R>
std::optional<typename Future<R>::value_t> TaskExecutor::InternalProcessReadyQueue(Future<R>&& future)
{
	ZoneScopedN("TaskExecutor::InternalProcessReadyQueue");

	if (!future.Valid())
		return std::nullopt;

	TaskHandle handle;
	while (!future.IsReady() && myReadyQueue.try_dequeue(handle))
	{
		InternalCall(handle);
		InternalPurgeDeletionQueue();
	}

	return std::make_optional(future.Get());
}

template <typename... Params>
void TaskExecutor::InternalCall(TaskHandle handle, Params&&... params)
{
	ZoneScopedN("TaskExecutor::InternalCall");

	Task& task = *InternalHandleToPtr(handle);

	ASSERT(task.InternalState()->latch.load(std::memory_order_relaxed) == 1);
	
	task(params...);
	InternalScheduleAdjacent(task);
	myDeletionQueue.enqueue(handle);
}
