#include "profiling.h"

template <typename R>
std::optional<typename Future<R>::value_t> TaskExecutor::Join(Future<R>&& future)
{
	ZoneScopedN("TaskExecutor::Join");

	return InternalProcessReadyQueue(std::forward<Future<R>>(future));
}

template <typename R>
std::pair<std::optional<typename Future<R>::value_t>, uint32_t> TaskExecutor::InternalProcessReadyQueue(Future<R>&& future)
{
	ZoneScopedN("TaskExecutor::InternalProcessReadyQueue");

	if (!future.Valid())
		return {std::nullopt, 0};

	uint32_t count = 0;
	TaskHandle handle;
	while (!future.IsReady() && myReadyQueue.try_dequeue(handle))
	{
		InternalCall(handle);
		InternalPurgeDeletionQueue();
		++count;
	}

	return {std::make_optional(future.Get()), count};
}

template <typename... Params>
void TaskExecutor::InternalCall(TaskHandle handle, Params&&... params)
{
	ZoneScopedN("TaskExecutor::InternalCall");

	Task& task = *Task::InternalHandleToPtr(handle);

	ASSERT(task.InternalState()->latch.load(std::memory_order_relaxed) == 1);
	
	task(params...);
	InternalScheduleAdjacent(task);
	myDeletionQueue.enqueue(handle);
}

template <typename... Params>
void TaskExecutor::InternalCall(ProducerToken& readyProducerToken, TaskHandle handle, Params&&... params)
{
	ZoneScopedN("TaskExecutor::InternalCall");

	Task& task = *Task::InternalHandleToPtr(handle);

	ASSERT(task.InternalState()->latch.load(std::memory_order_relaxed) == 1);
	
	task(params...);
	InternalScheduleAdjacent(readyProducerToken, task);
	myDeletionQueue.enqueue(handle);
}

template <typename... TaskHandles>
void TaskExecutor::InternalSubmit(TaskHandles&&... handles)
{
	ZoneScopedN("TaskExecutor::InternalSubmit");

	std::array<TaskHandle, sizeof...(handles)> handleArray{{handles...}};
	myReadyQueue.enqueue_bulk(handleArray.data(), handleArray.size());
	myTaskCount += sizeof...(handles);
	if constexpr (sizeof...(handles) > 1)
		myCV.notify_all();
	else
		myCV.notify_one();
}

template <typename... TaskHandles>
void TaskExecutor::InternalSubmit(ProducerToken& readyProducerToken, TaskHandles&&... handles)
{
	ZoneScopedN("TaskExecutor::InternalSubmit");

	std::array<TaskHandle, sizeof...(handles)> handleArray{{handles...}};
	myReadyQueue.enqueue_bulk(readyProducerToken, handleArray.data(), handleArray.size());
	myTaskCount += sizeof...(handles);
	if constexpr (sizeof...(handles) > 1)
		myCV.notify_all();
	else
		myCV.notify_one();
}
