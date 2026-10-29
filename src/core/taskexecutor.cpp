#include <core/taskexecutor.h>
#include <core/assert.h>//NOLINT(modernize-deprecated-headers)
#include <core/profiling.h>

#include <atomic>
#include <format>

// #if !defined(__cpp_lib_atomic_shared_ptr) || __cpp_lib_atomic_shared_ptr < 201711L
// static_assert(false, "std::atomic<std::shared_ptr> is not supported by the standard library!");
// #endif

namespace core
{

namespace detail
{

enum TaskExecutorState : uint8_t
{
	kTaskExecutorInitializing = 0,
	kTaskExecutorRunning = 1,
};
static std::atomic<TaskExecutorState> gTaskExecutorState = kTaskExecutorInitializing;

[[nodiscard]] static std::string ThreadName(uint32_t threadIndex)
{
	return std::format("TaskThread {}", threadIndex);
}

} // namespace detail

TaskExecutor::TaskExecutor(uint32_t threadCount)
{
	using namespace detail;

	ZoneScopedN("TaskExecutor()");

	ENSUREF(threadCount > 0, "Thread count must be nonzero");

	myThreads.reserve(threadCount);

	// named, and with stacks as a main thread's (see kThreadStackByteCount)
	for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
		myThreads.emplace_back(
			std_extra::jthread::name_hint(ThreadName(threadIt)),
			std_extra::jthread::stack_size_hint(kThreadStackByteCount),
			std::bind_front(&TaskExecutor::InternalThreadMain, this),
			threadIt);

	gTaskExecutorState.store(kTaskExecutorRunning, std::memory_order_release);
	gTaskExecutorState.notify_all();
}

TaskExecutor::~TaskExecutor()
{
	ZoneScopedN("~TaskExecutor()");

	ASSERT(myReadyQueue.size_approx() == 0);
	ASSERT(myDeletionQueue.size_approx() == 0);

	myStopSource.request_stop();
	myWakeCount.fetch_add(1, std::memory_order_release);
	myWakeCount.notify_all();

	for (auto& thread : myThreads)
		thread.join();
}

bool TaskExecutor::InternalTryDelete(TaskHandle handle)
{
	ZoneScopedN("TaskExecutor::InternalTryDelete");

	Task& task = *core::detail::InternalHandleToPtr(handle);
	ENSURE(task);
	auto& state = *std::atomic_load(&task.InternalState());
	
	if (std::atomic_ref(state.latch).load(std::memory_order_relaxed) == 0)
	{
		std::destroy_at(&task);
		core::detail::InternalFree(handle);
		return true;
	}

	return false;
}

void TaskExecutor::InternalScheduleAdjacent(Task& task)
{
	ZoneScopedN("TaskExecutor::InternalScheduleAdjacent");

	auto& state = *std::atomic_load(&task.InternalState());

	for (auto adjIt = 0; adjIt < state.adjacenciesCount; adjIt++)
	{
		TaskHandle adjacentHandle = state.adjacencies[adjIt];
		Task& adjacent = *core::detail::InternalHandleToPtr(adjacentHandle);
		ENSURE(adjacent);
		auto& adjacentState = *std::atomic_load(&adjacent.InternalState());
		auto adjacentLatch = std::atomic_ref(adjacentState.latch);
		ENSUREF(adjacentLatch, "Latch needs to have been constructed!");

		if (adjacentLatch.fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
			Submit({&adjacentHandle, 1}, !adjacentState.continuation);
	}
}

bool TaskExecutor::InternalTryCallOne()
{
	// count the task as active *before* dequeuing it, so that JoinAll never observes
	// an empty ready queue and zero active tasks while a dequeued task has yet to run.
	auto activeTaskCount = std::atomic_ref(myActiveTaskCount);
	activeTaskCount.fetch_add(1, std::memory_order_acq_rel);

	TaskHandle handle;
	bool dequeued = myReadyQueue.try_dequeue(handle);
	if (dequeued)
	{
		std::atomic_ref(myReadyQueueSize).fetch_sub(1, std::memory_order_acq_rel);
		InternalCall(handle);
		InternalPurgeDeletionQueue();
	}

	activeTaskCount.fetch_sub(1, std::memory_order_acq_rel);

	return dequeued;
}

void TaskExecutor::InternalProcessReadyQueue()
{
	ZoneScopedN("TaskExecutor::InternalProcessReadyQueue");

	while (InternalTryCallOne());
}

void TaskExecutor::InternalPurgeDeletionQueue()
{
	ZoneScopedN("TaskExecutor::InternalPurgeDeletionQueue");

	static thread_local std::vector<TaskHandle> gNextDeletionQueue;
	gNextDeletionQueue.reserve(std::max(myDeletionQueue.size_approx(), gNextDeletionQueue.size()));
	gNextDeletionQueue.clear();

	TaskHandle handle;
	while (myDeletionQueue.try_dequeue(handle))
		if (!InternalTryDelete(handle))
			gNextDeletionQueue.emplace_back(handle);

	myDeletionQueue.enqueue_bulk(gNextDeletionQueue.begin(), gNextDeletionQueue.size());
}

void TaskExecutor::JoinOne()
{
	ZoneScopedN("TaskExecutor::JoinOne");

	InternalTryCallOne();
}

void TaskExecutor::JoinAll()
{
	ZoneScopedN("TaskExecutor::JoinAll");

	while (std::atomic_ref(myReadyQueueSize).load(std::memory_order_acquire) > 0 ||
		   std::atomic_ref(myActiveTaskCount).load(std::memory_order_acquire) > 0)
		if (!InternalTryCallOne())
			std::this_thread::yield(); // remaining work is executing on other threads

	InternalPurgeDeletionQueue();
}

void TaskExecutor::InternalThreadMain(uint32_t threadIndex)
{
	using namespace detail;

	gTaskExecutorState.wait(kTaskExecutorInitializing, std::memory_order_acquire);
	
#if defined(TRACY_ENABLE)
	// the name in Tracy's too (the thread was given it as it started, see TaskExecutor), which it keeps a list of its own
	// for (it doesn't read the os's: only the thread id showed), the task threads in a group of their own
	tracy::SetThreadNameWithHint(ThreadName(threadIndex).c_str(), 1);
#endif
			
	auto stopToken = myStopSource.get_token();

	while (!stopToken.stop_requested())
	{
		// read the wake count before checking for work: Submit bumps it after making work visible, so if the queue
		// looks empty here, any later submit changes the count and the wait below returns right away.
		auto wakeCount = myWakeCount.load(std::memory_order_acquire);
		if (std::atomic_ref(myReadyQueueSize).load(std::memory_order_acquire) > 0)
			InternalProcessReadyQueue();
		else
			myWakeCount.wait(wakeCount, std::memory_order_acquire);
	}
}

void TaskExecutor::InternalSubmit(std::span<const TaskHandle> handles)
{
	ENSURE(myReadyQueue.enqueue_bulk(handles.data(), handles.size()));
	std::atomic_ref(myReadyQueueSize).fetch_add(handles.size(), std::memory_order_release);
}

void TaskExecutor::Submit(std::span<const TaskHandle> handles, bool wakeThreads)
{
	ZoneScopedN("TaskExecutor::Submit");

	InternalSubmit(handles);
	
	if (auto count = handles.size(); wakeThreads && count > 0)
	{
		myWakeCount.fetch_add(1, std::memory_order_release);
		if (count >= myThreads.size())
			myWakeCount.notify_all();
		else while (count-- > 0)
			myWakeCount.notify_one();
	}
}

} // namespace core
