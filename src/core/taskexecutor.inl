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
		std::atomic_size_t workers = 0; // worker tasks made
		size_t count = 0;
		size_t maxWorkers = 0;
		std::remove_reference_t<Fn>* body = nullptr; // Fn is a reference where body is an lvalue
		TaskExecutor* executor = nullptr;

		void Work()
		{
			for (size_t index = next++; index < count; index = next++)
			{
				(*body)(index);
				if (++done == count)
					done.notify_all();
			}
		}

		// a worker, as it starts, recruits the next while indices are left: the threads that free up during the loop join
		// it, and a loop done before its queued worker started leaves that one alone in the ready queue. making one per
		// thread up front, nested loops (a model's textures, each compressing its levels) on a busy executor left one per
		// thread per loop queued, which ran the task pool out on 32 threads
		static void Worker(const std::shared_ptr<Loop>& loop)
		{
			if (loop->next.load(std::memory_order_relaxed) < loop->count &&
				loop->workers.fetch_add(1, std::memory_order_relaxed) < loop->maxWorkers)
			{
				auto handle = CreateTask([loop] { Worker(loop); }).handle;
				loop->executor->Submit({&handle, 1});
			}
			loop->Work();
		}
	};
	auto loop = std::make_shared<Loop>();
	loop->count = count;
	loop->maxWorkers = std::min<size_t>(std::max<size_t>(myThreads.size(), 1), count) - 1; // the caller is a worker too
	loop->body = &body;
	loop->executor = this;

	// as many workers as threads are idle, at least one, which recruits the rest as threads free up (see Loop::Worker)
	auto workerCount = std::min<size_t>(
		std::max<size_t>(myIdleThreadCount.load(std::memory_order_relaxed), 1), loop->maxWorkers);
	loop->workers.store(workerCount, std::memory_order_relaxed);
	std::vector<TaskHandle> handles;
	handles.reserve(workerCount);
	for (size_t workerIt = 0; workerIt < workerCount; workerIt++)
		handles.push_back(CreateTask([loop] { Loop::Worker(loop); }).handle);
	Submit(handles);

	loop->Work();

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

} // namespace core
