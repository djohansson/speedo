#pragma once

#include "profiling.h"
#include "utils.h"

#include <array>
#include <atomic>
#include <concepts>
//#include <condition_variable>
#include <cstdint>
#include <memory>
#include <optional>
#include <semaphore>
//#include <shared_mutex>
//#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <concurrentqueue.h>

template <typename T, typename AtomicT = std::atomic<T>>
class CopyableAtomic : public AtomicT
{
public:
	using value_t = T;
	using atomic_t = AtomicT;
	using atomic_t::store;

	constexpr CopyableAtomic() noexcept = default;
	constexpr CopyableAtomic(value_t val) noexcept;
	CopyableAtomic(const CopyableAtomic& other) noexcept;

	CopyableAtomic& operator=(const CopyableAtomic& other) noexcept;
};

template <
	typename ValueT = uint32_t,
	uint32_t Aligmnent =
#if __cpp_lib_hardware_interference_size >= 201603
		std::hardware_destructive_interference_size>
#else
		128>
#endif
class alignas(Aligmnent) UpgradableSharedMutex
{
	using value_t = ValueT;
	enum : value_t
	{
		Reader = 4,
		Upgraded = 2,
		Writer = 1,
		None = 0
	};

#if __cpp_lib_atomic_ref >= 201806
	value_t myBits = 0;
	auto internalAtomicRef() noexcept;
#else
	CopyableAtomic<value_t> myAtomic;
	auto& internalAtomicRef() noexcept;
#endif

	template <typename Func>
	void internalAquireLock(Func lockFn) noexcept;

public:
	// Lockable Concept
	void lock() noexcept;

	// Writer is responsible for clearing up both the Upgraded and Writer bits.
	void unlock() noexcept;

	// SharedLockable Concept
	void lock_shared() noexcept;
	void unlock_shared() noexcept;

	// Downgrade the lock from writer status to reader status.
	void unlock_and_lock_shared() noexcept;

	// UpgradeLockable Concept
	void lock_upgrade() noexcept;
	void unlock_upgrade() noexcept;

	// unlock upgrade and try to acquire write lock
	void unlock_upgrade_and_lock() noexcept;

	// unlock upgrade and read lock atomically
	void unlock_upgrade_and_lock_shared() noexcept;

	// write unlock and upgrade lock atomically
	void unlock_and_lock_upgrade() noexcept;

	// Attempt to acquire writer permission. Return false if we didn't get it.
	template <value_t Expected = None>
	std::tuple<bool, value_t> try_lock() noexcept;

	// Try to get reader permission on the lock. This can fail if we
	// find out someone is a writer or upgrader.
	// Setting the Upgraded bit would allow a writer-to-be to indicate
	// its intention to write and block any new readers while waiting
	// for existing readers to finish and release their read locks. This
	// helps avoid starving writers (promoted from upgraders).
	std::tuple<bool, value_t> try_lock_shared() noexcept;

	// try to acquire an upgradable lock.
	std::tuple<bool, value_t> try_lock_upgrade() noexcept;
};

class Task;

struct TaskState
{
	std::optional<std::atomic_uint32_t> latch;
	std::vector<std::reference_wrapper<Task>> adjacencies;
};

// todo: dynamic memory allocation if larger tasks are created
class Task : public Noncopyable
{
public:
	constexpr Task() noexcept = default;
	template <
		typename F,
		typename CallableType = std::decay_t<F>,
		typename... Args,
		typename ArgsTuple = std::tuple<Args...>,
		typename ReturnType = std::invoke_result_t<CallableType, Args...>>
	requires std::invocable<F&, Args...>
	Task(F&& f, Args&&... args);
	Task(Task&& other) noexcept;
	~Task();

	operator bool() const noexcept;
	bool operator!() const noexcept;
	Task& operator=(Task&& other) noexcept;
	template <typename... Args>
	void operator()(Args&&... args);

private:
	friend class TaskGraph;
	friend class TaskExecutor;

	// template <class Tuple, size_t... I>
	// constexpr decltype(auto) apply(Tuple&& t, std::index_sequence<I...>)
	// {
	// 	return (*this)(std::get<I>(std::forward<Tuple>(t))...);
	// }

	auto& state() noexcept { return myState; }
	const auto& state() const noexcept { return myState; }

	static constexpr size_t kMaxCallableSizeBytes = 56;
	static constexpr size_t kMaxArgsSizeBytes = 32;

	alignas(intptr_t) AlignedArray<kMaxCallableSizeBytes> myCallableMemory = {};
	alignas(intptr_t) AlignedArray<kMaxArgsSizeBytes> myArgsMemory = {};
	void (*myInvokeFcnPtr)(const void*, const void*, void*) = nullptr;
	void (*myCopyFcnPtr)(void*, const void*, void*, const void*) = nullptr;
	void (*myDeleteFcnPtr)(void*, void*) = nullptr;
	std::shared_ptr<TaskState> myState = {};
};

//char (*__kaboom)[sizeof(Task)] = 1;

template <typename T>
class Future : public Noncopyable
{
public:
	using value_t = std::conditional_t<std::is_void_v<T>, std::nullptr_t, T>;

	constexpr Future() noexcept = default;
	Future(Future&& other) noexcept;

	Future& operator=(Future&& other) noexcept;

	value_t get();
	bool is_ready() const noexcept;
	bool valid() const noexcept;
	void wait() const;

	// template <
	// 	typename F,
	// 	typename CallableType = std::decay_t<F>,
	// 	typename... Args,
	// 	typename ReturnType = std::invoke_result_t<CallableType, Args...>>
	// requires std::invocable<F&, Args...> Future<ReturnType> then(F&& f);

private:
	friend class Task;
	friend class TaskGraph;

	struct FutureState : TaskState
	{
		value_t value;
	};

	Future(std::shared_ptr<FutureState>&& state) noexcept;

	std::shared_ptr<FutureState> myState;
};

class TaskGraph : public Noncopyable
{
public:
	using TaskNodeHandle = size_t;

	constexpr TaskGraph() noexcept = default;
	TaskGraph(TaskGraph&& other) noexcept;

	TaskGraph& operator=(TaskGraph&& other) noexcept;

	template <
		typename F,
		typename CallableType = std::decay_t<F>,
		typename... Args,
		typename ReturnType = std::invoke_result_t<CallableType, Args...>>
	requires std::invocable<F&, Args...> std::tuple<TaskNodeHandle, Future<ReturnType>>
	createTask(F&& f, Args&&... args);

	// a happens before b
	void addDependency(TaskNodeHandle a, TaskNodeHandle b);

private:
	friend class TaskExecutor;
	
	using TaskNodeHandleVector = std::vector<TaskNodeHandle>;
	using TaskNode = std::tuple<Task, TaskNodeHandleVector, size_t>; // task, adjacencies, dependencies
	using TaskNodeVec = std::vector<TaskNode>;

	void depthFirstSearch(
		size_t v,
		std::vector<bool>& discovered,
		std::vector<size_t>& departure,
		std::vector<size_t>& sorted,
		size_t& time) const;

	void finalize(); // will invalidate all task handles

	TaskNodeVec myNodes;
};

class TaskExecutor : public Noncopyable
{
public:
	TaskExecutor(uint32_t threadCount);
	~TaskExecutor();

	void submit(TaskGraph&& graph); // will invalidate all task handles
	void join();

	template <typename ReturnType>
	std::optional<typename Future<ReturnType>::value_t> join(Future<ReturnType>&& future);

private:

	void scheduleAdjacent(const Task& task);

	template <typename ReturnType>
	std::optional<typename Future<ReturnType>::value_t>
	processReadyQueue(Future<ReturnType>&& future);
	void processReadyQueue();
	void processReadyQueue(
		moodycamel::ProducerToken& readyProducerToken,
		moodycamel::ConsumerToken& readyConsumerToken
	);

	void removeFinishedGraphs();
	void removeFinishedGraphs(
		moodycamel::ProducerToken& waitingProducerToken,
		moodycamel::ConsumerToken& waitingConsumerToken);

	void threadMain(/*std::stop_token& stopToken*/);

	//std::shared_mutex myMutex;
	//std::vector<std::jthread> myThreads;
	std::vector<std::thread> myThreads;
	//std::condition_variable_any mySignal;
	//std::stop_source myStopSource;
	std::counting_semaphore<> mySignal;
	std::atomic_bool myStopSource;
	moodycamel::ConcurrentQueue<Task> myReadyQueue;
	moodycamel::ConcurrentQueue<TaskGraph> myWaitingQueue;
};

#include "concurrency-utils.inl"
