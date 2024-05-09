template <typename R>
std::optional<typename Future<R>::value_t> TaskExecutor::join(Future<R>&& future)
{
	ZoneScopedN("TaskExecutor::join");

	return processReadyQueue(std::forward<Future<R>>(future));
}

template <typename R>
std::optional<typename Future<R>::value_t> TaskExecutor::processReadyQueue(Future<R>&& future)
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	if (!future.valid())
		return std::nullopt;

	TaskHandle handle;
	while (!future.is_ready() && myReadyQueue.try_dequeue(handle))
	{
		internalCall(handle);
		purgeDeletionQueue();
	}

	return std::make_optional(future.get());
}

template <typename... Params, typename... Args, typename F, typename C, typename ArgsTuple, typename ParamsTuple, typename R>
requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
std::pair<TaskHandle, Future<R>> TaskExecutor::createTask(F&& callable, Args&&... args)
{
	ZoneScopedN("TaskExecutor::createTask");

	auto handle = ourTaskPool.allocate();
	Task* taskPtr = ourTaskPool.getPointer(handle);
	std::construct_at(taskPtr, std::forward<F>(callable), ParamsTuple{}, std::forward<Args>(args)...);

	return std::make_pair(handle, Future<R>(std::static_pointer_cast<typename Future<R>::FutureState>(taskPtr->state())));
}

template <typename... TaskParams>
void TaskExecutor::internalCall(TaskHandle handle, TaskParams&&... params)
{
	ZoneScopedN("TaskExecutor::internalCall");

	Task& task = *handleToTaskPtr(handle);

	assert(task.state()->latch.load(std::memory_order_relaxed) == 1);
	
	task(params...);
	scheduleAdjacent(task);
	myDeletionQueue.enqueue(handle);
}

template <typename... TaskParams>
void TaskExecutor::internalCall(ProducerToken& readyProducerToken, TaskHandle handle, TaskParams&&... params)
{
	ZoneScopedN("TaskExecutor::internalCall");

	Task& task = *handleToTaskPtr(handle);

	assert(task.state()->latch.load(std::memory_order_relaxed) == 1);
	
	task(params...);
	scheduleAdjacent(readyProducerToken, task);
	myDeletionQueue.enqueue(handle);
}

template <typename... TaskParams>
void TaskExecutor::call(TaskHandle handle, TaskParams&&... params)
{
	internalCall(handle, params...);
}
