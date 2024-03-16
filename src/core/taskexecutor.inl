template <typename ReturnType>
std::optional<typename Future<ReturnType>::value_t> TaskExecutor::join(Future<ReturnType>&& future)
{
	ZoneScopedN("TaskExecutor::join");

	return processReadyQueue(std::forward<Future<ReturnType>>(future));
}

template <typename ReturnType>
std::optional<typename Future<ReturnType>::value_t> TaskExecutor::processReadyQueue(Future<ReturnType>&& future)
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	if (!future.valid())
		return std::nullopt;

	TaskHandle task;
	while (!future.is_ready() && myReadyQueue.try_dequeue(task))
	{
		handleToTaskRef(task)();
		scheduleAdjacent(task);
		myDeletionQueue.enqueue(task);
	}

	return std::make_optional(future.get());
}

template <typename F, typename CallableType, typename... Args, typename ReturnType>
requires std::invocable<F&, Args...>
std::pair<TaskHandle, Future<ReturnType>> TaskExecutor::createTask(F&& f, Args&&... args)
{
	ZoneScopedN("TaskExecutor::createTask");

	Task* taskPtr = ourTaskPool.allocate();
	auto [taskIt, wasInserted] = myTasks.emplace(
		taskPtr,
		TaskUniquePtr(
			new (taskPtr) Task(std::forward<F>(f),std::forward<Args>(args)...),
			[](Task* task)
			{
				task->~Task();
				ourTaskPool.free(task);
			}));

	assert(wasInserted);

	return std::make_pair(
		taskIt->first,
		Future<ReturnType>(
			std::static_pointer_cast<typename Future<ReturnType>::FutureState>(taskIt->second->state())));
}
