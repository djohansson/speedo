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

	TaskHandle handle;
	while (!future.is_ready() && myReadyQueue.try_dequeue(handle))
	{
		Task& task = handleToTaskRef(handle);
		task();
		scheduleAdjacent(task);
		myDeletionQueue.enqueue(handle);
	}

	return std::make_optional(future.get());
}

template <typename F, typename CallableType, typename... Args, typename ReturnType>
requires std::invocable<F&, Args...>
std::pair<TaskHandle, Future<ReturnType>> TaskExecutor::createTask(F&& f, Args&&... args)
{
	ZoneScopedN("TaskExecutor::createTask");

	ourTaskPoolMutex.lock();
	Task* taskPtr = ourTaskPool.allocate();
	std::construct_at(taskPtr, Task(std::forward<F>(f), std::forward<Args>(args)...));
	ourTaskPoolMutex.unlock();

	return std::make_pair(
		taskPtr,
		Future<ReturnType>(
			std::static_pointer_cast<typename Future<ReturnType>::FutureState>(taskPtr->state())));
}

template <typename T, typename... Ts>
void TaskExecutor::submit(T&& first, Ts&&... rest)
{
	ZoneScopedN("TaskExecutor::submit");

	internalSubmit(std::forward<TaskHandle>(first));

	if constexpr (sizeof...(rest) > 0)
		internalSubmit(std::forward<Ts>(rest)...);

	mySignal.release();
}

template <typename T, typename... Ts>
void TaskExecutor::call(T&& first, Ts&&... rest)
{
	ZoneScopedN("TaskExecutor::call");

	internalCall(std::forward<TaskHandle>(first));

	if constexpr (sizeof...(rest) > 0)
		internalCall(std::forward<Ts>(rest)...);
}
