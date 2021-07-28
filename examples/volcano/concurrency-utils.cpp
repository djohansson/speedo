#include "concurrency-utils.h"

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

void Task::invoke()
{
	assert(myInvokeFcnPtr);

	myInvokeFcnPtr(myCallableMemory.data(), myArgsMemory.data(), myReturnState.get());
}

ThreadPool::ThreadPool(uint32_t threadCount)
	: mySignal(threadCount)
{
	ZoneScopedN("ThreadPool()");

	assertf(threadCount > 0, "Thread count must be nonzero");

	auto threadMain = [this] {
		//auto stopToken = myStopSource.get_token();
		internalThreadMain(/*stopToken*/);
	};

	myThreads.reserve(threadCount);
	for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
		myThreads.emplace_back(threadMain);
}

ThreadPool::~ThreadPool()
{
	ZoneScopedN("~ThreadPool()");

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
		ZoneScopedN("~ThreadPool()::join");

		for (auto& thread : myThreads)
			thread.join();
	}
}

void ThreadPool::join()
{
	ZoneScopedN("ThreadPool::join");

	internalProcessQueue();
}

void ThreadPool::internalProcessQueue()
{
	Task task;
	while (myQueue.try_dequeue(task))
		task.invoke();
}

void ThreadPool::internalThreadMain(/*std::stop_token& stopToken*/)
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
