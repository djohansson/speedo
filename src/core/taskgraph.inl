template <typename F, typename CallableType, typename... Args, typename ReturnType>
requires std::invocable<F&, Args...> std::tuple<TaskGraph::TaskNodeHandle, Future<ReturnType>>
TaskGraph::createTask(F&& f, Args&&... args)
{
	ZoneScopedN("TaskGraph::createTask");

	const auto& taskNode = myNodes.emplace_back(std::make_tuple(
		Task(std::forward<F>(f), std::forward<Args>(args)...), TaskNodeHandleVector{}, 0));

	const auto& [task, adjacencies, dependencies] = taskNode;

	return std::make_tuple(
		&taskNode - &myNodes[0],
		Future<ReturnType>(
			std::static_pointer_cast<typename Future<ReturnType>::FutureState>(task.state())));
}
