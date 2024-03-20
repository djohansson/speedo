template <typename ReturnType>
std::optional<typename Future<ReturnType>::value_t> TaskExecutor::join(Future<ReturnType>&& future)
{
	ZoneScopedN("TaskExecutor::join");

	auto retval = processReadyQueue(std::forward<Future<ReturnType>>(future));
	
	purgeDeletionQueue();

	return retval;
}

template <typename ReturnType>
std::optional<typename Future<ReturnType>::value_t> TaskExecutor::processReadyQueue(Future<ReturnType>&& future)
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	if (!future.valid())
		return std::nullopt;

	TaskHandle handle;
	while (!future.is_ready() && myReadyQueue.try_dequeue(handle))
		internalCall(handle);

	return std::make_optional(future.get());
}

template <typename F, typename CallableType, typename... Args, typename ReturnType>
requires std::invocable<F&, Args...>
std::pair<TaskHandle, Future<ReturnType>> TaskExecutor::createTask(F&& f, Args&&... args)
{
	ZoneScopedN("TaskExecutor::createTask");

	uint32_t poolIndex = ourTaskPool.allocate();
	Task* taskPtr = ourTaskPool.getPointer(poolIndex);
	std::construct_at(taskPtr, Task(std::forward<F>(f), std::forward<Args>(args)...));

	return std::make_pair(
		TaskHandle{poolIndex},
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
}

template <typename T, typename... Ts>
void TaskExecutor::call(T&& first, Ts&&... rest)
{
	ZoneScopedN("TaskExecutor::call");

	internalCall(std::forward<TaskHandle>(first));

	if constexpr (sizeof...(rest) > 0)
		internalCall(std::forward<Ts>(rest)...);
}
