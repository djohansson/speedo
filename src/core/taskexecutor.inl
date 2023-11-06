template <typename ReturnType>
std::optional<typename Future<ReturnType>::value_t> TaskExecutor::join(Future<ReturnType>&& future)
{
	ZoneScopedN("TaskExecutor::join");

	auto result = processReadyQueue(std::forward<Future<ReturnType>>(future));

	return result;
}

template <typename ReturnType>
std::optional<typename Future<ReturnType>::value_t>
TaskExecutor::processReadyQueue(Future<ReturnType>&& future)
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	if (!future.valid())
		return std::nullopt;

	Task task;
	while (!future.is_ready() && myReadyQueue.try_dequeue(task))
	{
		task();
		scheduleAdjacent(task);
	}

	return std::make_optional(future.get());
}
