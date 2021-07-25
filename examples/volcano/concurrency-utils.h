#pragma once

#include "utils.h"
#include "profiling.h"

#include <array>
#include <atomic>
//#include <condition_variable>
#include <cstdint>
#include <memory>
#include <optional>
#include <semaphore>
//#include <shared_mutex>
//#include <stop_token>
#include <tuple>
#include <type_traits>
#include <vector>

#include <concurrentqueue.h>

template <typename T>
using ConcurrentQueue = moodycamel::ConcurrentQueue<T>;

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

template <typename ValueT = uint32_t, uint32_t Aligmnent =
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
	using atomic_t = CopyableAtomic<value_t>;
	atomic_t myAtomic;
	auto& internalAtomicRef() noexcept;
#endif

	template <typename Func>
	void internalAquireLock(Func lockFn) noexcept;

public:

	// Lockable Concept
	void lock() noexcept;
	void unlock() noexcept; // Writer is responsible for clearing up both the Upgraded and Writer bits.

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

template <typename T>
struct Future : Noncopyable
{
public:

	using value_t = std::conditional_t<std::is_void_v<T>, std::nullptr_t, T>;
	using state_t = std::tuple<value_t, std::atomic_bool>;

	constexpr Future() = default;

	Future(std::shared_ptr<state_t>&& state) noexcept;	
	Future(Future&& other) noexcept;

	Future& operator=(Future&& other) noexcept;
	
	value_t get();
	bool is_ready() const;
	bool valid() const;
	void wait() const;
	
private:

	std::shared_ptr<state_t> myState;
};

// todo: fork/join
// todo: dynamic memory allocation if larger tasks are created
// wip: return/arg pipes
// wip: continuations
class Task : public Noncopyable
{
	static constexpr size_t kMaxCallableSizeBytes = 56;
	static constexpr size_t kMaxArgsSizeBytes = 24;

public:

	constexpr Task() = default;
	Task(Task&& other) noexcept;
	~Task();

	Task& operator=(Task&& other) noexcept;

	void invoke();

protected:

	template <
		typename F,
		typename CallableType = std::decay_t<F>,
		typename... Args,
		typename ArgsTuple = std::tuple<Args...>,
		typename ReturnType = std::invoke_result_t<CallableType, Args...>,
		typename ReturnState = typename Future<ReturnType>::state_t
	>
	Task(F&& f, Args&&... args);

	alignas(intptr_t) AlignedArray<kMaxCallableSizeBytes> myCallableMemory;
	alignas(intptr_t) AlignedArray<kMaxArgsSizeBytes> myArgsMemory;
	std::shared_ptr<void> myReturnState;
	std::unique_ptr<Task> myContinuation;
	void (*myInvokeFcnPtr)(void*, void*, void*, void*) = nullptr;

private:

	void (*myMoveFcnPtr)(void*, void*, void*, void*) = nullptr;
	void (*myDeleteFcnPtr)(void*, void*) = nullptr;
};

template <typename F, typename... Args>
class TypedTask : public Task
{
public:

	using CallableType = std::decay_t<F>;
	using ArgsTuple = std::tuple<Args...>;
	using ReturnType = std::invoke_result_t<CallableType, Args...>;
	using ReturnState = typename Future<ReturnType>::state_t;

	TypedTask(F&& f, Args&&... args);

	Future<ReturnType> getFuture();

	void invoke(Args&&... args);
	
	template <
		typename CF,
		typename CCallableType = std::decay_t<CF>,
		typename... CArgs,
		typename CArgsTuple = std::tuple<CArgs...>,
		typename CReturnType = std::invoke_result_t<CCallableType, CArgs...>,
		typename CReturnState = typename Future<CReturnType>::state_t>
	TypedTask<CF, CArgs...>& then(CF&& f, CArgs&&... args);
};

// todo: wait_all/wait_any instead of processQueue/processQueueUntil
class ThreadPool : public Noncopyable
{
public:

	ThreadPool(uint32_t threadCount);
	~ThreadPool();

	template <
		typename F,
		typename CallableType = std::decay_t<F>,
		typename... Args,
		typename ReturnType = std::invoke_result_t<CallableType, Args...>
	>
	Future<ReturnType> submit(TypedTask<F, Args...>&& task);

	void processQueue();

	template <typename ReturnType>
	std::optional<typename Future<ReturnType>::value_t> processQueueUntil(Future<ReturnType>&& future);

private:

	void internalProcessQueue();

	template <typename ReturnType>
	std::optional<typename Future<ReturnType>::value_t> internalProcessQueue(Future<ReturnType>&& future);

	void internalThreadMain(/*std::stop_token& stopToken*/);

	//std::shared_mutex myMutex;
	//std::vector<std::jthread> myThreads;
	std::vector<std::thread> myThreads;
	//std::condition_variable_any mySignal;
	//std::stop_source myStopSource;
	std::counting_semaphore<> mySignal;
	std::atomic_bool myStopSource;
	ConcurrentQueue<Task> myQueue;
};

#include "concurrency-utils.inl"
