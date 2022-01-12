#include "concurrency-utils.h"

//#include <xxhash.h>

Task::Task(Task&& other) noexcept
{
	*this = std::forward<Task>(other);
}

Task::Task(const Task& other) noexcept
{
	*this = other;
}

Task::~Task()
{
	if (myDeleteFcnPtr)
		myDeleteFcnPtr(myCallableMemory.data(), myArgsMemory.data());
}

Task& Task::operator=(Task&& other) noexcept
{
	if (myDeleteFcnPtr)
		myDeleteFcnPtr(myCallableMemory.data(), myArgsMemory.data());

	myReturnState = std::exchange(other.myReturnState, {});
	myInvokeFcnPtr = std::exchange(other.myInvokeFcnPtr, nullptr);
	myCopyFcnPtr = std::exchange(other.myCopyFcnPtr, nullptr);
	myDeleteFcnPtr = std::exchange(other.myDeleteFcnPtr, nullptr);

	if (myCopyFcnPtr)
		myCopyFcnPtr(myCallableMemory.data(), other.myCallableMemory.data(), myArgsMemory.data(), other.myArgsMemory.data());

	if (myDeleteFcnPtr)
		myDeleteFcnPtr(other.myCallableMemory.data(), other.myArgsMemory.data());

	return *this;
}

Task& Task::operator=(const Task& other) noexcept
{
	if (myDeleteFcnPtr)
		myDeleteFcnPtr(myCallableMemory.data(), myArgsMemory.data());

	myReturnState = {};
	myInvokeFcnPtr = other.myInvokeFcnPtr;
	myCopyFcnPtr = other.myCopyFcnPtr;
	myDeleteFcnPtr = other.myDeleteFcnPtr;

	if (myCopyFcnPtr)
		myCopyFcnPtr(myCallableMemory.data(), other.myCallableMemory.data(), myArgsMemory.data(), other.myArgsMemory.data());

	return *this;
}

Task::operator bool() const noexcept
{
	return myInvokeFcnPtr != nullptr;
}

bool Task::operator!() const noexcept
{
	return !static_cast<bool>(*this);
}

template <>
void Task::operator()()
{
	assert(myInvokeFcnPtr);

	myInvokeFcnPtr(myCallableMemory.data(), myArgsMemory.data(), myReturnState.get());
}

// uint64_t Task::hash() const noexcept
// {
// 	thread_local std::unique_ptr<XXH3_state_t, XXH_errorcode (*)(XXH3_state_t*)> threadXXHState =
// 	{
// 		XXH3_createState(),
// 		XXH3_freeState
// 	};

// 	auto result = XXH3_64bits_reset(threadXXHState.get());
// 	assert(result != XXH_ERROR);

// 	result = XXH3_64bits_update(threadXXHState.get(), myCallableMemory.data(), sizeof(myCallableMemory));
// 	assert(result != XXH_ERROR);

// 	result = XXH3_64bits_update(threadXXHState.get(), myArgsMemory.data(), sizeof(myArgsMemory));
// 	assert(result != XXH_ERROR);

// 	return XXH3_64bits_digest(threadXXHState.get());
// }

TaskExecutor::TaskExecutor(uint32_t threadCount)
	: mySignal(threadCount)
{
	ZoneScopedN("TaskExecutor()");

	assertf(threadCount > 0, "Thread count must be nonzero");

	auto threadMain = [this] {
		//auto stopToken = myStopSource.get_token();
		internalThreadMain(/*stopToken*/);
	};

	myThreads.reserve(threadCount);
	for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
		myThreads.emplace_back(threadMain);
}

TaskExecutor::~TaskExecutor()
{
	ZoneScopedN("~TaskExecutor()");

	//myStopSource.request_stop();
	myStopSource.store(true, std::memory_order_relaxed);
	//mySignal.notify_all();
	mySignal.release(mySignal.max());

	// workaround for the following problem in msvc implementation of jthread in <mutex>:
	// TRANSITION, ABI: Due to the unsynchronized delivery of notify_all by _Stoken,
	// this implementation cannot tolerate *this destruction while an interruptible wait
	// is outstanding. A future ABI should store both the internal CV and internal mutex
	// in the reference counted block to allow this.
	{
		ZoneScopedN("~TaskExecutor()::join");

		for (auto& thread : myThreads)
			thread.join();
	}
}

void TaskExecutor::join()
{
	ZoneScopedN("TaskExecutor::join");

	internalProcessQueue();
}

void TaskExecutor::internalProcessQueue()
{
	Task task;
	while (myQueue.try_dequeue(task))
		task();
}

void TaskExecutor::internalThreadMain(/*std::stop_token& stopToken*/)
{
	//while (!stopToken.stop_requested())
	while (!myStopSource.load(std::memory_order_relaxed))
	{
		internalProcessQueue();

		//auto lock = std::shared_lock(myMutex);

		//mySignal.wait(lock, stopToken, [&queue = myQueue](){ return !queue.empty(); });
		//mySignal.wait(lock, [&stopSource = myStopSource, &queue = myQueue](){ return stopSource.load(std::memory_order_relaxed) || queue.size_approx(); });
		mySignal.acquire();
	}
}
