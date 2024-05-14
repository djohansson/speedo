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

	if (!future.valid())
		return std::nullopt;

	TaskHandle handle;
	while (!future.is_ready() && myReadyQueue.try_dequeue(handle))
	{
		InternalCall(handle);
		PurgeDeletionQueue();
	}

	return std::make_optional(future.get());
}

template <typename... Params, typename... Args, typename F, typename C, typename ArgsTuple, typename ParamsTuple, typename R>
requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
TaskCreateInfo<R> TaskExecutor::CreateTask(F&& callable, Args&&... args)
{
	ZoneScopedN("TaskExecutor::CreateTask");

	auto handle = gTaskPool.allocate();
	Task* taskPtr = gTaskPool.getPointer(handle);
	std::construct_at(taskPtr, std::forward<F>(callable), ParamsTuple{}, std::forward<Args>(args)...);

	return std::make_pair(handle, Future<R>(std::static_pointer_cast<typename Future<R>::FutureState>(taskPtr->state())));
}

template <typename... TaskParams>
void TaskExecutor::InternalCall(TaskHandle handle, TaskParams&&... params)
{
	ZoneScopedN("TaskExecutor::internalCall");

	Task& task = *HandleToTaskPtr(handle);

	ASSERT(task.state()->latch.load(std::memory_order_relaxed) == 1);
	
	task(params...);
	ScheduleAdjacent(task);
	myDeletionQueue.enqueue(handle);
}

template <typename... TaskParams>
void TaskExecutor::InternalCall(ProducerToken& readyProducerToken, TaskHandle handle, TaskParams&&... params)
{
	ZoneScopedN("TaskExecutor::internalCall");

	Task& task = *HandleToTaskPtr(handle);

	ASSERT(task.state()->latch.load(std::memory_order_relaxed) == 1);
	
	task(params...);
	ScheduleAdjacent(readyProducerToken, task);
	myDeletionQueue.enqueue(handle);
}

template <typename... TaskParams>
void TaskExecutor::Call(TaskHandle handle, TaskParams&&... params)
{
	InternalCall(handle, params...);
}

