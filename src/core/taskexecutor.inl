template <typename R>
std::optional<typename Future<R>::value_t> TaskExecutor::Join(Future<R>&& future)
{
	ZoneScopedN("TaskExecutor::join");

	return ProcessReadyQueue(std::forward<Future<R>>(future));
}

template <typename R>
std::optional<typename Future<R>::value_t> TaskExecutor::ProcessReadyQueue(Future<R>&& future)
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	if (!future.Valid())
		return std::nullopt;

	TaskHandle handle;
	while (!future.IsReady() && myReadyQueue.try_dequeue(handle))
	{
		InternalCall(handle);
		PurgeDeletionQueue();
	}

	return std::make_optional(future.Get());
}

template <typename... Params>
void TaskExecutor::InternalCall(TaskHandle handle, Params&&... params)
{
	ZoneScopedN("TaskExecutor::internalCall");

	Task& task = *Task::InternalHandleToPtr(handle);

	ASSERT(task.InternalState()->latch.load(std::memory_order_relaxed) == 1);
	
	task(params...);
	ScheduleAdjacent(task);
	myDeletionQueue.enqueue(handle);
}

template <typename... Params>
void TaskExecutor::InternalCall(ProducerToken& readyProducerToken, TaskHandle handle, Params&&... params)
{
	ZoneScopedN("TaskExecutor::internalCall");

	Task& task = *Task::InternalHandleToPtr(handle);

	ASSERT(task.InternalState()->latch.load(std::memory_order_relaxed) == 1);
	
	task(params...);
	ScheduleAdjacent(readyProducerToken, task);
	myDeletionQueue.enqueue(handle);
}

template <typename... Params>
void TaskExecutor::Call(TaskHandle handle, Params&&... params)
{
	InternalCall(handle, params...);
}

