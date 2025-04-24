#include "profiling.h"

#include <atomic>

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
		std::atomic_ref(myReadyQueueSize).fetch_sub(1, std::memory_order_acq_rel);
		InternalCall(handle);
		InternalPurgeDeletionQueue();
	}

	return std::make_optional(future.Get());
}

template <typename... Params>
void TaskExecutor::InternalCall(TaskHandle handle, Params&&... params)
{
	ZoneScopedN("TaskExecutor::InternalCall");

	Task& task = *core::detail::InternalHandleToPtr(handle);
	auto& state = *std::atomic_load(&task.InternalState());
	
	ASSERT(std::atomic_ref(state.latch).load(std::memory_order_relaxed) == 1);

	task(params...);
	InternalScheduleAdjacent(task);
	myDeletionQueue.enqueue(handle);
}
