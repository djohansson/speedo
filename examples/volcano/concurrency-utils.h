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

	constexpr CopyableAtomic(value_t val) noexcept
		: atomic_t(val)
	{ }

	CopyableAtomic(const CopyableAtomic& other) noexcept
	{
		store(other.load(std::memory_order_acquire), std::memory_order_release);
	}
	
	CopyableAtomic& operator=(const CopyableAtomic& other) noexcept
	{
		store(other.load(std::memory_order_acquire), std::memory_order_release);

		return *this;
	}
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
	inline auto internalAtomicRef() noexcept
	{
		return std::atomic_ref(myBits);
	}
#else
	using atomic_t = CopyableAtomic<value_t>;
	atomic_t myAtomic;
	inline auto& internalAtomicRef() noexcept
	{
		return myAtomic;
	}
#endif

	template <typename Func>
	inline void internalAquireLock(Func lockFn) noexcept
	{
		auto result = lockFn();
		auto& [success, value] = result;
		while (!success)
		{
			internalAtomicRef().wait(value);
			result = lockFn();
		}
	}

public:

	// Lockable Concept
	void lock() noexcept
	{
		internalAquireLock([this]() { return try_lock(); });
	}

	// Writer is responsible for clearing up both the Upgraded and Writer bits.
	inline void unlock() noexcept
	{
		static_assert(Reader > Writer + Upgraded, "wrong bits!");

		internalAtomicRef().fetch_and(~(Writer | Upgraded), std::memory_order_release);
		internalAtomicRef().notify_all();
	}

	// SharedLockable Concept
	void lock_shared() noexcept
	{
		internalAquireLock([this]() { return try_lock_shared(); });
	}

	inline void unlock_shared() noexcept
	{
		internalAtomicRef().fetch_add(-Reader, std::memory_order_release);
		internalAtomicRef().notify_all();
	}

	// Downgrade the lock from writer status to reader status.
	inline void unlock_and_lock_shared() noexcept
	{
		internalAtomicRef().fetch_add(Reader, std::memory_order_acquire);

		unlock();
	}

	// UpgradeLockable Concept
	void lock_upgrade() noexcept
	{
		internalAquireLock([this]() { return try_lock_upgrade(); });
	}

	inline void unlock_upgrade() noexcept
	{
		internalAtomicRef().fetch_add(-Upgraded, std::memory_order_acq_rel);
		internalAtomicRef().notify_all();
	}

	// unlock upgrade and try to acquire write lock
	void unlock_upgrade_and_lock() noexcept
	{
		// try to unlock upgrade and write lock atomically
		internalAquireLock([this]() { return try_lock<Upgraded>(); });
	}

	// unlock upgrade and read lock atomically
	inline void unlock_upgrade_and_lock_shared() noexcept
	{
		internalAtomicRef().fetch_add(Reader - Upgraded, std::memory_order_acq_rel);
		internalAtomicRef().notify_all();
	}

	// write unlock and upgrade lock atomically
	inline void unlock_and_lock_upgrade() noexcept
	{
		// need to do it in two steps here -- as the Upgraded bit might be OR-ed at
		// the same time when other threads are trying do try_lock_upgrade().

		internalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);
		internalAtomicRef().fetch_add(-Writer, std::memory_order_release);
		internalAtomicRef().notify_all();
	}

	// Attempt to acquire writer permission. Return false if we didn't get it.
	template <value_t Expected = None>
	inline std::tuple<bool, value_t> try_lock() noexcept
	{
		auto result = std::make_tuple(false, Expected);
		auto& [success, value] = result;
		success = internalAtomicRef().compare_exchange_weak(value, Writer, std::memory_order_acq_rel);
		return result;
	}

	// Try to get reader permission on the lock. This can fail if we
	// find out someone is a writer or upgrader.
	// Setting the Upgraded bit would allow a writer-to-be to indicate
	// its intention to write and block any new readers while waiting
	// for existing readers to finish and release their read locks. This
	// helps avoid starving writers (promoted from upgraders).
	inline std::tuple<bool, value_t> try_lock_shared() noexcept
	{
		// fetch_add is considerably (100%) faster than compare_exchange,
		// so here we are optimizing for the common (lock success) case.

		value_t value = internalAtomicRef().fetch_add(Reader, std::memory_order_acquire);
		if (value & (Writer | Upgraded))
		{
			value = internalAtomicRef().fetch_add(-Reader, std::memory_order_release);
			return std::make_tuple(false, value);
		}
		return std::make_tuple(true, value);
	}

	// try to acquire an upgradable lock.
	inline std::tuple<bool, value_t> try_lock_upgrade() noexcept
	{
		value_t value = internalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);

		// Note: when failed, we cannot flip the Upgraded bit back,
		// as in this case there is either another upgrade lock or a write lock.
		// If it's a write lock, the bit will get cleared up when that lock's done
		// with unlock().
		return std::make_tuple(((value & (Upgraded | Writer)) == 0), value);
	}
};

template <typename T>
struct Future : Noncopyable
{
public:

	using value_t = std::conditional_t<std::is_void_v<T>, std::nullptr_t, T>;
	using state_t = std::tuple<value_t, std::atomic_bool>;

	constexpr Future() = default;

	Future(std::shared_ptr<state_t>&& state)
		: myState(std::forward<std::shared_ptr<state_t>>(state))
	{ }
	
	Future(Future&& other)
		: myState(std::exchange(other.myState, {}))
	{ }

	Future& operator=(Future&& other)
	{
		myState = std::exchange(other.myState, {});

		return *this;
	}

	inline value_t& get()
	{
		assert(valid());
		auto& [value, flag] = *myState;
		
		if (!is_ready())
			flag.wait(false, std::memory_order_acquire);

		return value;
	}

	inline bool valid() const { return !!myState; }

	inline bool is_ready() const
	{
		assert(valid());
		auto& [value, flag] = *myState;

		return flag.load(std::memory_order_acquire);
	}

	inline void reset() { myState.reset(); } // potential race condition? std::atomic<std::shared_ptr<state_t>> required?
	
private:

	std::shared_ptr<state_t> myState;
};

// todo: continuations
// todo: return/arg pipes
// todo: fork/join
class Task : public Noncopyable
{
	static constexpr size_t kMaxCallableSizeBytes = 56;
	static constexpr size_t kMaxArgsSizeBytes = 24;

public:

	constexpr Task() {};

	template <
		typename F,
		typename dF = std::decay_t<F>,
		typename... Args,
		typename ArgsTuple = std::tuple<Args...>,
		typename R = std::invoke_result_t<dF, Args...>,
		typename RState = typename Future<R>::state_t
	>
	Task(F&& f, Args&&... args)
		: myInvokeThunk([](void* callablePtr, void* argsPtr, void* returnPtr)
		{
			auto& callable = *static_cast<dF*>(callablePtr);
			auto& args = *static_cast<ArgsTuple*>(argsPtr);
			auto& [value, flag] = *static_cast<RState*>(returnPtr);
			
			if constexpr (std::is_void_v<R>)
				std::apply(callable, args);
			else
				value = std::apply(callable, args);
			
			flag.store(true, std::memory_order_release);
			flag.notify_all();
		})
		, myMoveThunk([](
			void* callablePtr, void* otherCallablePtr,
			void* argsPtr, void* otherArgsPtr)
		{
			std::construct_at(static_cast<dF*>(callablePtr), std::move(*static_cast<dF*>(otherCallablePtr)));
			std::destroy_at(static_cast<dF*>(otherCallablePtr));
			std::construct_at(static_cast<ArgsTuple*>(argsPtr), std::move(*static_cast<ArgsTuple*>(otherArgsPtr)));
			std::destroy_at(static_cast<ArgsTuple*>(otherArgsPtr));
		})
		, myDeleteThunk([](void* callablePtr, void* argsPtr)
		{
			std::destroy_at(static_cast<dF*>(callablePtr));
			std::destroy_at(static_cast<ArgsTuple*>(argsPtr));
		})
	{	
		static_assert(sizeof(dF) <= kMaxCallableSizeBytes);
		std::construct_at(myCallableMemory.data(), std::forward<F>(f));
		new () dF();

		static_assert(sizeof(ArgsTuple) <= kMaxArgsSizeBytes);
		new (myArgsMemory.data()) ArgsTuple(std::forward<Args>(args)...);

		myReturnState = std::make_shared<RState>();
	}

	Task(Task&& other) noexcept	{ *this = std::move(other);	}

	~Task()
	{
		if (myDeleteThunk)
			myDeleteThunk(myCallableMemory.data(), myArgsMemory.data());
	}

	Task& operator=(Task&& other) noexcept
	{
		if (myDeleteThunk)
			myDeleteThunk(myCallableMemory.data(), myArgsMemory.data());
		
		myInvokeThunk = std::exchange(other.myInvokeThunk, nullptr);
		myReturnState = std::exchange(other.myReturnState, {});
		myMoveThunk = std::exchange(other.myMoveThunk, nullptr);
		myDeleteThunk = std::exchange(other.myDeleteThunk, nullptr);

		if (myMoveThunk)
			myMoveThunk(
				myCallableMemory.data(), other.myCallableMemory.data(),
				myArgsMemory.data(), other.myArgsMemory.data());

		return *this;
	}

	inline void operator()()
	{
		assert(myInvokeThunk);
		
		myInvokeThunk(myCallableMemory.data(), myArgsMemory.data(), myReturnState.get());
	}

	template <typename R>
	inline Future<R> getFuture()
	{
		return Future<R>(std::static_pointer_cast<typename Future<R>::state_t>(myReturnState));
	}

private:

	// cache line 1
	void (*myInvokeThunk)(void*, void*, void*) = nullptr;
	alignas(intptr_t) AlignedArray<kMaxCallableSizeBytes> myCallableMemory;
	// cache line 2
	alignas(intptr_t) AlignedArray<kMaxArgsSizeBytes> myArgsMemory;
	std::shared_ptr<void> myReturnState;
	void (*myMoveThunk)(void*, void*, void*, void*) = nullptr;
	void (*myDeleteThunk)(void*, void*) = nullptr;
};

class TaskThreadPool : public Noncopyable
{
public:

	TaskThreadPool(uint32_t threadCount)
		: mySignal(threadCount)
	{
		ZoneScopedN("TaskThreadPool()");

		assertf(threadCount > 0, "Thread count must be nonzero");

		auto threadMain = [this]
		{
			//auto stopToken = myStopSource.get_token();

			internalThreadMain(/*stopToken*/);
		};
		
		myThreads.reserve(threadCount);
		for (uint32_t threadIt = 0; threadIt < threadCount; threadIt++)
			myThreads.emplace_back(threadMain);
	}

	~TaskThreadPool()
	{
		ZoneScopedN("~TaskThreadPool()");

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
			ZoneScopedN("~TaskThreadPool()::join");

			for (auto& thread : myThreads)
				thread.join();
		}
	}

	template <typename F, typename... Args, typename R = std::invoke_result_t<F, Args...>>
	Future<R> submit(F&& callable, Args&&... args)
	{
		ZoneScopedN("TaskThreadPool::submit");

		Task task(std::forward<F>(callable), std::forward<Args>(args)...);
		auto future = task.template getFuture<R>();

		{
			ZoneScopedN("TaskThreadPool::submit::enqueue");

			myQueue.enqueue(std::move(task));
		}

		{
			ZoneScopedN("TaskThreadPool::submit::signal");
		
			//mySignal.notify_one();
			mySignal.release();
		}

		return future;
	}

	void processQueue()
	{
		ZoneScopedN("TaskThreadPool::processQueue");

		internalProcessQueue();
	}

	template <typename R>
	std::optional<typename Future<R>::value_t> processQueueUntil(Future<R>&& future)
	{
		ZoneScopedN("TaskThreadPool::processQueueUntil");

		return internalProcessQueue(std::forward<Future<R>>(future));
	}

private:

	void internalProcessQueue()
	{
		Task task;
		while (myQueue.try_dequeue(task))
			task();
	}

	template <typename R>
	std::optional<typename Future<R>::value_t> internalProcessQueue(Future<R>&& future)
	{
		if (!future.valid())
			return std::nullopt;

		if (!future.is_ready())
		{
			Task task;
			while (myQueue.try_dequeue(task))
			{
				task();

				if (future.is_ready())
					break;
			}
		}

		auto retval = std::make_optional(future.get());
		future.reset();

		return retval;
	}

	void internalThreadMain(/*std::stop_token& stopToken*/)
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

	//std::shared_mutex myMutex;
	//std::vector<std::jthread> myThreads;
	std::vector<std::thread> myThreads;
	//std::condition_variable_any mySignal;
	//std::stop_source myStopSource;
	std::counting_semaphore<> mySignal;
	std::atomic_bool myStopSource;
	ConcurrentQueue<Task> myQueue;
};
