#include "concurrency-utils.h"

Task::Task(Task&& other) noexcept
{
	*this = std::forward<Task>(other);
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

	myState = std::exchange(other.myState, {});
	myInvokeFcnPtr = std::exchange(other.myInvokeFcnPtr, nullptr);
	myCopyFcnPtr = std::exchange(other.myCopyFcnPtr, nullptr);
	myDeleteFcnPtr = std::exchange(other.myDeleteFcnPtr, nullptr);

	if (myCopyFcnPtr)
		myCopyFcnPtr(
			myCallableMemory.data(),
			other.myCallableMemory.data(),
			myArgsMemory.data(),
			other.myArgsMemory.data());

	if (myDeleteFcnPtr)
		myDeleteFcnPtr(other.myCallableMemory.data(), other.myArgsMemory.data());

	return *this;
}

Task::operator bool() const noexcept
{
	return myInvokeFcnPtr && myCopyFcnPtr && myDeleteFcnPtr && myState;
}

bool Task::operator!() const noexcept
{
	return !static_cast<bool>(*this);
}

void Task::dependsOn(const Task& other) const
{
	myState->dependents.push_back(other.state().get());
}

TaskExecutor::TaskExecutor(uint32_t threadCount)
	: mySignal(threadCount)
{
	ZoneScopedN("TaskExecutor()");

	assertf(threadCount > 0, "Thread count must be nonzero");

	myThreads.reserve(threadCount);
	for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
		myThreads.emplace_back(&TaskExecutor::internalThreadMain, this);
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

void TaskExecutor::submit(TaskGraph&& graph)
{
	ZoneScopedN("TaskExecutor::submit");

	// TODO: traverse graph to properly initialize latches

	auto taskIt = graph.tasks().begin();
	while (taskIt != graph.tasks().end())
	{
		if (taskIt->state()->dependents.empty())
		{
			taskIt->state()->latch.emplace(1);
			myReadyQueue.enqueue(std::move(*taskIt));
			taskIt = graph.tasks().erase(taskIt);
			continue;
		}

		taskIt++;
	}

	// if (!graph.tasks().empty())
	// 	myWaitingQueue.enqueue(std::forward<TaskGraph>(graph));

	mySignal.release();
}

void TaskExecutor::join()
{
	ZoneScopedN("TaskExecutor::join");

	internalProcessQueues();
}

void TaskExecutor::internalProcessQueues()
{
	Task task;
	while (myReadyQueue.try_dequeue(task))
		task();
}

void TaskExecutor::internalProcessQueues(
	moodycamel::ProducerToken& readyProducerToken,
	moodycamel::ConsumerToken& readyConsumerToken,
	moodycamel::ProducerToken& waitingProducerToken,
	moodycamel::ConsumerToken& waitingConsumerToken)
{
	Task task;
	while (myReadyQueue.try_dequeue(readyConsumerToken, task))
		task();
}

void TaskExecutor::internalThreadMain(/*std::stop_token& stopToken*/)
{
	moodycamel::ProducerToken readyProducerToken(myReadyQueue);
	moodycamel::ConsumerToken readyConsumerToken(myReadyQueue);
	moodycamel::ProducerToken waitingProducerToken(myWaitingQueue);
	moodycamel::ConsumerToken waitingConsumerToken(myWaitingQueue);

	//while (!stopToken.stop_requested())
	while (!myStopSource.load(std::memory_order_relaxed))
	{
		internalProcessQueues(
			readyProducerToken,
			readyConsumerToken,
			waitingProducerToken,
			waitingConsumerToken);

		//auto lock = std::shared_lock(myMutex);

		//mySignal.wait(lock, stopToken, [&queue = myReadyQueue](){ return !queue.empty(); });
		//mySignal.wait(lock, [&stopSource = myStopSource, &queue = myReadyQueue](){ return stopSource.load(std::memory_order_relaxed) || queue.size_approx(); });
		mySignal.acquire();
	}
}
