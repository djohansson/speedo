#include <core/profiling.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace core
{

template <typename Fn>
void TaskExecutor::ForEach(size_t count, Fn&& body)
{
	ZoneScopedN("TaskExecutor::ForEach");

	if (count == 0)
		return;

	// the indices, taken in turn, and how many are done, shared with the worker tasks: one that starts after the loop is
	// done finds no index left and returns without touching body, which is the caller's
	struct Loop
	{
		std::atomic_size_t next = 0;
		std::atomic_size_t done = 0;
	};
	auto loop = std::make_shared<Loop>();
	auto work = [loop, fn = &body, count]
	{
		for (size_t index = loop->next++; index < count; index = loop->next++)
		{
			(*fn)(index);
			if (++loop->done == count)
				loop->done.notify_all();
		}
	};

	// the caller is a worker too
	auto workerCount = std::min<size_t>(std::max<size_t>(myThreads.size(), 1), count) - 1;
	std::vector<TaskHandle> handles;
	handles.reserve(workerCount);
	for (size_t workerIt = 0; workerIt < workerCount; workerIt++)
		handles.push_back(CreateTask([work] { work(); }).handle);
	Submit(handles);

	work();

	// the indices the others took are running: wait for them, taking nothing else from the ready queue meanwhile (see
	// ForEach's comment in the header)
	for (size_t done = loop->done.load(); done < count; done = loop->done.load())
		loop->done.wait(done);
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
