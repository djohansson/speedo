#include <core/profiling.h>

#include <algorithm>
#include <atomic>
#include <vector>

namespace core
{

template <typename Fn>
void TaskExecutor::ForEach(size_t count, Fn&& body)
{
	ZoneScopedN("TaskExecutor::ForEach");

	if (count == 0)
		return;

	std::atomic_size_t next = 0;
	auto work = [&next, &body, count]
	{
		for (size_t index = next++; index < count; index = next++)
			body(index);
	};

	auto workerCount = std::min<size_t>(std::max<size_t>(myThreads.size(), 1), count);
	std::vector<TaskHandle> handles;
	std::vector<Future<void>> futures;
	handles.reserve(workerCount);
	futures.reserve(workerCount);
	for (size_t workerIt = 0; workerIt < workerCount; workerIt++)
	{
		auto [handle, future] = CreateTask([&work] { work(); });
		handles.push_back(handle);
		futures.push_back(std::move(future));
	}
	Submit(handles);
	for (auto& future : futures)
		(void)Join(std::move(future));
}

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

	while (!future.IsReady() && InternalTryCallOne());

	return std::make_optional(future.Get());
}

template <typename... Params>
void TaskExecutor::InternalCall(TaskHandle handle, Params&&... params)
{
	ZoneScopedN("TaskExecutor::InternalCall");

	Task& task = *core::detail::InternalHandleToPtr(handle);
	auto& state = *std::atomic_load(&task.InternalState());
	
	ENSURE(std::atomic_ref(state.latch).load(std::memory_order_relaxed) == 1);

	task(params...);
	InternalScheduleAdjacent(task);
	myDeletionQueue.enqueue(handle);
}

} // namespace core
