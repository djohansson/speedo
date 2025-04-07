#include "taskexecutor.h"
#include "assert.h"//NOLINT(modernize-deprecated-headers)

#include <format>
#include <iostream>
#include <shared_mutex>

// #if !defined(__cpp_lib_atomic_shared_ptr) || __cpp_lib_atomic_shared_ptr < 201711L
// static_assert(false, "std::atomic<std::shared_ptr> is not supported by the standard library!");
// #endif

#ifdef _WIN32
#	include <windows.h>
#else
#	include <pthread.h>
#endif
namespace taskexecutor
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

}

TaskExecutor::TaskExecutor(uint32_t threadCount)
{
	using namespace taskexecutor;

	ZoneScopedN("TaskExecutor()");

	ASSERTF(threadCount > 0, "Thread count must be nonzero");

	myThreads.reserve(threadCount);

	for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
		myThreads.emplace_back(std::jthread(std::bind_front(&TaskExecutor::InternalThreadMain, this), threadIt));
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
	ASSERT(task);
	auto& state = *std::atomic_load(&task.InternalState());
	
	if (state.Latch().load(std::memory_order_relaxed) == 0)
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
		ASSERT(adjacent);
		auto& adjacentState = *std::atomic_load(&adjacent.InternalState());
		ASSERTF(adjacentState.Latch(), "Latch needs to have been constructed!");

		if (adjacentState.Latch().fetch_sub(1, std::memory_order_relaxed) - 1 == 1)
			Submit({&adjacentHandle, 1}, !adjacentState.continuation);
	}
}

void TaskExecutor::InternalProcessReadyQueue()
{
	ZoneScopedN("TaskExecutor::InternalProcessReadyQueue");

	TaskHandle handle;
	while (myReadyQueue.try_dequeue(handle))
	{
		InternalCall(handle);
		InternalPurgeDeletionQueue();
	}
}

void TaskExecutor::InternalPurgeDeletionQueue()
{
	ZoneScopedN("TaskExecutor::InternalPurgeDeletionQueue");

	static thread_local std::vector<TaskHandle> nextDeletionQueue;
	nextDeletionQueue.reserve(std::max(myDeletionQueue.size_approx(), nextDeletionQueue.size()));
	nextDeletionQueue.clear();

	TaskHandle handle;
	while (myDeletionQueue.try_dequeue(handle))
		if (!InternalTryDelete(handle))
			nextDeletionQueue.emplace_back(handle);

	myDeletionQueue.enqueue_bulk(nextDeletionQueue.begin(), nextDeletionQueue.size());
}

void TaskExecutor::JoinOne()
{
	ZoneScopedN("TaskExecutor::JoinOne");

	TaskHandle handle;
	if (myReadyQueue.try_dequeue(handle))
	{
		InternalCall(handle);
		InternalPurgeDeletionQueue();
	}
}

void TaskExecutor::InternalThreadMain(uint32_t threadIndex)
{
	using namespace taskexecutor;
	
	SetThreadName(myThreads[threadIndex], std::format("TaskThread {}", threadIndex).c_str());
			
	std::shared_lock lock(myMutex);
	auto stopToken = myStopSource.get_token();

	while (!stopToken.stop_requested())
		if (myCV.wait(lock, stopToken, [this]{ return myReadyQueue.size_approx() > 0; }))
			InternalProcessReadyQueue();
}

void TaskExecutor::InternalSubmit(std::span<const TaskHandle> handles)
{
	ENSURE(myReadyQueue.enqueue_bulk(handles.data(), handles.size()));
}

void TaskExecutor::Submit(std::span<const TaskHandle> handles, bool wakeThreads)
{
	ZoneScopedN("TaskExecutor::Submit");

	InternalSubmit(handles);
	
	if (auto count = handles.size(); wakeThreads && count > 0)
	{
		if (count >= myThreads.size())
			myCV.notify_all();
		else while (count--)
			myCV.notify_one();
	}
}