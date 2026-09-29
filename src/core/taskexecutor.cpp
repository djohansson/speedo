#include <core/taskexecutor.h>
#include <core/assert.h>//NOLINT(modernize-deprecated-headers)

#include <atomic>
#include <shared_mutex>

// #if !defined(__cpp_lib_atomic_shared_ptr) || __cpp_lib_atomic_shared_ptr < 201711L
// static_assert(false, "std::atomic<std::shared_ptr> is not supported by the standard library!");
// #endif

#ifdef _WIN32
#	include <windows.h>
#else
#	include <pthread.h>
#endif

namespace core
{

namespace detail
{

#ifdef _WIN32

const DWORD MS_VC_EXCEPTION = 0x406D1388;

#pragma pack(push, 8)
typedef struct _THREADNAME_INFO
{
	DWORD dwType;	  // Must be 0x1000.
	LPCSTR szName;	  // Pointer to name (in user addr space).
	DWORD dwThreadID; // Thread ID (-1=caller thread).
	DWORD dwFlags;	  // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)

void SetThreadName(uint32_t dwThreadID, const char* threadName)
{
	THREADNAME_INFO info;
	info.dwType = 0x1000;
	info.szName = threadName;
	info.dwThreadID = dwThreadID;
	info.dwFlags = 0;

	__try
	{
		RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{}
}

void SetThreadName(const char* threadName)
{
	SetThreadName(GetCurrentThreadId(), threadName);
}

void SetThreadName(std::jthread& thread, const char* threadName)
{
	SetThreadName(::GetThreadId(static_cast<HANDLE>(thread.native_handle())), threadName);
}

#elif defined(__APPLE__)

void SetThreadName(const char* threadName)
{
	pthread_setname_np(threadName);
}

void SetThreadName(std::jthread& /*thread*/, const char* threadName)
{
	SetThreadName(threadName);
}

#else

void SetThreadName(std::jthread& thread, const char* threadName)
{
	auto handle = thread.native_handle();
	pthread_setname_np(handle, threadName);
}

#endif

enum TaskExecutorState : uint8_t
{
	kTaskExecutorInitializing = 0,
	kTaskExecutorRunning = 1,
};
static std::atomic<TaskExecutorState> gTaskExecutorState = kTaskExecutorInitializing;

} // namespace detail

TaskExecutor::TaskExecutor(uint32_t threadCount)
{
	using namespace detail;

	ZoneScopedN("TaskExecutor()");

	ENSUREF(threadCount > 0, "Thread count must be nonzero");

	myThreads.reserve(threadCount);

	for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
		myThreads.emplace_back(std::bind_front(&TaskExecutor::InternalThreadMain, this), threadIt);

	gTaskExecutorState.store(kTaskExecutorRunning, std::memory_order_release);
	gTaskExecutorState.notify_all();
}

TaskExecutor::~TaskExecutor()
{
	ZoneScopedN("~TaskExecutor()");

	ASSERT(myReadyQueue.size_approx() == 0);
	ASSERT(myDeletionQueue.size_approx() == 0);

	myStopSource.request_stop();

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
	
	SetThreadName(myThreads[threadIndex], std::format("TaskThread {}", threadIndex).c_str());
			
	std::shared_lock lock(myMutex);
	auto stopToken = myStopSource.get_token();

	while (!stopToken.stop_requested())
		if (myCV.wait(lock, stopToken,
			[this]{ return std::atomic_ref(myReadyQueueSize).load(std::memory_order_acquire) > 0; }))
			InternalProcessReadyQueue();
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
		if (count >= myThreads.size())
			myCV.notify_all();
		else while (count-- > 0)
			myCV.notify_one();
	}
}

} // namespace core
